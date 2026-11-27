#include <atomic>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lvgl.h>
#include "business/conversation/conversation_presentation.h"
#include "ui/apps/assistant/assistant_screen.h"
#include "ui/common/widgets/screen_frontlight.h"
#include "ui/apps/standby/standby_screen.h"
#include "ui/apps/standby/standby_classic.h"
#include "sysmgmt/power/standby_wallpaper.h"
#include "business/wallpaper/wallpaper_active.h"
#include "sysmgmt/power/power_lifecycle.h"
#include "ui/common/drawing/display_orient.h"
#include "ui/common/drawing/adapter/epd_refresh.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/common/drawing/rendering/wallpaper_frame.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"

static constexpr const char* TAG = "StandbyScreen";
static constexpr const char* kScreenId = "standby";

enum class StandbyVariant : uint8_t { None = 0, Classic = 1, Wallpaper = 2 };

static bool s_as_overlay = false;
static lv_obj_t* s_overlay_root = nullptr;
static lv_obj_t* s_underlying_scr = nullptr;
static StandbyVariant s_variant = StandbyVariant::None;
static std::atomic<uint32_t> s_paint_token{0};
static std::atomic<uint32_t> s_paint_flushed{0};
static SemaphoreHandle_t s_park_done = nullptr;
static std::atomic<bool> s_park_ok{false};
static std::atomic<unsigned> s_wake_request{0};
static uint64_t s_standby_started_ms = 0; // 当前待机开始时刻，由界面上下文维护
static uint64_t s_standby_total_ms = 0; // 已结束的待机累计时长
static bool s_standby_timing = false;

static void BeginStandbyTiming() {
    if (!s_standby_timing) {
        s_standby_started_ms = esp_timer_get_time() / 1000;
        s_standby_timing = true;
    }
}

static void EndStandbyTiming() {
    if (s_standby_timing) {
        s_standby_total_ms += esp_timer_get_time() / 1000 - s_standby_started_ms;
        s_standby_timing = false;
    }
}

uint64_t StandbyPresentation::TotalDurationMs() {
    return s_standby_total_ms + (s_standby_timing ? esp_timer_get_time() / 1000 - s_standby_started_ms : 0);
}

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
    EndStandbyTiming();
    TeardownVariant();
    WallpaperFrame_Clear();
    DisplayUiSetFullScreen(false);
    PowerLifecycleSetStandby(false);
    s_as_overlay = false;
    s_overlay_root = nullptr;
    s_underlying_scr = nullptr;
}

static void OnOverlayDeleted(lv_event_t* /*e*/) {
    if (!PowerLifecycleIsStandby()) {
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
    BeginStandbyTiming();
    EnsureStandbyPortrait();
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    ClearStatusBindings();
    s_as_overlay = false;
    PowerLifecycleSetStandby(true);
    MountVariant(scr, false);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    VkKey_AttachScreen(scr, kScreenId,
                       VkKeyScreenDesc{StandbyScreen::Create, StandbyOnVkKey, StandbyPresentation::HandleBootClick,
                                       StandbyPresentation::HandleBootLongPress});
    return scr;
}

void StandbyPresentation::Show() {
    if (PowerLifecycleIsStandby()) {
        return;
    }
    ScreenFrontlightHide();
    s_wake_request.store(0);
    PowerLifecycleSetStandbyResuming(false);
    lv_obj_t* under = lv_screen_active();
    if (under == nullptr) {
        ESP_LOGW(TAG, "show: no active screen");
        return;
    }
    PowerLifecyclePrepareStandbyOverlay();
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
    BeginStandbyTiming();
    PowerLifecycleSetStandby(true);
    s_paint_token.fetch_add(1, std::memory_order_relaxed);
    s_paint_flushed.store(0, std::memory_order_relaxed);

    MountVariant(overlay, true);
    lv_obj_move_foreground(overlay);
    lv_obj_add_event_cb(overlay, OnOverlayDeleted, LV_EVENT_DELETE, nullptr);

    // 前光关在 Park 上墨之后（见 ParkEpdForStandby），勿在此提前灭

    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->BeginStandbyEnterPaint();
    }

    PowerLifecyclePauseStandbyActivities();
}

void StandbyPresentation::Dismiss() {
    if (!PowerLifecycleIsStandby()) {
        return;
    }
    ExitEpdSleep();
    // 唤醒推白帧完成前保持恢复窗口，前台应用业务等到画面落墨后再继续。
    PowerLifecycleSetStandbyResuming(true);
    PowerLifecycleResumeStandbyStorage();
    ESP_LOGI(TAG, "dismiss overlay (under=%p variant=%u)", static_cast<void*>(s_underlying_scr),
             static_cast<unsigned>(s_variant));

    if (s_variant == StandbyVariant::Classic) {
        StandbyClassic_StopRuntime();
    }

    lv_obj_t* overlay = s_overlay_root;
    s_overlay_root = nullptr;
    EndStandbyTiming();
    PowerLifecycleSetStandby(false);

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
    PowerLifecycleResumeStandbyActivities();
    // 勿在此 FullRefresh：会堵 LVGL 数秒，浅醒后侧键同步易把仍按下的电源当成已满长按→关机图
    // 底层下一帧落墨后再开前光（见 RequestRestoreFrontlightAfterCommit）
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->RequestRestoreFrontlightAfterCommit();
        if (lv_screen_active() != nullptr) {
            lv_obj_invalidate(lv_screen_active());
        }
    }
    PowerLifecycleFinishStandbyResume();
}

bool StandbyPresentation::IsActive() {
    return PowerLifecycleIsStandby();
}

bool StandbyPresentation::IsResuming() {
    return PowerLifecycleIsStandbyResuming();
}

void StandbyPresentation::RequestWake(bool assistant) {
    s_wake_request.store(assistant ? 2 : 1, std::memory_order_release);
}

bool StandbyPresentation::HasWakeRequest() {
    return s_wake_request.load(std::memory_order_acquire) != 0;
}

void StandbyPresentation::FinishWake() {
    const unsigned request = s_wake_request.exchange(0);
    Dismiss();
    if (request == 2) {
        ConversationPresentation::RequestOpen();
    }
}

bool StandbyPresentation::IsPaintReady() {
    if (!PowerLifecycleIsStandby()) {
        return false;
    }
    if (s_variant == StandbyVariant::Wallpaper) {
        return StandbyWallpaper_IsContentReady();
    }
    // 经典页：须等至少一帧 flush 落地，避免 standby_lp 过早 Park 在 CPU0 上再次刷屏
    const uint32_t tok = s_paint_token.load(std::memory_order_relaxed);
    return tok != 0 && s_paint_flushed.load(std::memory_order_relaxed) == tok;
}

void StandbyPresentation::NotifyFlushCompleted() {
    if (!PowerLifecycleIsStandby()) {
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

bool StandbyPresentation::EnterEpdSleep() {
    if (!PowerLifecycleIsStandby()) {
        return false;
    }
    // Park 刷屏放到 lv_epdiy_tick（CPU1）执行，避免 standby_lp（CPU0）堵死 IDLE0→WDT
    if (s_park_done == nullptr) {
        s_park_done = xSemaphoreCreateBinary();
    }
    if (s_park_done == nullptr) {
        return false;
    }
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

void StandbyPresentation::ExitEpdSleep() {
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->WakeEpdFromStandby();
    }
}

bool StandbyPresentation::HandleBootClick() {
    if (!PowerLifecycleIsStandby()) {
        return false;
    }
    ESP_LOGI(TAG, "boot short -> ignore (stay standby)");
    return true;
}

bool StandbyPresentation::HandleBootLongPress() {
    if (!PowerLifecycleIsStandby()) {
        return false;
    }
    ESP_LOGI(TAG, "boot long -> dismiss + assistant (hold-through)");
    RequestWake(true);
    return true;
}
