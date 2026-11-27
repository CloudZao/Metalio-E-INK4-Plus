#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <optional>
#include <utility>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "aging_test.h"
#include "assets/factory_test_assets.h"
#include "assets/lang_config.h"
#include "boards/common/audio/bluetooth/bluetooth_audio.h"
#include "boards/common/haptics/vibration.h"
#include "boards/common/storage/sd_access.h"
#include "config.h"
#include "sysmgmt/audio/audio_runtime.h"
#include "infra/storage/sd_paths.h"
#include "infra/storage/settings.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "sysmgmt/system_control.h"
#include "sysmgmt/tasks/task_executor.h"

static constexpr const char* TAG = "AgingTest";
static constexpr int64_t kAudioReplayUs = 7500000; // 约2.5秒播放及5秒间隔，SD读取完成后开始计时
static constexpr int64_t kVibeCycleUs = 10500000; // 0.5秒输出与10秒间隔
static_assert(BOARD_AGING_RESTART_SECONDS > 0);
static constexpr int64_t kRestartIntervalUs = int64_t(BOARD_AGING_RESTART_SECONDS) * 1000000;

static std::mutex s_mutex;
static std::atomic<uint32_t> s_generation{0};
static AgingTestPhase s_phase = AgingTestPhase::Idle;
static int32_t s_restart_count = -1;
static int32_t s_saved_restart_count = -1;
static uint32_t s_cycles = 0;
static int s_preview_side = 0;
static esp_timer_handle_t s_timer = nullptr;
static bool s_timer_running = false;
static bool s_audio_ready = false;
static bool s_audio_pending = false;
static bool s_save_pending = false;
static bool s_save_failed = false;
static bool s_reboot_requested = false;
static int64_t s_audio_at = 0;
static int64_t s_audio_init_at = 0;
static int64_t s_vibe_at = 0;
static int64_t s_save_at = 0;
static int64_t s_restart_at = 0;
static uint32_t s_vibe_pulse = 0;
static std::optional<PowerNeedHold> s_test_hold;
static std::optional<PowerNeedHold> s_inhibit_hold;
static std::atomic<uint32_t> s_sd_pending{0};
static std::atomic<unsigned> s_reading{0};
static std::shared_ptr<const AgingSdResult> s_preview;

static bool AgingSdCancelled(void* context) {
    return static_cast<AgingSdResult*>(context)->generation != s_generation.load() || SdAccessIsQuiescing();
}

static bool AgingSdImageName(const char* name) {
    if (name[0] == '.' || !SdIsValidFileName(name)) {
        return false;
    }
    const char* extension = std::strrchr(name, '.');
    return extension && (!strcasecmp(extension, ".png") || !strcasecmp(extension, ".jpg") ||
                         !strcasecmp(extension, ".jpeg") || !strcasecmp(extension, ".a2i1") ||
                         !strcasecmp(extension, ".a2l8") || !strcasecmp(extension, ".ebgr"));
}

static void AgingSdLogResult(const AgingSdResult& work) {
    static uint32_t generation = UINT32_MAX;
    static StorageError previous;
    static ImageDecodeError previous_decode = ImageDecodeError::kNone;
    static AgingSdStatus previous_status = AgingSdStatus::kEmpty;
    const bool same = generation == work.generation && previous.code == work.error.code &&
                      previous.stage == work.error.stage && previous.system_error == work.error.system_error &&
                      previous.platform_error == work.error.platform_error && previous_decode == work.decode_error &&
                      previous_status == work.status;
    if (same) {
        return;
    }
    if (work.status == AgingSdStatus::kReady && generation == work.generation &&
        previous_status == AgingSdStatus::kFailed) {
        ESP_LOGI(TAG, "wallpaper read recovered");
    } else if (work.status == AgingSdStatus::kFailed) {
        if (work.error) {
            if (work.error.code != StorageErrorCode::Busy && work.error.code != StorageErrorCode::UsbOwned) {
                LogStorageError(TAG, work.error);
            }
        } else {
            ESP_LOGE(TAG, "operation=wallpaper_decode code=%u", unsigned(work.decode_error));
        }
    }
    generation = work.generation;
    previous = work.error;
    previous_decode = work.decode_error;
    previous_status = work.status;
}

static void AgingSdReadFirst(AgingSdResult& work) {
    if (work.generation != s_generation.load()) {
        work.status = AgingSdStatus::kCancelled;
        return;
    }
    work.status = AgingSdStatus::kFailed;
    SdAccessTransaction access;
    if (!access) {
        work.error = access.Error();
        return;
    }
    errno = 0;
    std::unique_ptr<DIR, decltype(&closedir)> directory(opendir(SD_PATH_WALLPAPER), closedir);
    if (!directory) {
        work.error = StorageErrorFromErrno(StorageStage::DirectoryOpen, errno);
        return;
    }
    unsigned visited = 0;
    while (!AgingSdCancelled(&work)) {
        errno = 0;
        const dirent* entry = readdir(directory.get());
        if (!entry) {
            if (errno) {
                work.error = StorageErrorFromErrno(StorageStage::DirectoryRead, errno);
            } else {
                work.status = AgingSdStatus::kEmpty;
            }
            return;
        }
        if ((++visited & 15u) == 0) {
            vTaskDelay(1);
        }
        if (!AgingSdImageName(entry->d_name)) {
            continue;
        }
        char path[SD_FILE_PATH_CAPACITY];
        const int length = std::snprintf(path, sizeof(path), "%s/%s", SD_PATH_WALLPAPER, entry->d_name);
        if (length < 0 || static_cast<size_t>(length) >= sizeof(path)) {
            work.error = StorageErrorFromErrno(StorageStage::Stat, ENAMETOOLONG);
            return;
        }
        struct stat status {};
        if (stat(path, &status) != 0) {
            work.error = StorageErrorFromErrno(StorageStage::Stat, errno);
            return;
        }
        if (!S_ISREG(status.st_mode)) {
            continue;
        }
        errno = 0;
        const bool decoded =
            DecodeImageFileToL8(path, work.side, work.side, work.image, AgingSdCancelled, &work,
                                &work.decode_error, &work.error);
        if (AgingSdCancelled(&work)) {
            work.status = work.generation != s_generation.load() ? AgingSdStatus::kCancelled : AgingSdStatus::kFailed;
            work.error = work.status == AgingSdStatus::kCancelled ? StorageError{StorageErrorCode::Cancelled} :
                         StorageError{StorageErrorCode::Busy, StorageStage::Access, EBUSY};
        } else if (decoded && !work.image.empty()) {
            work.status = AgingSdStatus::kReady;
        } else if (!work.error && work.decode_error == ImageDecodeError::kNone) {
            work.decode_error = ImageDecodeError::kFailed;
        }

        return;
    }
    work.status = work.generation != s_generation.load() ? AgingSdStatus::kCancelled : AgingSdStatus::kFailed;
    work.error = work.status == AgingSdStatus::kCancelled ? StorageError{StorageErrorCode::Cancelled} :
                 StorageError{StorageErrorCode::Busy, StorageStage::Access, EBUSY};
}

static bool Running(uint32_t generation) {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_phase == AgingTestPhase::Running && s_generation.load() == generation;
}

static void FinishAudioCycle(uint32_t generation) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_generation.load() == generation && s_phase == AgingTestPhase::Running) {
        s_audio_pending = false;
        s_audio_at = esp_timer_get_time() + kAudioReplayUs;
    }
}

static void ReadSdCycle(uint32_t generation, int side) try {
    uint32_t idle = 0;
    if (!s_sd_pending.compare_exchange_strong(idle, generation)) {
        FinishAudioCycle(generation);
        return;
    }
    struct SubmitGuard {
        uint32_t generation;
        bool accepted = false;
        ~SubmitGuard() {
            if (!accepted) {
                uint32_t expected = generation;
                s_sd_pending.compare_exchange_strong(expected, 0);
                FinishAudioCycle(generation);
            }
        }
    } guard{generation};
    auto result = std::make_shared<AgingSdResult>();
    result->generation = generation;
    result->side = side;
    if (TaskExecutor::Schedule([result] {
        bool reading = false;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_generation.load() == result->generation && s_phase == AgingTestPhase::Running) {
                s_reading.fetch_add(1);
                reading = true;
            }
        }
        if (reading) {
            try {
                AgingSdReadFirst(*result);
                if (result->status != AgingSdStatus::kReady) {
                    result->image.Reset();
                } else if (!result->image.empty()) {
                    result->image.BindDsc();
                }
            } catch (const std::bad_alloc&) {
                result->status = AgingSdStatus::kFailed;
                result->error = {StorageErrorCode::NoMemory, StorageStage::Read};
                result->decode_error = ImageDecodeError::kNoMemory;
                result->image.Reset();
            }
            AgingSdLogResult(*result);
            s_reading.fetch_sub(1);
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_generation.load() == result->generation && s_phase == AgingTestPhase::Running) {
                s_preview = result;
            }
        }
        uint32_t expected = result->generation;
        s_sd_pending.compare_exchange_strong(expected, 0);
        FinishAudioCycle(result->generation);
    }, TaskDomain::Image)) {
        guard.accepted = true;
        return;
    }
    result->status = AgingSdStatus::kFailed;
    result->error = {StorageErrorCode::Rejected, StorageStage::Submit};
    AgingSdLogResult(*result);
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_generation.load() == generation && s_phase == AgingTestPhase::Running) {
        s_preview = result;
    }
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=sd_cycle stage=allocate result=no_memory");
    FinishAudioCycle(generation);
}

static void InitializeAudio(uint32_t generation) {
    bool ready = false;
    try {
        if (BluetoothAudio_WaitModeReady(kBluetoothAudioInitWaitMs, [generation] { return Running(generation); })) {
            ready = AudioRuntime::GetInstance().Acquire(AudioClient::AgingTest, [generation] { return Running(generation); });
        }
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "operation=audio_init result=no_memory");
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_generation.load() == generation && s_phase == AgingTestPhase::Running) {
        s_audio_ready = ready;
        s_audio_pending = false;
        s_audio_init_at = esp_timer_get_time() + 5000000;
        s_audio_at = esp_timer_get_time();
    }
}

static void PlayAudioCycle(uint32_t generation, int side) {
    if (!Running(generation)) {
        return;
    }
    try {
        auto& audio = AudioRuntime::GetInstance().Service();
        if (!audio.IsStarted()) {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_generation.load() == generation) {
                s_audio_ready = false;
                s_audio_pending = false;
                s_audio_init_at = esp_timer_get_time() + 5000000;
            }
            return;
        }
        audio.PlaySound(FactoryTestAssets_OGG_FACTORY_TEST_AUDIO);
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_generation.load() != generation || s_phase != AgingTestPhase::Running) {
                return;
            }
            ++s_cycles;
        }
        ReadSdCycle(generation, side);
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "operation=audio_cycle result=no_memory");
        FinishAudioCycle(generation);
    }
}

static void SaveRestartState() {
    int32_t value;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        value = s_restart_count;
    }
    bool saved = false;
    try {
        Settings settings("factory_test", true);
        saved = settings.SetInt("aging_restarts", value) && settings.Commit();
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "operation=save_restart_state result=no_memory");
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    s_save_pending = false;
    if (saved) {
        s_saved_restart_count = value;
        s_save_failed = false;
        ESP_LOGI(TAG, "operation=save_restart_state result=committed count=%ld", long(value));
    } else {
        if (!s_save_failed) {
            ESP_LOGE(TAG, "operation=save_restart_state result=failed count=%ld", long(value));
        }
        s_save_failed = true;
        s_save_at = esp_timer_get_time() + 5000000;
    }
}

static void StopRuntimeLocked(bool retain_inhibit) {
    s_generation.fetch_add(1);
    Vibration_CancelPulse(s_vibe_pulse);
    s_vibe_pulse = 0;
    s_audio_ready = false;
    s_audio_pending = false;
    s_preview.reset();
    s_test_hold.reset();
    if (!retain_inhibit) {
        s_inhibit_hold.reset();
    }
    // 音频释放入口保留队列拥塞时的清理责任，不在调用线程等待codec。
    AudioRuntime::GetInstance().RequestRelease(AudioClient::AgingTest);
}

void AgingTestPoll() {
    std::lock_guard<std::mutex> lock(s_mutex);
    const auto now = esp_timer_get_time();
    if (s_phase == AgingTestPhase::Idle && s_restart_count == s_saved_restart_count && !s_save_pending) {
        return;
    }
    if (s_restart_count != s_saved_restart_count && !s_save_pending && now >= s_save_at) {
        s_save_at = now + 5000000;
        s_save_pending = true;
        if (!TaskExecutor::Schedule(SaveRestartState, TaskDomain::Storage)) {
            s_save_pending = false;
            s_save_failed = true;
        }
    }
    const bool saved = s_restart_count == s_saved_restart_count && !s_save_pending;
    if (s_phase == AgingTestPhase::Stopping && saved && s_reading.load() == 0) {
        s_phase = AgingTestPhase::Idle;
    }
    if (s_phase == AgingTestPhase::Running && saved && now >= s_restart_at) {
        s_phase = AgingTestPhase::Restarting;
        StopRuntimeLocked(true);
    }
    if (s_phase == AgingTestPhase::Restarting && saved && s_reading.load() == 0 &&
        !s_reboot_requested && now >= s_restart_at) {
        s_reboot_requested = SystemRequestReboot();
        s_restart_at = now + 5000000;
    }
    if (s_phase == AgingTestPhase::Running) {
        const uint32_t generation = s_generation.load();
        if (now >= s_vibe_at) {
            s_vibe_pulse = Vibration_PulseFor(500);
            s_vibe_at = now + kVibeCycleUs;
        }
        if (!s_audio_pending && !s_audio_ready && now >= s_audio_init_at) {
            s_audio_pending = true;
            s_audio_init_at = now + 5000000;
            if (!TaskExecutor::Schedule([generation] { InitializeAudio(generation); }, TaskDomain::Audio)) {
                s_audio_pending = false;
            }
        } else if (!s_audio_pending && s_audio_ready && now >= s_audio_at) {
            s_audio_pending = true;
            const int side = s_preview_side;
            if (!TaskExecutor::Schedule([generation, side] { PlayAudioCycle(generation, side); }, TaskDomain::Audio)) {
                s_audio_pending = false;
                s_audio_at = now + 5000000;
            }
        }
    }
    if ((s_phase == AgingTestPhase::Idle && saved && s_reading.load() == 0) || s_reboot_requested) {
        if (s_timer_running) {
            esp_timer_stop(s_timer);
        }
        s_timer_running = false;
    }
}

static bool EnsureControlTimerLocked() {
    if (!s_timer) {
        const esp_timer_create_args_t args = {
            .callback = [](void*) { AgingTestPoll(); },
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "aging_control",
            .skip_unhandled_events = true,
        };
        const auto error = esp_timer_create(&args, &s_timer);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "operation=timer stage=create error=%d", int(error));
            return false;
        }
    }
    if (!s_timer_running) {
        const auto error = esp_timer_start_periodic(s_timer, 100000);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "operation=timer stage=start error=%d", int(error));
            return false;
        }
        s_timer_running = true;
    }
    return true;
}

bool AgingTestLoadBootState() {
    Settings settings("factory_test");
    const int32_t saved = settings.GetInt("aging_restarts", -1);
    std::lock_guard<std::mutex> lock(s_mutex);
    s_saved_restart_count = saved;
    s_restart_count = saved >= 0 ? saved < INT32_MAX ? saved + 1 : INT32_MAX : -1;
    return saved >= 0;
}

bool AgingTestStart(int preview_side) {
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (preview_side <= 0 || !EnsureControlTimerLocked()) {
            return false;
        }
        if (s_phase == AgingTestPhase::Running || s_phase == AgingTestPhase::Restarting) {
            StopRuntimeLocked(false);
        }
        s_phase = AgingTestPhase::Running;
        s_generation.fetch_add(1);
        s_restart_count = std::max<int32_t>(0, s_restart_count);
        s_cycles = 0;
        s_preview_side = preview_side;
        s_preview.reset();
        s_save_at = s_audio_at = s_audio_init_at = s_vibe_at = 0;
        s_audio_pending = s_audio_ready = s_reboot_requested = false;
        s_restart_at = esp_timer_get_time() + kRestartIntervalUs;
        s_test_hold.emplace(PowerNeed::AudioTesting);
        s_inhibit_hold.emplace(PowerNeed::StandbyInhibit);
    }
    AgingTestPoll();
    return true;
}

void AgingTestStop() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_phase == AgingTestPhase::Idle && s_restart_count == -1 && s_saved_restart_count == -1) {
        return;
    }
    if (s_phase != AgingTestPhase::Stopping && s_phase != AgingTestPhase::Idle) {
        StopRuntimeLocked(false);
    }
    s_phase = AgingTestPhase::Stopping;
    s_restart_count = -1;
    s_save_at = 0;
    EnsureControlTimerLocked();
}

bool AgingTestIsActive() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_phase == AgingTestPhase::Running || s_phase == AgingTestPhase::Restarting;
}

AgingTestSnapshot AgingTestRead() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return {s_phase, s_restart_count, s_cycles, s_save_failed, s_preview};
}
