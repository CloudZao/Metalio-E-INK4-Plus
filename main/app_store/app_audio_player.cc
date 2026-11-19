#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <opus.h>
#include "app_audio_player.h"
#include "app_audio_ogg.h"
#include "app_audio_wav.h"
#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "metalio_audio.h"
#include "power_policy.h"
#include "sd_access.h"
#include "sd_paths.h"
#include "standby_screen/standby_screen.h"

static constexpr const char* kAudioTag = "AppAudio";
static constexpr uint32_t kAudioRate = 16000;
static constexpr size_t kAudioStackBytes = 64 * 1024;
static constexpr size_t kShortAudioCacheBytes = 256 * 1024;
// 与 AudioService 的解码和输出同级，屏幕扫描供数仍使用更高优先级。
static constexpr UBaseType_t kAudioPriority = configMAX_PRIORITIES - 2;

struct AppAudioPlayer {
    AppStore::Item item; // 仅保存定位资源所需的安装信息
    std::string path; // 已复制的 SD 相对文件路径
    uint32_t source = XZ_APP_AUDIO_ASSET; // 音频来源
    uint32_t offset = 0; // 资源区域起始字节
    uint32_t length = 0; // 资源区域长度
    uint32_t format = XZ_APP_AUDIO_OGG_OPUS; // 音频编码格式
    bool loop = false; // 循环播放
    std::atomic<bool> cancelled {false}; // 取消后后台自行收尾
    std::atomic<bool> paused {false}; // 应用主动暂停
    std::atomic<bool> foreground {false}; // 宿主前台与推白状态
    std::atomic<bool> running {false}; // 后台任务仍存活
    std::mutex mutex; // 只保护短小的状态快照
    xz_app_audio_status_t status {sizeof(xz_app_audio_status_t), XZ_APP_AUDIO_STOPPED, XZ_APP_OK, 0, 0, 0, 0}; // 查询结果
};

class AppAudioFile {
public:
    AppAudioFile() = default;
    AppAudioFile(const AppAudioFile&) = delete;
    AppAudioFile& operator=(const AppAudioFile&) = delete;

    ~AppAudioFile() {
        Close();
        heap_caps_free(cache_);
    }

    bool Open() {
        if (file_) return true;
        sd_access::Transaction transaction;
        if (!transaction) return false;
        FILE* file = std::fopen(path.c_str(), "rb");
        if (!file) return false;
        struct stat info {};
        const bool valid = fstat(fileno(file), &info) == 0 && S_ISREG(info.st_mode) &&
            info.st_size > 0 && info.st_size <= LONG_MAX;
        const uint64_t size = valid ? static_cast<uint64_t>(info.st_size) : 0;
        // 待机重挂载会改变挂载代次，恢复时按文件属性与音频流头核对来源。
        if (!valid || base > size || (length && length > size - base) ||
            (opened_ && (size != file_size_ || info.st_mtime != modified_))) {
            std::fclose(file);
            return false;
        }
        if (!length) length = size - base;
        std::array<uint8_t, 64> header {};
        const size_t header_size = static_cast<size_t>(std::min<uint64_t>(header.size(), length));
        if (!header_size || std::fseek(file, static_cast<long>(base), SEEK_SET) != 0 ||
            std::fread(header.data(), 1, header_size, file) != header_size ||
            (opened_ && header != header_)) {
            std::fclose(file);
            return false;
        }
        // 短循环音频预读到有上限的 PSRAM 缓存，播放期间不受 SD 读延迟影响。
        if (!cache_attempted_ && length <= kShortAudioCacheBytes) {
            cache_attempted_ = true;
            auto* buffer = static_cast<uint8_t*>(heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!buffer) {
                ESP_LOGW(kAudioTag, "operation=play stage=cache_allocate result=unavailable fallback=stream bytes=%u",
                         static_cast<unsigned>(length));
            } else if (std::fseek(file, static_cast<long>(base), SEEK_SET) != 0 ||
                       std::fread(buffer, 1, length, file) != length) {
                heap_caps_free(buffer);
                std::fclose(file);
                cache_attempted_ = false;
                return false;
            } else {
                cache_ = buffer;
            }
        }
        file_ = file;
        file_size_ = size;
        modified_ = info.st_mtime;
        header_ = header;
        opened_ = true;
        return true;
    }

    void Close() {
        if (!file_) return;
        if (std::fclose(file_) != 0)
            ESP_LOGE(kAudioTag, "operation=play stage=file_close result=failed errno=%d", errno);
        file_ = nullptr;
    }

    static bool Read(void* context, uint64_t position, void* buffer, size_t bytes) {
        auto* source = static_cast<AppAudioFile*>(context);
        if (!source->file_ || position > source->length || bytes > source->length - position ||
            source->base + position > LONG_MAX) return false;
        if (source->cache_) {
            std::memcpy(buffer, source->cache_ + position, bytes);
            return true;
        }
        return std::fseek(source->file_, static_cast<long>(source->base + position), SEEK_SET) == 0 &&
            std::fread(buffer, 1, bytes, source->file_) == bytes;
    }

    std::string path; // 宿主文件位置，不写入日志
    uint64_t base = 0; // 音频区域起始字节
    uint64_t length = 0; // 有界区域长度

private:
    FILE* file_ = nullptr; // 仅后台任务访问
    uint64_t file_size_ = 0; // 暂停后重开时检查长度变化
    int64_t modified_ = 0; // 暂停后重开时检查修改时间
    std::array<uint8_t, 64> header_ {}; // 音频区域头部，检查是否仍为同一逻辑流
    bool opened_ = false; // 已登记文件身份
    uint8_t* cache_ = nullptr; // 至多 256KiB 的压缩音频快照，仅当前会话持有
    bool cache_attempted_ = false; // 分配失败后沿用流式读取，避免反复申请与刷日志
};

class AppAudioOutput {
public:
    explicit AppAudioOutput(AppAudioPlayer* owner) : owner_(owner) {}
    ~AppAudioOutput() { Release(); }

    bool Acquire() {
        if (acquired_) return true;
        std::atomic<int> result {0};
        const bool queued = Application::GetInstance().Schedule([owner = owner_, &result] {
            const bool acquired = Application::GetInstance().AcquireLocalPlayback(owner);
            result.store(acquired ? 1 : -1);
        }, Application::TaskDomain::Audio);
        if (!queued) return false;
        // codec 启动会读取 NVS，必须在内部 RAM 栈执行。
        // 投递成功后必须等回调完成，不能因暂停或取消提前销毁结果与会话。
        while (result.load() == 0) vTaskDelay(pdMS_TO_TICKS(10));
        if (result.load() != 1) return false;
        acquired_ = true;
        power_.emplace(PowerNeed::LocalPlayback);
        awake_.emplace(PowerNeed::StandbyInhibit);
        return true;
    }

    void Release() {
        if (acquired_) Application::GetInstance().ReleaseLocalPlayback(owner_);
        acquired_ = false;
        power_.reset();
        awake_.reset();
    }

private:
    AppAudioPlayer* owner_; // 宿主会话身份，不指向 ELF
    bool acquired_ = false; // 是否申请过硬件归属
    std::optional<PowerNeedHold> power_; // 保持功放，不申请联网
    std::optional<PowerNeedHold> awake_; // 播放期间禁止无操作待机
};

static void AppAudioState(AppAudioPlayer& player, uint32_t state, int result = XZ_APP_OK) {
    std::lock_guard<std::mutex> lock(player.mutex);
    if (player.cancelled.load()) return;
    player.status.state = state;
    player.status.result = result;
}

static void AppAudioFailure(AppAudioPlayer& player, const char* stage, int result) {
    if (player.cancelled.load()) return;
    AppAudioState(player, XZ_APP_AUDIO_ERROR, result);
    if (result == XZ_APP_ERR_BUSY || result == XZ_APP_ERR_NOT_READY) {
        ESP_LOGW(kAudioTag, "operation=play stage=%s result=unavailable code=%d", stage, result);
    } else if (result != XZ_APP_ERR_UNSUPPORTED) {
        ESP_LOGE(kAudioTag, "operation=play stage=%s result=failed code=%d errno=%d", stage, result, errno);
    }
}

static bool AppAudioRelativePath(const char* path, std::string& result) {
    if (!path) return false;
    const size_t length = strnlen(path, 241);
    if (!length || length > 240) return false;
    result.assign(path, length);
    size_t start = 0;
    while (start < result.size()) {
        const size_t end = result.find('/', start);
        const std::string part = result.substr(start, end == std::string::npos ? end : end - start);
        if (part == "." || part == ".." || !SdIsValidFileName(part.c_str())) return false;
        if (end == std::string::npos) return true;
        start = end + 1;
    }
    return false;
}

static int AppAudioCopy(void* target, uint32_t capacity, const void* source, size_t bytes) {
    if (!target || capacity < sizeof(uint32_t)) return XZ_APP_ERR_INVALID;
    std::memcpy(target, source, std::min<size_t>(capacity, bytes));
    return XZ_APP_OK;
}

static bool AppAudioWanted(const AppAudioPlayer& player) {
    return !player.cancelled.load() && !player.paused.load() && player.foreground.load() &&
        !sd_access::IsQuiescing() && !StandbyScreen::IsActive() && !StandbyScreen::IsResuming();
}

static bool AppAudioWaitForeground(AppAudioPlayer& player, AppAudioFile& file, AppAudioOutput& output) {
    while (!player.cancelled.load()) {
        if (AppAudioWanted(player)) return true;
        output.Release();
        file.Close();
        AppAudioState(player, XZ_APP_AUDIO_PAUSED);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}


static bool AppAudioPrepareOutput(AppAudioPlayer& player, AppAudioFile& file, AppAudioOutput& output) {
    while (AppAudioWaitForeground(player, file, output)) {
        if (!file.Open()) {
            if (!AppAudioWanted(player)) continue;
            AppAudioFailure(player, "file_reopen", XZ_APP_ERR_IO);
            return false;
        }
        if (!MetalioAudio_ModeReady()) {
            AppAudioState(player, XZ_APP_AUDIO_LOADING);
            if (!MetalioAudio_WaitModeReady(kMetalioAudioInitWaitMs, [&player] {
                return AppAudioWanted(player);
            })) {
                if (!AppAudioWanted(player)) continue;
                AppAudioFailure(player, "audio_mode", XZ_APP_ERR_NOT_READY);
                return false;
            }
        }
        // 前一个会话停止后可能仍在归还硬件，只等待一个短收尾窗口。
        const int64_t until = esp_timer_get_time() + 500000;
        bool acquired = false;
        while (AppAudioWanted(player)) {
            acquired = output.Acquire();
            if (acquired || esp_timer_get_time() >= until) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!AppAudioWanted(player)) continue;
        if (!acquired) {
            AppAudioFailure(player, "audio_acquire", XZ_APP_ERR_BUSY);
            return false;
        }
        return true;
    }
    return false;
}

static void AppAudioRunWav(AppAudioPlayer& player, AppAudioFile& file, AppAudioOutput& output) {
    AppAudioWav wav;
    const int result = AppWavOpen(AppAudioFile::Read, &file, file.length, wav);
    if (result != XZ_APP_OK) { AppAudioFailure(player, "wav_header", result); return; }
    {
        std::lock_guard<std::mutex> lock(player.mutex);
        player.status.duration_ms = wav.bytes / 32;
    }
    std::vector<int16_t> pcm;
    uint32_t position = 0;
    while (AppAudioPrepareOutput(player, file, output)) {
        if (position == wav.bytes) {
            {
                std::lock_guard<std::mutex> lock(player.mutex);
                ++player.status.loops;
                player.status.position_ms = player.status.duration_ms;
            }
            if (!player.loop) { AppAudioState(player, XZ_APP_AUDIO_ENDED); break; }
            position = 0;
        }
        const size_t bytes = std::min<uint32_t>(1280, wav.bytes - position);
        pcm.resize(bytes / sizeof(int16_t));
        if (!AppAudioFile::Read(&file, wav.offset + position, pcm.data(), bytes)) {
            AppAudioFailure(player, "wav_read", XZ_APP_ERR_IO);
            break;
        }
        bool written = false;
        while (AppAudioPrepareOutput(player, file, output)) {
            if (Application::GetInstance().WriteLocalPlayback(&player, pcm)) { written = true; break; }
            if (!AppAudioWanted(player)) continue;
            AppAudioFailure(player, "wav_output", XZ_APP_ERR_BUSY);
            break;
        }
        if (!written) break;
        position += bytes;
        {
            std::lock_guard<std::mutex> lock(player.mutex);
            player.status.position_ms = position / 32;
        }
        AppAudioState(player, XZ_APP_AUDIO_PLAYING);
    }
}

static void AppAudioRun(const std::shared_ptr<AppAudioPlayer>& player) {
    AppAudioFile file;
    AppAudioOutput output(player.get());
    if (!AppAudioWaitForeground(*player, file, output)) return;
    if (player->source == XZ_APP_AUDIO_ASSET) {
        size_t base = 0, length = 0;
        if (!AppStore::LocateAsset(player->item, file.path, base, length)) {
            AppAudioFailure(*player, "asset", XZ_APP_ERR_IO);
            return;
        }
        if (player->offset >= length || (player->length && player->length > length - player->offset)) {
            AppAudioFailure(*player, "asset_bounds", XZ_APP_ERR_INVALID);
            return;
        }
        file.base = base + player->offset;
        file.length = player->length ? player->length : length - player->offset;
    } else {
        file.path = std::string(SD_APP_ROOT) + "/" + player->path;
    }
    if (!file.Open()) {
        AppAudioFailure(*player, "file_open", XZ_APP_ERR_IO);
        return;
    }
    if (player->format == XZ_APP_AUDIO_WAV_PCM) { AppAudioRunWav(*player, file, output); return; }
    AppAudioOggReader reader;
    int result = reader.Open(AppAudioFile::Read, &file, file.length);
    if (result != XZ_APP_OK) {
        AppAudioFailure(*player, "ogg_header", result);
        return;
    }
    int opus_error = OPUS_OK;
    std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> decoder(
        opus_decoder_create(kAudioRate, 1, &opus_error), opus_decoder_destroy);
    if (!decoder || opus_error != OPUS_OK) {
        AppAudioFailure(*player, "decoder_create", XZ_APP_ERR_MEMORY);
        return;
    }
    if (opus_decoder_ctl(decoder.get(), OPUS_SET_GAIN(reader.Gain())) != OPUS_OK) {
        AppAudioFailure(*player, "decoder_gain", XZ_APP_ERR_UNSUPPORTED);
        return;
    }
    const uint64_t duration = reader.Duration48k();
    {
        std::lock_guard<std::mutex> lock(player->mutex);
        player->status.duration_ms = duration / 48;
    }
    std::vector<int16_t> pcm(kAudioRate * 120 / 1000);
    uint64_t decoded_48k = 0;
    uint64_t played_samples = 0;
    ESP_LOGI(kAudioTag, "operation=play result=started source=%u loop=%d", player->source, player->loop);
    while (!player->cancelled.load()) {
        if (!AppAudioPrepareOutput(*player, file, output)) break;
        result = reader.Next();
        if (result == 1) {
            if (decoded_48k < reader.PreSkip() + duration ||
                reader.FinalGranule() != reader.PreSkip() + duration) {
                AppAudioFailure(*player, "ogg_duration", XZ_APP_ERR_INVALID);
                break;
            }
            {
                std::lock_guard<std::mutex> lock(player->mutex);
                ++player->status.loops;
                player->status.position_ms = player->status.duration_ms;
            }
            if (!player->loop) {
                AppAudioState(*player, XZ_APP_AUDIO_ENDED);
                break;
            }
            result = reader.Open(AppAudioFile::Read, &file, file.length, duration);
            if (result != XZ_APP_OK || opus_decoder_ctl(decoder.get(), OPUS_RESET_STATE) != OPUS_OK ||
                opus_decoder_ctl(decoder.get(), OPUS_SET_GAIN(reader.Gain())) != OPUS_OK) {
                AppAudioFailure(*player, "loop_restart", result == XZ_APP_OK ? XZ_APP_ERR_IO : result);
                break;
            }
            decoded_48k = played_samples = 0;
            continue;
        }
        if (result != XZ_APP_OK) {
            AppAudioFailure(*player, "ogg_packet", result);
            break;
        }
        pcm.resize(kAudioRate * 120 / 1000);
        const auto& packet = reader.Packet();
        const int samples = opus_decode(decoder.get(), packet.data(), packet.size(), pcm.data(), pcm.size(), 0);
        if (samples <= 0) {
            AppAudioFailure(*player, "opus_decode", XZ_APP_ERR_INVALID);
            break;
        }
        const uint64_t begin = decoded_48k;
        decoded_48k += uint64_t(samples) * 3;
        const uint64_t keep_begin = std::max<uint64_t>(begin, reader.PreSkip());
        const uint64_t keep_end = std::min<uint64_t>(decoded_48k, reader.PreSkip() + duration);
        if (keep_end <= keep_begin) continue;
        const size_t skip = static_cast<size_t>((keep_begin - begin) / 3);
        const size_t keep = static_cast<size_t>((keep_end - begin) / 3) - skip;
        if (!keep) continue;
        std::memmove(pcm.data(), pcm.data() + skip, keep * sizeof(int16_t));
        pcm.resize(keep);
        bool written = false;
        while (AppAudioPrepareOutput(*player, file, output)) {
            if (Application::GetInstance().WriteLocalPlayback(player.get(), pcm)) {
                written = true;
                break;
            }
            if (!AppAudioWanted(*player)) continue;
            AppAudioFailure(*player, "output", Application::GetInstance().HasLocalPlaybackWork() ? XZ_APP_ERR_IO : XZ_APP_ERR_BUSY);
            break;
        }
        if (!written) break;
        played_samples += keep;
        {
            std::lock_guard<std::mutex> lock(player->mutex);
            player->status.position_ms = played_samples * 1000 / kAudioRate;
        }
        AppAudioState(*player, XZ_APP_AUDIO_PLAYING);
    }
    ESP_LOGI(kAudioTag, "operation=play result=finished");
}

static void AppAudioTask(void* context) {
    auto* holder = static_cast<std::shared_ptr<AppAudioPlayer>*>(context);
    auto player = std::move(*holder);
    delete holder;
    AppAudioRun(player);
    player->running.store(false);
    player.reset();
    vTaskDeleteWithCaps(nullptr);
}

std::shared_ptr<AppAudioPlayer> AppAudioCreate(const AppStore::Item& item) {
    auto player = std::shared_ptr<AppAudioPlayer>(new(std::nothrow) AppAudioPlayer);
    if (player) {
        player->item.app_id = item.app_id;
        player->item.installed_name = item.installed_name;
        player->item.name = item.name;
        player->item.size = item.size;
    }
    return player;
}

int AppAudioGetCaps(xz_app_audio_caps_t* result) {
    if (!result) return XZ_APP_ERR_INVALID;
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (!codec || codec->output_sample_rate() != kAudioRate || codec->output_channels() != 1)
        return XZ_APP_ERR_UNSUPPORTED;
    const xz_app_audio_caps_t caps {sizeof(caps), XZ_APP_AUDIO_OGG_OPUS | XZ_APP_AUDIO_WAV_PCM,
        XZ_APP_AUDIO_ASSET | XZ_APP_AUDIO_SD, kAudioRate, 1};
    return AppAudioCopy(result, result->struct_size, &caps, sizeof(caps));
}

int AppAudioPlay(std::shared_ptr<AppAudioPlayer>& player, const xz_app_audio_config_t* config) {
    if (!player) return XZ_APP_ERR_MEMORY;
    if (!config || config->struct_size < sizeof(*config)) return XZ_APP_ERR_INVALID;
    xz_app_audio_caps_t caps {};
    caps.struct_size = sizeof(caps);
    const int available = AppAudioGetCaps(&caps);
    if (available != XZ_APP_OK) return available;
    if (config->format != XZ_APP_AUDIO_OGG_OPUS && config->format != XZ_APP_AUDIO_WAV_PCM) return XZ_APP_ERR_UNSUPPORTED;
    if (config->source != XZ_APP_AUDIO_ASSET && config->source != XZ_APP_AUDIO_SD) return XZ_APP_ERR_INVALID;
    auto next = AppAudioCreate(player->item);
    if (!next) return XZ_APP_ERR_MEMORY;
    if (config->source == XZ_APP_AUDIO_SD &&
        (config->offset || config->length || !AppAudioRelativePath(config->path, next->path))) return XZ_APP_ERR_INVALID;
    if (config->source == XZ_APP_AUDIO_ASSET && config->path) return XZ_APP_ERR_INVALID;
    next->format = config->format;
    next->source = config->source;
    next->offset = config->offset;
    next->length = config->length;
    next->loop = config->loop != 0;
    next->foreground.store(player->foreground.load());
    next->status.state = XZ_APP_AUDIO_LOADING;
    auto* holder = new(std::nothrow) std::shared_ptr<AppAudioPlayer>(next);
    if (!holder) return XZ_APP_ERR_MEMORY;
    next->running.store(true);
    AppAudioStop(player);
    if (xTaskCreateWithCaps(AppAudioTask, "app_audio", kAudioStackBytes, holder, kAudioPriority, nullptr,
                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        delete holder;
        ESP_LOGE(kAudioTag, "operation=play stage=task_create result=failed");
        return XZ_APP_ERR_MEMORY;
    }
    player = std::move(next);
    return XZ_APP_OK;
}

int AppAudioPause(const std::shared_ptr<AppAudioPlayer>& player, bool paused) {
    if (!player || !player->running.load() || player->cancelled.load()) return XZ_APP_ERR_NOT_READY;
    player->paused.store(paused);
    return XZ_APP_OK;
}

int AppAudioStop(const std::shared_ptr<AppAudioPlayer>& player) {
    if (!player) return XZ_APP_OK;
    player->cancelled.store(true);
    std::lock_guard<std::mutex> lock(player->mutex);
    player->status.state = XZ_APP_AUDIO_STOPPED;
    player->status.result = XZ_APP_OK;
    return XZ_APP_OK;
}

void AppAudioSetForeground(const std::shared_ptr<AppAudioPlayer>& player, bool foreground) {
    if (player) player->foreground.store(foreground);
}

int AppAudioSetVolume(uint32_t volume) {
    if (volume > 100) return XZ_APP_ERR_INVALID;
    const bool queued = Application::GetInstance().Schedule([volume] {
        if (auto* codec = Board::GetInstance().GetAudioCodec()) codec->SetOutputVolume(volume);
    }, Application::TaskDomain::Storage);
    return queued ? XZ_APP_OK : XZ_APP_ERR_BUSY;
}

int AppAudioGetStatus(const std::shared_ptr<AppAudioPlayer>& player, xz_app_audio_status_t* result) {
    if (!player || !result) return XZ_APP_ERR_INVALID;
    xz_app_audio_status_t status;
    {
        std::lock_guard<std::mutex> lock(player->mutex);
        status = player->status;
    }
    if (auto* codec = Board::GetInstance().GetAudioCodec()) status.volume = codec->output_volume();
    return AppAudioCopy(result, result->struct_size, &status, sizeof(status));
}

bool AppAudioBusy(const std::shared_ptr<AppAudioPlayer>& player) {
    return player && player->running.load();
}
