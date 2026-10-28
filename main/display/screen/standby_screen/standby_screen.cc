#include "screen_frontlight.h"
#include "standby_screen/standby_screen.h"

#include "standby_screen/standby_classic.h"
#include "standby_screen/standby_wallpaper.h"

#include "wallpaper_screen/wallpaper_active.h"
#include "wallpaper_screen/wallpaper_screen.h"

#include "application.h"
#include "assistant_screen/assistant_screen.h"
#include "workbuddy_screen/workbuddy_screen.h"
#include "book_screen/book_screen.h"
#include "cloud_screen/cloud_screen.h"
#include "bq27220_gauge.h"
#include "display_orient.h"
#include "lv_adapter_display.h"
#include "power_policy.h"
#include "screen_common.h"
#include "task_screen/task_screen.h"
#include "settings_screen/settings_screen.h"
#include "settings_screen/settings_test/settings_test_battery_screen.h"
#include "vk_key_handler.h"
#include "sd_access.h"
#include "sd_log_writer.h"
#include "book_library_warmup.h"

#include <atomic>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lvgl.h>


static constexpr const char* TAG = "StandbyScreen";
static constexpr const char* kScreenId = "standby";

enum class StandbyVariant : uint8_t { None = 0, Classic = 1, Wallpaper = 2 };

static std::atomic<bool> s_host_alive{false};
static bool s_as_overlay = false;
static lv_obj_t* s_overlay_root = nullptr;
static lv_obj_t* s_underlying_scr = nullptr;
static StandbyVariant s_variant = StandbyVariant::None;
static std::atomic<uint32_t> s_paint_token{0};
static std::atomic<uint32_t> s_paint_flushed{0};
static SemaphoreHandle_t s_park_done = nullptr;
static std::atomic<bool> s_park_ok{false};
static std::atomic<unsigned> s_wake_request{0};

static void EnsureStandbyPortrait() {
    DisplayUiSetFullScreen(true);
    if (DisplayUiSetOrient(kDisplayUiPortrait)) {
        if (auto* disp = LVAdapterDisplay::Instance()) {
            disp->RequestFullOnNextCommit();
        }
    }
}

static void ClearStatusBindings() {
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->BindStatusWidgets(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    }
}

static bool PreferWallpaperVariant() {
    return Wallpaper_HasStandbySelection();
}

static void TeardownVariant() {
    if (s_variant == StandbyVariant::Classic) {
        StandbyClassic_Teardown();
    } else if (s_variant == StandbyVariant::Wallpaper) {
        StandbyWallpaper_Teardown();
    }
    s_variant = StandbyVariant::None;
}

static void MountVariant(lv_obj_t* root, bool as_overlay) {
    if (PreferWallpaperVariant()) {
        s_variant = StandbyVariant::Wallpaper;
        ESP_LOGI(TAG, "variant=wallpaper");
        StandbyWallpaper_Build(root);
        StandbyWallpaper_StartLoad();
    } else {
        s_variant = StandbyVariant::Classic;
        ESP_LOGI(TAG, "variant=classic");
        StandbyClassic_Build(root, as_overlay);
        StandbyClassic_StartRuntime();
    }
}

static void TeardownHostState() {
    TeardownVariant();
    DisplayUiSetFullScreen(false);
    s_host_alive = false;
    s_as_overlay = false;
    s_overlay_root = nullptr;
    s_underlying_scr = nullptr;
}

static void OnOverlayDeleted(lv_event_t* /*e*/) {
    if (!s_host_alive) {
        return;
    }
    TeardownHostState();
}

static void OnScreenUnloaded(lv_event_t* /*e*/) {
    if (s_as_overlay) {
        return;
    }
    TeardownHostState();
}

static bool StandbyOnVkKey(const char* /*key_name*/) {
    return true;
}

lv_obj_t* StandbyScreen::Create() {
    EnsureStandbyPortrait();
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    ClearStatusBindings();
    s_as_overlay = false;
    s_host_alive = true;
    MountVariant(scr, false);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    VkKey_AttachScreen(scr, kScreenId,
                       VkKeyScreenDesc{StandbyScreen::Create, StandbyOnVkKey,
                                       StandbyScreen::HandleBootClick, StandbyScreen::HandleBootLongPress});
    return scr;
}

void StandbyScreen::Show() {
    if (s_host_alive) {
        return;
    }
    ScreenFrontlightHide();
    s_wake_request.store(0);
    lv_obj_t* under = lv_screen_active();
    if (under == nullptr) {
        ESP_LOGW(TAG, "show: no active screen");
        return;
    }
    ESP_LOGI(TAG, "show overlay on screen=%p", static_cast<void*>(under));
    if (!PreferWallpaperVariant()) {
        EnsureStandbyPortrait();
    }

    lv_obj_t* overlay = lv_obj_create(under);
    lv_obj_remove_style_all(overlay);
    // Standby covers the drawable screen, independently of the underlying
    // page's flex/grid placement and scroll offset.
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    s_underlying_scr = under;
    s_overlay_root = overlay;
    s_as_overlay = true;
    s_host_alive = true;
    s_paint_token.fetch_add(1, std::memory_order_relaxed);
    s_paint_flushed.store(0, std::memory_order_relaxed);

    MountVariant(overlay, true);
    lv_obj_move_foreground(overlay);
    lv_obj_add_event_cb(overlay, OnOverlayDeleted, LV_EVENT_DELETE, nullptr);

    // 前光关在 Park 上墨之后（见 ParkEpdForStandby），勿在此提前灭

    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->BeginStandbyEnterPaint();
    }

    WallpaperScreen::SetDecodePaused(true);
    BookScreen::OnEnterStandby();
    AssistantScreen::SetStandby(true);
    WorkBuddyScreen::SetStandby(true);
    // Stop the log producer before closing admission. Existing file handles and
    // the explicitly permitted standby decoder finish before SD is removed.
    SdLogWriter_OnSdUnmounting();
    sd_access::Quiesce();
    SettingsScreen_OnEnterStandby();
    SettingsTestBatteryScreen_PauseForStandby();
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->PauseStandbyBackgroundWork();
    }
    PowerPolicy::GetInstance().RequestReevaluate();
}

void StandbyScreen::Dismiss() {
    if (!s_host_alive) {
        return;
    }
    ExitEpdSleep();
    sd_access::Resume();
    SdLogWriter_OnSdMounted();
    ESP_LOGI(TAG, "dismiss overlay (under=%p variant=%u)", static_cast<void*>(s_underlying_scr),
             static_cast<unsigned>(s_variant));

    if (s_variant == StandbyVariant::Classic) {
        StandbyClassic_StopRuntime();
    }

    lv_obj_t* overlay = s_overlay_root;
    s_overlay_root = nullptr;
    s_host_alive = false;

    if (overlay != nullptr) {
        lv_obj_remove_event_cb(overlay, OnOverlayDeleted);
        // 先卸变体像素/timer，再删 overlay，避免 DELETE 回调二次 Teardown
        TeardownVariant();
        lv_obj_delete(overlay);
    } else {
        TeardownVariant();
    }

    s_as_overlay = false;
    s_underlying_scr = nullptr;

    DisplayUiSetFullScreen(false);
    PowerPolicy::GetInstance().OnStandbyOverlayDismissed();
    AssistantScreen::SetStandby(false);
    WorkBuddyScreen::SetStandby(false);

    // 清除待机前电量，待唤醒负载稳定 3s 后重新校准。
    Bq27220Gauge::GetInstance().ResetFilter();
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->UpdateStatusBar(true);
    }

    if (TaskScreen::IsActive()) {
        TaskScreen::OnResumeFromStandby();
    }
    BookScreen::OnResumeFromStandby();
    if (!reader::book_library_warmup::IsProgressCacheReady()) {
        reader::book_library_warmup::RequestBootWarmup();
    }
    SettingsScreen_OnResumeFromStandby();
    SettingsTestBatteryScreen_ResumeFromStandby();
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->ResumeStandbyBackgroundWork();
    }
    // 勿在此 FullRefresh：会堵 LVGL 数秒，浅醒后侧键同步易把仍按下的电源当成已满长按→关机图
    // 底层下一帧落墨后再开前光（见 RequestRestoreFrontlightAfterCommit）
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->RequestRestoreFrontlightAfterCommit();
        if (lv_screen_active() != nullptr) {
            lv_obj_invalidate(lv_screen_active());
        }
    }
    WallpaperScreen::SetDecodePaused(false);
    CloudScreen::ReloadApAfterStandby();
}

bool StandbyScreen::IsActive() {
    return s_host_alive;
}

void StandbyScreen::RequestWake(bool assistant) {
    s_wake_request.store(assistant ? 2 : 1, std::memory_order_release);
}

bool StandbyScreen::HasWakeRequest() {
    return s_wake_request.load(std::memory_order_acquire) != 0;
}

void StandbyScreen::FinishWake() {
    const unsigned request = s_wake_request.exchange(0);
    Dismiss();
    if (request == 2) AssistantScreen::RequestOpen();
}

bool StandbyScreen::IsPaintReady() {
    if (!s_host_alive) {
        return false;
    }
    if (s_variant == StandbyVariant::Wallpaper) {
        return StandbyWallpaper_IsContentReady();
    }
    // 经典页：须等至少一帧 flush 落地，避免 standby_lp 过早 Park 在 CPU0 上再次刷屏
    const uint32_t tok = s_paint_token.load(std::memory_order_relaxed);
    return tok != 0 && s_paint_flushed.load(std::memory_order_relaxed) == tok;
}

void StandbyScreen::NotifyFlushCompleted() {
    if (!s_host_alive) {
        return;
    }
    const uint32_t tok = s_paint_token.load(std::memory_order_relaxed);
    if (tok != 0) {
        s_paint_flushed.store(tok, std::memory_order_relaxed);
    }
}

static void ParkEpdAsync(void* /*arg*/) {
    if (auto* disp = LVAdapterDisplay::Instance()) {
        s_park_ok.store(disp->ParkEpdForStandby());
    }
    if (s_park_done != nullptr) {
        xSemaphoreGive(s_park_done);
    }
}

bool StandbyScreen::EnterEpdSleep()
{
    if (!s_host_alive) {
        return false;
    }
    // Park 刷屏放到 lv_epdiy_tick（CPU1）执行，避免 standby_lp（CPU0）堵死 IDLE0→WDT
    if (s_park_done == nullptr) {
        s_park_done = xSemaphoreCreateBinary();
    }
    if (s_park_done == nullptr) return false;
    s_park_ok.store(false);
    while (xSemaphoreTake(s_park_done, 0) == pdTRUE) {
    }
    ESP_LOGI(TAG, "park: queue on LVGL tick");
    bool queued = false;
    for (int i = 0; i < 40 && !queued; ++i) {
        queued = ScreenLvAsyncUrgent(ParkEpdAsync);
        if (!queued) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    if (!queued) {
        ESP_LOGW(TAG, "park: async fail, direct on caller");
        if (auto* disp = LVAdapterDisplay::Instance()) {
            return disp->ParkEpdForStandby();
        }
        return false;
    }
    // 等待已排队的回调结束，避免超时后重入 Park 或与唤醒竞争。
    xSemaphoreTake(s_park_done, portMAX_DELAY);
    return s_park_ok.load();
}

void StandbyScreen::ExitEpdSleep()
{
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->WakeEpdFromStandby();
    }
}

bool StandbyScreen::HandleBootClick() {
    if (!s_host_alive) {
        return false;
    }
    ESP_LOGI(TAG, "boot short -> ignore (stay standby)");
    return true;
}

bool StandbyScreen::HandleBootLongPress() {
    if (!s_host_alive) {
        return false;
    }
    ESP_LOGI(TAG, "boot long -> dismiss + assistant (hold-through)");
    RequestWake(true);
    return true;
}

void StandbyScreen::EnsureWeatherCached() {
    // 天气缓存与变体无关：开机仍灌，经典页可立刻用；壁纸页不读 UI。
    StandbyClassic_EnsureWeatherCached();
}

