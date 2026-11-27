#include <new>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ssid_manager.h>
#include "infra/tasks/task_create.h"
#include "ui/common/widgets/weather_view.h"
#include "ui/application_ui.h"
#include "application.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "boards/common/board/board.h"
#include "boards/common/storage/SdCardManager.hpp"
#include "boards/common/storage/sd_log_writer.h"
#include "business/checklist/checklist_service.h"
#include "business/diagnostics/aging_test.h"
#include "business/conversation/activation_polling.h"
#include "business/conversation/assistant_chat_store.h"
#include "business/conversation/chat_cloud.h"
#include "business/conversation/conversation_controller.h"
#include "business/haptics/haptic_preferences.h"
#include "business/mcp_tool/mcp_tools.h"
#include "business/reader/book_home_snapshot.h"
#include "business/weather/weather_cache.h"
#include "business/wallpaper/wallpaper_active.h"
#include "business/yunxin_binding/yunxin_binding.h"
#include "sysmgmt/audio/audio_runtime.h"
#include "infra/diagnostics/firmware_memory.h"
#include "infra/network/endpoints/api_endpoints.h"
#include "infra/platform/system_info.h"
#include "sysmgmt/time/network_time.h"
#include "sysmgmt/system_control.h"
#include "sysmgmt/system_state.h"
#include "sysmgmt/tasks/task_executor.h"
#include "sysmgmt/update/firmware_update.h"
#include "ui/common/drawing/a2ui/a2ui_img_cache.h"
#include "ui/common/drawing/display.h"
#include "ui/common/drawing/font/fontpack_lvgl.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "business/reader/book_reader_prefs.h"
#include "business/reader/book_tap_zones.h"
#include "ui/common/widgets/screen_common.h"
#include "sysmgmt/power/standby_presentation.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"
#include "sysmgmt/system_notice.h"

static constexpr const char* TAG = "Application";
static constexpr EventBits_t kClock = 1;
static constexpr EventBits_t kConversation = 2;

bool Application::InitializeRuntime() {
    event_group_ = xEventGroupCreate();
    if (event_group_ == nullptr) {
        ESP_LOGE(TAG, "operation=start stage=event_group result=no_memory");
        return false;
    }
    const esp_timer_create_args_t args = {
        .callback =
            [](void* context) {
                auto* app = static_cast<Application*>(context);
                xEventGroupSetBits(app->event_group_, kClock);
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "system_tick",
        .skip_unhandled_events = true,
    };
    const auto result = esp_timer_create(&args, &clock_timer_handle_);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "operation=start stage=clock_create code=%d", static_cast<int>(result));
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
        return false;
    }
    if (!ConversationController::GetInstance().Initialize([] {
            auto& app = Application::GetInstance();
            if (app.event_group_) {
                xEventGroupSetBits(app.event_group_, kConversation);
            }
        })) {
        esp_timer_delete(clock_timer_handle_);
        clock_timer_handle_ = nullptr;
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
        return false;
    }
    SystemControlInitialize([] { ConversationController::GetInstance().CloseForSystem(); });
    FirmwareUpdate::GetInstance().Initialize([] { return ConversationController::GetInstance().CloseForSystem(); },
                                             [] { ConversationController::GetInstance().StartSession(); });
    return true;
}

bool Application::StartDispatcher() {
    if (FirmwareCreateTask([](void* context) { static_cast<Application*>(context)->MainEventLoop(); }, "main_event_loop", 3072,
                    this, 3, &dispatcher_, tskNO_AFFINITY, 0) != pdPASS) {
        ESP_LOGE(TAG, "operation=start stage=dispatcher result=no_memory");
        return false;
    }
    const auto result = esp_timer_start_periodic(clock_timer_handle_, 1000000);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "operation=start stage=clock_start code=%d", static_cast<int>(result));
        vTaskDelete(dispatcher_);
        dispatcher_ = nullptr;
        return false;
    }
    return true;
}

void Application::FinishStartup(bool completed) {
    SystemState::GetInstance().SetStartupBusy(false);
    if (!completed) {
        SystemState::GetInstance().FailStartup();
    }
}

void Application::MainEventLoop() {
    for (;;) {
        const auto bits = xEventGroupWaitBits(event_group_, kClock | kConversation, pdTRUE, pdFALSE, portMAX_DELAY);
        ConversationController::GetInstance().Poll();
        if (bits & kClock) {
            DispatchClockTick();
        }
        AudioRuntime::GetInstance().RetryPendingReleases();
        SystemControlPoll();
    }
}

void Application::BootStandbyCheckpoint() {
    SystemState::GetInstance().SetStartupBusy(true);
    if (StandbyPresentation::IsActive() || SystemState::GetInstance().Phase() == SystemPhase::Updating ||
        SystemState::GetInstance().Phase() == SystemPhase::Restarting) {
        SystemState::GetInstance().SetStartupBusy(false);
        ESP_LOGI(TAG, "boot paused for standby (SD/network initialization deferred)");
        while (StandbyPresentation::IsActive() || SystemState::GetInstance().Phase() == SystemPhase::Updating ||
               SystemState::GetInstance().Phase() == SystemPhase::Restarting) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        SystemState::GetInstance().SetStartupBusy(true);
    }
}

bool Application::Start() try {
    UiInitializeApplications();
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        ESP_LOGE(TAG, "operation=start result=already_started");
        return false;
    }
    struct StartupGuard {
        Application& owner;
        bool completed = false;
        ~StartupGuard() {
            owner.FinishStartup(completed);
        }
    } startup{*this};
    if (!InitializeRuntime()) {
        return false;
    }
    BootStandbyCheckpoint();
    ChatCloudInitializeFromNvs();
    // UI 语言优先读取 NVS ui/language，无记录时使用板型默认语言。
    LangInitFromNvs();
    // 在板级构造创建显示与触摸线程之前预读 NVS，避免界面线程访问 Flash。
    UiLoadApplicationPreferences();
    // 先预读系统字体选择，SD 可用后由现有 UI 轮询异步恢复，不等待联网。
    UiFontRestore();
    BookReaderPrefsEnsureLoaded();
    BookTapZonesEnsureLoaded();
    HapticIsEnabled();
    Wallpaper_HydrateFromNvsNow();
    BookHomeSnapshotHydrateFromNvs();

    // SD 运行日志使用公共存储路径，默认开启。
    // 挂载且开启时，Flash coredump 导出成功后擦除原槽。
    SdLogWriter_Start();
    // The 470 SD logger exports crashes on its existing internal-stack worker.

    auto& board = Board::GetInstance();
    SystemState::GetInstance().PublishDevice(kDeviceStateStarting);
    // 在本地启动阶段检测摄像头，不依赖联网或云端初始化。
    board.GetCamera();

    /* Setup the display */
    auto display = board.GetDisplay();
    if (display == nullptr) {
        ESP_LOGE(TAG, "operation=start stage=display result=unavailable");
        return false;
    }

    // 在内部 DRAM 栈初始化字库映射并预热系统界面字体角色。
    fontpack_lv_ensure_ready();
    UiFont();

    // A2UI 图片会话缓存使用公共 SD 布局并在启动时清空；无卡时仅支持 HTTP。
    {
        esp_err_t cache_err = a2ui_img_cache_init();
        if (cache_err != ESP_OK) {
            ESP_LOGW(TAG, "a2ui_img_cache_init: %s (HTTP-only images)", esp_err_to_name(cache_err));
        }
    }

    // 会话 JSON 使用公共 SD 布局并在启动时清空；无卡时不保存历史。
    {
        esp_err_t chat_err = assistant_chat_store_init();
        if (chat_err != ESP_OK) {
            ESP_LOGW(TAG, "assistant_chat_store_init: %s (RAM-only chat)", esp_err_to_name(chat_err));
        }
    }

    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    if (!StartDispatcher()) {
        display->SetStatus(LangString_ERROR);
        return false;
    }
    FirmwareUpdate::GetInstance().PrepareBoot();
    BootStandbyCheckpoint();
    FirmwareUpdate::GetInstance().TrySdCard();
    BootStandbyCheckpoint();

    /* 有已存 WiFi：等保网时长；失败不 SoftAP。无 SSID 跳过联网，到设置→网络机上配网 */
    board.StartNetwork();
    BootStandbyCheckpoint();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

    BootStandbyCheckpoint();

    auto& firmware = FirmwareUpdate::GetInstance();
    auto& conversation = ConversationController::GetInstance();
    firmware.CheckVersion();
    BootStandbyCheckpoint();
    if (firmware.VersionChecked()) {
        conversation.RefreshConfiguration();
        TimeServiceSyncNetwork();
    }
    BootStandbyCheckpoint();
    InitializeDeviceMcpTools(
        board,
        {SystemRequestReboot, [](const std::string& url) { return FirmwareUpdate::GetInstance().RequestManual(url); }},
        [](const std::string& payload) { ConversationController::GetInstance().SendMcpMessage(payload); });

    AudioRuntime::GetInstance().StopIfUnused();

    SystemState::GetInstance().PublishDevice(kDeviceStateIdle);
    UiRestoreIdleStatus();
    // Core readiness must not wait for optional weather/checklist HTTP requests.
    SystemState::GetInstance().MarkReady();
    ESP_LOGI(TAG, "Boot core ready; activation polling and PTT enabled");
    FirmwareMemoryCheckpoint("boot-complete");
    conversation.OnSystemReady();

    ESP_LOGI(TAG, "Boot background warmup begin");
    BootStandbyCheckpoint();
    // 天气：每天开机拉一次，写入 NVS；当天后续走缓存
    WeatherEnsureCached();
    WeatherView_Refresh();
    // 开机未连上：勿 EnsureNetworkReady 再空等（会硬占网续扫）
    // 配网 SoftAP / 4G 未真正上网时 IsNetworkReady=false，一律跳过联网任务
    if (board.IsNetworkReady() && !board.IsWifiConfigMode()) {
        if (ApiHasHost()) {
            ChecklistSyncCache();
            // 全库上报依赖完整扫描，按需目录浏览不在开机触发该操作。
        } else {
            ESP_LOGI(TAG, "skip boot checklist: cloud endpoints blank");
        }
    } else {
        ESP_LOGW(TAG, "skip boot checklist/book sync: network not ready");
    }

    ESP_LOGI(TAG, "Boot background warmup complete");
    startup.completed = true;
    return true;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=start result=no_memory");
    return false;
}

void Application::DispatchClockTick() {
    AgingTestPoll();
    const int tick = clock_ticks_.fetch_add(1) + 1;
    if (!status_work_queued_.exchange(true)) {
        if (!ScreenLvAsync(
                [](void* context) {
                    auto* app = static_cast<Application*>(context);
                    static bool sd_error_notified = false;
                    if (!sd_error_notified && SdCardManager::GetInstance().HasIoError() && !StandbyPresentation::IsActive()) {
                        ScreenShowToast(LangString_STORAGE_IO_ERROR);
                        sd_error_notified = true;
                    }
                    if (auto* display = Board::GetInstance().GetDisplay()) {
                        display->UpdateStatusBar();
                    }
                    app->status_work_queued_.store(false, std::memory_order_release);
                },
                this)) {
            status_work_queued_.store(false);
        }
    }
    if (SystemState::GetInstance().IsReady() && tick % 30 == 0 && !StandbyPresentation::IsActive() &&
        !storage_retry_queued_.exchange(true)) {
        if (!TaskExecutor::Schedule(
                [this]() {
                    struct Completion {
                        std::atomic<bool>& queued;
                        ~Completion() {
                            queued.store(false, std::memory_order_release);
                        }
                    } completion{storage_retry_queued_};
                    SsidManager::GetInstance().RetryStorage();
                },
                TaskDomain::Storage)) {
            storage_retry_queued_.store(false);
        }
    }
    auto& conversation = ConversationController::GetInstance();
    auto& firmware = FirmwareUpdate::GetInstance();
    auto& polling = ActivationPolling::GetInstance();
    const bool due = (YunxinBinding::Enabled() ? YunxinBinding::IsDue() : tick % 10 == 0) || polling.HasCheckRequest();
    if (SystemState::GetInstance().IsReady() && !conversation.IsConfigured() && due && !StandbyPresentation::IsActive() &&
        !polling.IsPaused() && !firmware.IsBusy() && !activation_work_queued_.exchange(true)) {
        if (!TaskExecutor::Schedule(
                [this] {
                    struct Completion {
                        std::atomic<bool>& queued;
                        ~Completion() {
                            queued.store(false, std::memory_order_release);
                        }
                    } completion{activation_work_queued_};
                    try {
                        auto& board = Board::GetInstance();
                        auto& polling = ActivationPolling::GetInstance();
                        auto& firmware = FirmwareUpdate::GetInstance();
                        if (StandbyPresentation::IsActive() || polling.IsPaused() || firmware.IsBusy() ||
                            !board.IsNetworkReady() || board.IsWifiConfigMode()) {
                            return;
                        }
                        polling.TakeCheckRequest();
                        if (!firmware.VersionChecked() && !firmware.CheckVersion()) {
                            polling.RequestCheck();
                            return;
                        }
                        if (!firmware.IsBusy() && !StandbyPresentation::IsActive() && !polling.IsPaused()) {
                            ConversationController::GetInstance().RefreshConfiguration();
                        }
                    } catch (const std::bad_alloc&) {
                        ESP_LOGE(TAG, "operation=cloud_refresh result=no_memory");
                    }
                },
                TaskDomain::Network)) {
            activation_work_queued_.store(false, std::memory_order_release);
        }
    }
}
