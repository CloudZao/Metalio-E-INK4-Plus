#include <cerrno>
#include <climits>
#include <utility>
#include <new>
#include <sys/stat.h>
#include <unistd.h>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "firmware_update.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "boards/common/board/board.h"
#include "boards/common/input/input.h"
#include "boards/common/storage/SdCardManager.hpp"
#include "business/conversation/activation_polling.h"
#include "business/app_store/app_network.h"
#include "sysmgmt/audio/audio_runtime.h"
#include "infra/network/endpoints/api_endpoints.h"
#include "infra/ota/firmware_image.h"
#include "infra/network/device_config_client.h"
#include "infra/ota/firmware_ignore.h"
#include "infra/storage/sd_paths.h"
#include "sysmgmt/system_control.h"
#include "sysmgmt/system_state.h"
#include "sysmgmt/tasks/task_executor.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "ui/common/drawing/display.h"
#include "sysmgmt/update/update_presentation.h"
#include "sysmgmt/power/standby_presentation.h"
#include "sysmgmt/system_notice.h"

static constexpr const char* TAG = "FirmwareUpdate";

struct FirmwareCheckOccupancy {
    std::atomic<bool>& checking;  // 查询所有者的占用状态
    ~FirmwareCheckOccupancy() {
        checking.store(false, std::memory_order_release);
    }
};

static bool ReadVersionSegment(const std::string& version, size_t& position, unsigned& value) {
    const size_t begin = position;
    value = 0;
    while (position < version.size() && version[position] >= '0' && version[position] <= '9') {
        const unsigned digit = unsigned(version[position++] - '0');
        if (value > (unsigned(INT_MAX) - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    if (position == begin || (position < version.size() && version[position] != '.')) {
        return false;
    }
    if (position < version.size()) {
        ++position;
        if (position == version.size()) {
            return false;
        }
    }
    return true;
}

static bool IsValidFirmwareVersion(const std::string& version) {
    size_t position = 0;
    unsigned value = 0;
    if (version.empty()) {
        return false;
    }
    while (position < version.size()) {
        if (!ReadVersionSegment(version, position, value)) {
            return false;
        }
    }
    return true;
}

static bool FirmwareVersionIsNewer(const std::string& current, const std::string& next, bool& newer) {
    newer = false;
    if (!IsValidFirmwareVersion(current) || !IsValidFirmwareVersion(next)) {
        return false;
    }
    size_t current_position = 0;
    size_t next_position = 0;
    while (current_position < current.size() && next_position < next.size()) {
        unsigned current_value = 0;
        unsigned next_value = 0;
        ReadVersionSegment(current, current_position, current_value);
        ReadVersionSegment(next, next_position, next_value);
        if (current_value != next_value) {
            newer = next_value > current_value;
            return true;
        }
    }
    // 保持已有规则：公共段相等时，段数更多的版本较新。
    newer = next_position < next.size();
    return true;
}

FirmwareUpdate& FirmwareUpdate::GetInstance() {
    static FirmwareUpdate instance;
    return instance;
}

void FirmwareUpdate::Initialize(bool (*prepare_update)(), void (*resume_conversation)()) {
    prepare_update_ = prepare_update;
    resume_conversation_ = resume_conversation;
}

bool FirmwareUpdate::VersionChecked() const {
    return version_checked_.load(std::memory_order_acquire);
}

FirmwareCheckResult FirmwareUpdate::QueryVersion(const std::function<bool()>& still_needed) try {
    FirmwareCheckResult result;
    result.current = esp_app_get_description()->version;
    auto cancelled = [&] {
        return busy_.load(std::memory_order_acquire) || StandbyPresentation::IsActive() ||
               SystemState::GetInstance().Phase() == SystemPhase::Restarting ||
               ActivationPolling::GetInstance().IsPaused() || (still_needed && !still_needed());
    };
    if (cancelled()) {
        result.status = FirmwareCheckStatus::Deferred;
        return result;
    }
    if (!ApiHasDefaultOtaUrl()) {
        result.status = FirmwareCheckStatus::Current;
        return result;
    }
    ActivationPollGuard poll_guard;
    if (!poll_guard) {
        result.status = FirmwareCheckStatus::Deferred;
        return result;
    }
    PowerNeedHold network(PowerNeed::OtaDownload);
    DeviceConfigClient client(cancelled);
    const esp_err_t err = client.QueryFirmware();
    if (cancelled()) {
        result.status = FirmwareCheckStatus::Deferred;
        return result;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "operation=query_firmware code=%d", int(err));
        return result;
    }
    result.version = client.GetFirmwareVersion();
    result.url = client.GetFirmwareUrl();
    if (result.version.empty() && result.url.empty()) {
        result.status = FirmwareCheckStatus::Current;
        return result;
    }
    bool newer = false;
    if (!FirmwareVersionIsNewer(result.current, result.version, newer) || result.url.empty()) {
        ESP_LOGE(TAG, "operation=query_firmware result=invalid_metadata");
        return result;
    }
    result.status = newer ? FirmwareCheckStatus::Available : FirmwareCheckStatus::Current;
    return result;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=query_firmware result=no_memory");
    return {};
}

bool FirmwareUpdate::RequestVersionCheck(std::function<bool()> still_needed,
                                         std::function<void(FirmwareCheckResult)> completed) try {
    bool idle = false;
    if (!completed || !checking_.compare_exchange_strong(idle, true)) {
        return false;
    }
    if (!TaskExecutor::Schedule(
            [this, still_needed = std::move(still_needed), completed = std::move(completed)]() {
                FirmwareCheckOccupancy occupancy{checking_};
                auto result = QueryVersion(still_needed);
                completed(std::move(result));
            },
            TaskDomain::Network)) {
        checking_.store(false, std::memory_order_release);
        return false;
    }
    return true;
} catch (const std::bad_alloc&) {
    checking_.store(false, std::memory_order_release);
    ESP_LOGE(TAG, "operation=request_firmware_check result=no_memory");
    return false;
}

bool FirmwareUpdate::CheckVersion() try {
    bool idle = false;
    if (prepare_update_ == nullptr || !Board::GetInstance().IsNetworkReady() ||
        !checking_.compare_exchange_strong(idle, true)) {
        return false;
    }
    FirmwareCheckOccupancy occupancy{checking_};
    const bool background = SystemState::GetInstance().IsReady();
    if (!background) {
        SystemState::GetInstance().PublishDevice(kDeviceStateActivating);
        Board::GetInstance().GetDisplay()->SetStatus(LangString_CHECKING_NEW_VERSION);
    }
    const auto offer = QueryVersion({});
    if (offer.status == FirmwareCheckStatus::Deferred) {
        return false;
    }
    if (offer.status == FirmwareCheckStatus::Failed) {
        UiShowSystemAlert(LangString_ERROR, LangString_SERVER_ERROR, "cloud_slash");
    } else if (offer.status == FirmwareCheckStatus::Available && !FirmwareIgnoreIsIgnored(offer.version)) {
        const auto choice =
            OtaConfirmDialog::ShowBlocking(offer.current.c_str(), offer.version.c_str(), OtaConfirmDialog::Mode::Boot);
        if (choice == OtaConfirmDialog::Choice::Failed) {
            ESP_LOGW(TAG, "operation=confirm_firmware result=unavailable");
            return false;
        }
        if (choice == OtaConfirmDialog::Choice::Upgrade) {
            if (background) {
                if (!RequestFromUrl(offer.url, offer.current, offer.version)) {
                    ESP_LOGE(TAG, "operation=request_upgrade result=not_accepted");
                }
            } else {
                bool idle_upgrade = false;
                if (busy_.compare_exchange_strong(idle_upgrade, true)) {
                    UpgradeFromUrl(offer.url, offer.current, offer.version);
                }
            }
        } else if (choice == OtaConfirmDialog::Choice::IgnorePersist) {
            FirmwareIgnoreSet(offer.version);
        }
    }
    version_checked_.store(true, std::memory_order_release);
    return true;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=check_firmware result=no_memory");
    return false;
}

void FirmwareUpdate::FinishUpgrade() {
    if (SystemState::GetInstance().Phase() == SystemPhase::Updating) {
        SystemState::GetInstance().EndUpdate();
    }
    const bool resume = restore_conversation_;
    restore_conversation_ = false;
    busy_.store(false, std::memory_order_release);
    if (resume && resume_conversation_) {
        resume_conversation_();
    }
}

bool FirmwareUpdate::RequestManual(const std::string& url) {
    return RequestFromUrl(url, esp_app_get_description()->version, LangString_OTA_MANUAL);
}

bool FirmwareUpdate::RequestFromUrl(const std::string& url, const std::string& current, const std::string& next) try {
    bool idle = false;
    if (url.empty() || prepare_update_ == nullptr || SystemState::GetInstance().Phase() == SystemPhase::Restarting ||
        !busy_.compare_exchange_strong(idle, true)) {
        return false;
    }
    if (!TaskExecutor::Schedule([this, url, current, next] { UpgradeFromUrl(url, current, next); })) {
        busy_.store(false, std::memory_order_release);
        return false;
    }
    return true;
} catch (const std::bad_alloc&) {
    busy_.store(false, std::memory_order_release);
    ESP_LOGE(TAG, "operation=request_upgrade result=no_memory");
    return false;
}

bool FirmwareUpdate::TrySdCard() try {
    if (!SdCardManager::GetInstance().IsMounted()) {
        return false;
    }

    struct stat st{};
    if (stat(SD_PATH_OTA_FIRMWARE, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        return false;
    }

    ESP_LOGI(TAG, "SD firmware found: %s (%ld bytes)", SD_PATH_OTA_FIRMWARE, static_cast<long>(st.st_size));

    bool idle = false;
    if (prepare_update_ == nullptr || !busy_.compare_exchange_strong(idle, true)) {
        return false;
    }
    struct Completion {
        FirmwareUpdate& owner;
        ~Completion() {
            owner.FinishUpgrade();
        }
    } completion{*this};
    const esp_app_desc_t* app_desc = esp_app_get_description();
    const std::string current_version = (app_desc != nullptr && app_desc->version[0] != '\0') ? app_desc->version : "—";

    // Sd 模式：立刻升级 / 取消；文案仅「SD卡发现新固件 + 当前版本」
    const auto choice = OtaConfirmDialog::ShowBlocking(current_version.c_str(), "", OtaConfirmDialog::Mode::Sd);
    if (choice != OtaConfirmDialog::Choice::Upgrade) {
        ESP_LOGI(TAG, "SD firmware upgrade cancelled; keep %s for next boot", SD_PATH_OTA_FIRMWARE);
        return false;
    }

    PowerNeedHold download(PowerNeed::OtaDownload);
    if (!PrepareExecution()) {
        return false;
    }

    InputSamplingPause touch_pause;
    // 进度页不传最新版本（空串隐藏「最新版本」行）
    if (!OtaUpgradeScreen::Show(current_version.c_str(), "")) {
        ESP_LOGW(TAG, "SD OTA upgrade UI show incomplete; continuing with keys locked");
    }
    OtaUpgradeScreen::SetProgress(0);
    UiShowSystemAlert(LangString_OTA_UPGRADE, LangString_UPGRADING, "download");
    vTaskDelay(pdMS_TO_TICKS(800));

    AudioRuntime::GetInstance().StopIfRunning();
    vTaskDelay(pdMS_TO_TICKS(500));

    FirmwareImage image;
    const bool ok = image.StartUpgradeFromFile(
        SD_PATH_OTA_FIRMWARE, [](int progress, size_t /*speed*/) { OtaUpgradeScreen::SetProgress(progress); });

    auto remove_sd_fw = []() {
        if (unlink(SD_PATH_OTA_FIRMWARE) != 0) {
            ESP_LOGW(TAG, "failed to remove %s: errno=%d", SD_PATH_OTA_FIRMWARE, errno);
        } else {
            ESP_LOGI(TAG, "removed %s", SD_PATH_OTA_FIRMWARE);
        }
    };

    if (!ok) {
        ESP_LOGE(TAG, "SD firmware upgrade failed");
        OtaUpgradeScreen::SetHint(LangString_UPGRADE_FAILED);
        vTaskDelay(pdMS_TO_TICKS(3000));
        OtaUpgradeScreen::Dismiss();
        return false;
    }

    FirmwareIgnoreClear();
    ESP_LOGI(TAG, "SD firmware upgrade successful, rebooting...");
    OtaUpgradeScreen::SetProgress(100);
    OtaUpgradeScreen::SetHint(LangString_OTA_SUCCESS_REBOOT);
    vTaskDelay(pdMS_TO_TICKS(1200));
    remove_sd_fw();
    restore_conversation_ = false;
    SystemRequestReboot();
    return true;
} catch (const std::bad_alloc&) {
    OtaUpgradeScreen::Dismiss();
    ESP_LOGE(TAG, "operation=sd_upgrade result=no_memory");
    return false;
}

bool FirmwareUpdate::UpgradeFromUrl(const std::string& url, const std::string& current_version,
                                    const std::string& new_version) try {
    struct Completion {
        FirmwareUpdate& owner;
        ~Completion() {
            owner.FinishUpgrade();
        }
    } completion{*this};
    std::string cur = current_version.empty() ? "—" : current_version;
    std::string next = new_version.empty() ? "—" : new_version;

    // Close audio channel if it's open
    PowerNeedHold download(PowerNeed::OtaDownload);
    if (!PrepareExecution()) {
        return false;
    }

    InputSamplingPause touch_pause;
    // 全屏升级页：锁键 + 版本/进度；须在非 LVGL 线程调用
    if (!OtaUpgradeScreen::Show(cur.c_str(), next.c_str())) {
        ESP_LOGW(TAG, "OTA upgrade UI show incomplete; continuing with keys locked");
    }
    OtaUpgradeScreen::SetProgress(0);

    // 顶栏文案 + 提示音；进度改由全屏页展示（墨水屏 SetChatMessage 在非百问页无效）
    UiShowSystemAlert(LangString_OTA_UPGRADE, LangString_UPGRADING, "download");
    vTaskDelay(pdMS_TO_TICKS(800));  // 给墨水首帧局刷时间

    AudioRuntime::GetInstance().StopIfRunning();
    vTaskDelay(pdMS_TO_TICKS(500));

    if (!Board::GetInstance().EnsureNetworkReady()) {
        ESP_LOGE(TAG, "Firmware upgrade aborted: network not ready");
        OtaUpgradeScreen::Dismiss();
        UiShowSystemAlert(LangString_ERROR, LangString_UPGRADE_FAILED, "circle_xmark");
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    }

    FirmwareImage image;
    bool upgrade_success =
        image.StartUpgradeFromUrl(url, [](int progress, size_t /*speed*/) { OtaUpgradeScreen::SetProgress(progress); });

    if (!upgrade_success) {
        ESP_LOGE(TAG, "Firmware upgrade failed, continuing operation...");
        OtaUpgradeScreen::Dismiss();
        UiShowSystemAlert(LangString_ERROR, LangString_UPGRADE_FAILED, "circle_xmark");
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    }

    FirmwareIgnoreClear();
    ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
    OtaUpgradeScreen::SetProgress(100);
    OtaUpgradeScreen::SetHint(LangString_OTA_SUCCESS_REBOOT);
    vTaskDelay(pdMS_TO_TICKS(1200));  // 让 100% 有机会上屏
    // 成功路径直接重启，无需 Dismiss（复位后自然清空）
    restore_conversation_ = false;
    SystemRequestReboot();
    return true;
} catch (const std::bad_alloc&) {
    OtaUpgradeScreen::Dismiss();
    ESP_LOGE(TAG, "operation=url_upgrade result=no_memory");
    return false;
}

void FirmwareUpdate::PrepareBoot() {
    FirmwareImage::MarkCurrentVersionValid();
}

bool FirmwareUpdate::PrepareExecution() {
    restore_conversation_ = prepare_update_();
    auto& audio = AudioRuntime::GetInstance();
    audio.StopIfRunning();
    const auto deadline = esp_timer_get_time() + 30000000;
    while (audio.HasWork()) {
        if (esp_timer_get_time() >= deadline) {
            ESP_LOGW(TAG, "operation=prepare_upgrade stage=audio result=busy");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!SystemState::GetInstance().BeginUpdate()) {
        ESP_LOGW(TAG, "operation=prepare_upgrade result=lifecycle_changed");
        return false;
    }
    // 先关闭 ELF 网络准入，再等待前台取消及连接收尾；60 秒连接期限外另留 5 秒收尾。
    if (AppNetworkBusy()) {
        ESP_LOGI(TAG, "operation=prepare_upgrade stage=app_network result=draining");
    }
    const auto network_deadline = esp_timer_get_time() + 65000000;
    while (AppNetworkBusy()) {
        if (esp_timer_get_time() >= network_deadline) {
            ESP_LOGW(TAG, "operation=prepare_upgrade stage=app_network result=busy");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}
