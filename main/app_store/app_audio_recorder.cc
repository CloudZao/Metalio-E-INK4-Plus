#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include "app_audio_recorder.h"
#include "app_audio_wav.h"
#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "metalio_audio.h"
#include "power_policy.h"
#include "reader/http_download_file.h"
#include "sd_access.h"
#include "sd_paths.h"
#include "standby_screen/standby_screen.h"

static constexpr const char* kRecordTag = "AppRecord";

struct AppAudioRecorder {
    std::string directory; // 已校验的应用私有数据目录
    std::string pending; // 对应工作目录中的临时文件
    std::string relative; // 产品 SD 根目录下的正式文件相对路径
    std::atomic<bool> closed {false}; // 关闭后不再接纳请求，后台完成收尾
    std::atomic<bool> foreground {false}; // 宿主前台状态
    std::atomic<bool> busy {true}; // 包含启动时的旧文件探测
    std::atomic<bool> start {false}; // UI 提交的启动请求
    std::atomic<bool> stop {false}; // 停止并保存请求
    std::atomic<bool> paused {false}; // 暂停写入
    std::mutex mutex; // 保护状态快照
    xz_app_record_status_t status {}; // 不持有应用内存
};

static void AppRecordState(AppAudioRecorder& r, uint32_t state, int result = XZ_APP_OK) {
    std::lock_guard<std::mutex> lock(r.mutex);
    r.status.state = state;
    r.status.result = result;
}

static bool AppRecordVisible(const AppAudioRecorder& r) {
    return !r.closed.load() && r.foreground.load() && !sd_access::IsQuiescing() &&
        !StandbyScreen::IsActive() && !StandbyScreen::IsResuming();
}

static bool AppRecordDirectory(const std::string& path) {
    if (mkdir(path.c_str(), 0755) == 0) return true;
    struct stat st {};
    return errno == EEXIST && stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

struct AppRecordFileSource {
    FILE* file; // 当前任务持有的文件
};
static bool AppRecordRead(void* context, uint64_t offset, void* data, size_t size) {
    auto* source = static_cast<AppRecordFileSource*>(context);
    return offset <= LONG_MAX && fseek(source->file, static_cast<long>(offset), SEEK_SET) == 0 &&
        fread(data, 1, size, source->file) == size;
}

static void AppRecordProbe(AppAudioRecorder& r) {
    sd_access::Transaction transaction;
    if (!transaction) {
        ESP_LOGW(kRecordTag, "operation=probe stage=sd_access result=unavailable");
        AppRecordState(r, XZ_APP_RECORD_ERROR, XZ_APP_ERR_NOT_READY);
        return;
    }
    const std::string path = std::string(SD_APP_ROOT) + "/" + r.relative;
    struct stat st {};
    const std::string backup = path + ".cloud-backup";
    // 沿用现有覆盖事务的恢复备份；目标存在时不擅自删除备份。
    if (stat(path.c_str(), &st) != 0 && errno == ENOENT && stat(backup.c_str(), &st) == 0 &&
        rename(backup.c_str(), path.c_str()) != 0) {
        ESP_LOGE(kRecordTag, "operation=probe stage=restore result=failed errno=%d", errno);
    }
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        if (errno != ENOENT) {
            ESP_LOGE(kRecordTag, "operation=probe stage=open result=failed errno=%d", errno);
            AppRecordState(r, XZ_APP_RECORD_ERROR, XZ_APP_ERR_IO);
        }
        if (errno == ENOENT) AppRecordState(r, XZ_APP_RECORD_IDLE);
        return;
    }
    AppRecordFileSource source {file};
    AppAudioWav wav;
    const int result = fstat(fileno(file), &st) == 0 && S_ISREG(st.st_mode) && st.st_size <= LONG_MAX ?
        AppWavOpen(AppRecordRead, &source, st.st_size, wav) : XZ_APP_ERR_IO;
    const bool closed = fclose(file) == 0;
    if (result != XZ_APP_OK || !closed) {
        ESP_LOGE(kRecordTag, "operation=probe stage=wav result=failed code=%d errno=%d", result, errno);
        AppRecordState(r, XZ_APP_RECORD_ERROR, result != XZ_APP_OK ? result : XZ_APP_ERR_IO);
        return;
    }
    std::lock_guard<std::mutex> lock(r.mutex);
    r.status.state = XZ_APP_RECORD_SAVED;
    r.status.saved_duration_ms = wav.bytes / 32;
    std::memcpy(r.status.path, r.relative.c_str(), r.relative.size() + 1);
}

static int AppRecordAcquire(AppAudioRecorder& r) {
    if (!MetalioAudio_WaitModeReady(kMetalioAudioInitWaitMs, [&r] {
        return AppRecordVisible(r) && !r.stop.load();
    })) return XZ_APP_ERR_NOT_READY;
    std::atomic<int> result {0};
    const bool queued = Application::GetInstance().Schedule([&r, &result] {
        result.store(Application::GetInstance().AcquireLocalRecording(&r) ? 1 : -1);
    }, Application::TaskDomain::Audio);
    if (!queued) return XZ_APP_ERR_BUSY;
    // NVS 读取必须使用内部 RAM 栈；投递后等待回调完成，防止悬空引用。
    while (!result.load()) vTaskDelay(pdMS_TO_TICKS(10));
    return result.load() == 1 ? XZ_APP_OK : XZ_APP_ERR_BUSY;
}

static int AppRecordTake(AppAudioRecorder& r) {
    sd_access::Transaction transaction;
    if (!transaction) return XZ_APP_ERR_NOT_READY;
    if (!AppRecordDirectory(SD_PATH_SETTINGS) || !AppRecordDirectory(SD_PATH_APP_STATE) ||
        !AppRecordDirectory(r.directory) || !AppRecordDirectory(SD_PATH_TEMP) ||
        !AppRecordDirectory(SD_PATH_APP_STORE_TEMP) ||
        !AppRecordDirectory(r.pending.substr(0, r.pending.find_last_of('/')))) {
        ESP_LOGE(kRecordTag, "operation=record stage=directory result=failed errno=%d", errno);
        return XZ_APP_ERR_IO;
    }
    int result = AppRecordAcquire(r);
    if (result != XZ_APP_OK) return result;
    std::optional<PowerNeedHold> awake;
    awake.emplace(PowerNeed::StandbyInhibit);
    FILE* file = fopen(r.pending.c_str(), "wb");
    uint32_t bytes = 0;
    uint8_t header[44];
    AppWavHeader(header, 0);
    if (!file || fwrite(header, 1, sizeof(header), file) != sizeof(header)) {
        ESP_LOGE(kRecordTag, "operation=record stage=open_header result=failed errno=%d", errno);
        result = XZ_APP_ERR_IO;
    }
    std::vector<int16_t> pcm;
    while (result == XZ_APP_OK && AppRecordVisible(r) && !r.stop.load()) {
        if (!Application::GetInstance().ReadLocalRecording(&r, pcm)) {
            if (AppRecordVisible(r) && !r.stop.load()) result = XZ_APP_ERR_BUSY;
            break;
        }
        const bool paused = r.paused.load();
        AppRecordState(r, paused ? XZ_APP_RECORD_PAUSED : XZ_APP_RECORD_RECORDING);
        if (paused) continue;
        if (bytes > static_cast<uint32_t>(LONG_MAX - 44) - pcm.size() * sizeof(int16_t)) {
            result = XZ_APP_ERR_LIMIT;
            break;
        }
        if (fwrite(pcm.data(), sizeof(int16_t), pcm.size(), file) != pcm.size()) {
            ESP_LOGE(kRecordTag, "operation=record stage=pcm_write result=failed errno=%d", errno);
            result = XZ_APP_ERR_IO;
            break;
        }
        bytes += pcm.size() * sizeof(int16_t);
        std::lock_guard<std::mutex> lock(r.mutex);
        r.status.bytes = bytes;
        r.status.duration_ms = bytes / 32;
    }
    Application::GetInstance().ReleaseLocalPlayback(&r);
    awake.reset();
    AppRecordState(r, XZ_APP_RECORD_SAVING);
    if (file) {
        if ((result == XZ_APP_OK || result == XZ_APP_ERR_LIMIT) && bytes) {
            AppWavHeader(header, bytes);
            if (fseek(file, 0, SEEK_SET) != 0 || fwrite(header, 1, sizeof(header), file) != sizeof(header) ||
                fflush(file) != 0 || fsync(fileno(file)) != 0) {
                ESP_LOGE(kRecordTag, "operation=record stage=finalize result=failed errno=%d", errno);
                result = XZ_APP_ERR_IO;
            }
        }
        if (fclose(file) != 0 && result != XZ_APP_ERR_IO) {
            ESP_LOGE(kRecordTag, "operation=record stage=close result=failed errno=%d", errno);
            result = XZ_APP_ERR_IO;
        }
    }
    if ((result == XZ_APP_OK || result == XZ_APP_ERR_LIMIT) && bytes) {
        std::string error;
        if (!reader::CommitDownloadedFile(r.pending, std::string(SD_APP_ROOT) + "/" + r.relative, true, error)) {
            ESP_LOGE(kRecordTag, "operation=record stage=commit result=failed errno=%d", errno);
            result = XZ_APP_ERR_IO;
        }
        else {
            std::lock_guard<std::mutex> lock(r.mutex);
            r.status.saved_duration_ms = bytes / 32;
            std::memcpy(r.status.path, r.relative.c_str(), r.relative.size() + 1);
        }
    } else if (result == XZ_APP_OK) result = XZ_APP_ERR_NOT_READY;
    if (unlink(r.pending.c_str()) != 0 && errno != ENOENT)
        ESP_LOGW(kRecordTag, "operation=record stage=temp_cleanup result=failed errno=%d", errno);
    return result;
}

static void AppRecordTask(void* context) {
    std::unique_ptr<std::shared_ptr<AppAudioRecorder>> holder(static_cast<std::shared_ptr<AppAudioRecorder>*>(context));
    auto r = *holder;
    holder.reset();
    AppRecordProbe(*r);
    r->busy.store(false);
    while (!r->closed.load()) {
        if (!r->start.exchange(false)) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        const int result = AppRecordTake(*r);
        AppRecordState(*r, result == XZ_APP_OK ? XZ_APP_RECORD_SAVED : XZ_APP_RECORD_ERROR, result);
        if (result != XZ_APP_OK && result != XZ_APP_ERR_IO) {
            ESP_LOGW(kRecordTag, "operation=record stage=take result=unavailable code=%d", result);
        } else if (result == XZ_APP_OK) ESP_LOGI(kRecordTag, "operation=record result=saved");
        r->busy.store(false);
    }
    r.reset();
    vTaskDeleteWithCaps(nullptr);
}

std::shared_ptr<AppAudioRecorder> AppRecordCreate(const std::string& key) {
    if (key.empty()) return nullptr;
    auto r = std::shared_ptr<AppAudioRecorder>(new(std::nothrow) AppAudioRecorder);
    if (!r) return nullptr;
    r->directory = std::string(SD_PATH_APP_STATE) + "/" + key;
    r->relative = "settings/apps/" + key + "/recording.wav";
    r->pending = std::string(SD_PATH_APP_STORE_TEMP) + "/" + key + "/recording.wav.part";
    if (r->relative.size() >= sizeof(r->status.path)) return nullptr;
    r->status.struct_size = sizeof(r->status);
    r->status.state = XZ_APP_RECORD_PREPARING;
    auto* holder = new(std::nothrow) std::shared_ptr<AppAudioRecorder>(r);
    if (!holder) return nullptr;
    if (xTaskCreateWithCaps(AppRecordTask, "app_record", 16384, holder, configMAX_PRIORITIES - 2,
                           nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        delete holder;
        ESP_LOGE(kRecordTag, "operation=create stage=task result=failed");
        return nullptr;
    }
    return r;
}

int AppRecordCaps(xz_app_audio_caps_t* result) {
    if (!result || result->struct_size < sizeof(uint32_t)) return XZ_APP_ERR_INVALID;
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (!codec || codec->input_sample_rate() != 16000 ||
        (codec->input_channels() != 1 && !(codec->input_channels() == 2 && codec->input_reference()))) return XZ_APP_ERR_UNSUPPORTED;
    const xz_app_audio_caps_t caps {sizeof(caps), XZ_APP_AUDIO_WAV_PCM, XZ_APP_AUDIO_SD, 16000, 1};
    std::memcpy(result, &caps, std::min<size_t>(result->struct_size, sizeof(caps)));
    return XZ_APP_OK;
}

int AppRecordStart(const std::shared_ptr<AppAudioRecorder>& r) {
    if (!r || !AppRecordVisible(*r)) return XZ_APP_ERR_NOT_READY;
    if (r->busy.exchange(true)) return XZ_APP_ERR_BUSY;
    r->stop.store(false);
    r->paused.store(false);
    {
        std::lock_guard<std::mutex> lock(r->mutex);
        r->status.duration_ms = r->status.bytes = 0;
        r->status.state = XZ_APP_RECORD_PREPARING;
        r->status.result = XZ_APP_OK;
    }
    r->start.store(true);
    return XZ_APP_OK;
}

int AppRecordPause(const std::shared_ptr<AppAudioRecorder>& r, bool paused) {
    if (!r || !r->busy.load() || r->closed.load() || r->stop.load()) return XZ_APP_ERR_NOT_READY;
    {
        std::lock_guard<std::mutex> lock(r->mutex);
        if (r->status.state != XZ_APP_RECORD_RECORDING && r->status.state != XZ_APP_RECORD_PAUSED) return XZ_APP_ERR_NOT_READY;
    }
    r->paused.store(paused);
    return XZ_APP_OK;
}

int AppRecordStop(const std::shared_ptr<AppAudioRecorder>& r) {
    if (r) r->stop.store(true);
    return XZ_APP_OK;
}

int AppRecordStatus(const std::shared_ptr<AppAudioRecorder>& r, xz_app_record_status_t* result) {
    if (!r || !result || result->struct_size < sizeof(uint32_t)) return XZ_APP_ERR_INVALID;
    std::lock_guard<std::mutex> lock(r->mutex);
    std::memcpy(result, &r->status, std::min<size_t>(result->struct_size, sizeof(r->status)));
    return XZ_APP_OK;
}

bool AppRecordBusy(const std::shared_ptr<AppAudioRecorder>& r) { return r && r->busy.load(); }
void AppRecordForeground(const std::shared_ptr<AppAudioRecorder>& r, bool foreground) {
    if (r) r->foreground.store(foreground);
}
void AppRecordClose(const std::shared_ptr<AppAudioRecorder>& r) {
    if (r) { r->stop.store(true); r->closed.store(true); }
}
