#include "ota_upgrade_screen/ota_upgrade_screen.h"

#include "fontpack_lvgl.h"
#include "standby_screen/standby_screen.h"
#include "ui_scale.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <esp_lv_adapter.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lvgl.h>
#include "assets/lang_config.h"


static constexpr const char* TAG = "OtaUpgradeUi";

static constexpr lv_coord_t kBarW = UiSx(320);
static constexpr lv_coord_t kBarH = UiSy(28);
static constexpr lv_coord_t kPanelMargin = UiSx(48);
static constexpr lv_coord_t kLabelMargin = UiSx(64);
static constexpr lv_coord_t kPanelRowGap = UiSy(16);
static constexpr lv_coord_t kBarRadius = UiSx(6);
static constexpr lv_coord_t kBarIndRadius = UiSx(3);
static constexpr lv_coord_t kBarBorderW = UiSx(2);
static constexpr lv_coord_t kBarPad = UiSx(3);
// 局部刷新节流：0%/100% 和跨 5% 的更新必须立即刷新。
static constexpr int64_t kProgUiIntervalUs = 2 * 1000 * 1000;
static constexpr uint32_t kShowTimeoutMs = 5000;
static constexpr uint32_t kDismissTimeoutMs = 3000;
static constexpr size_t kVerCap = 48;
static constexpr size_t kHintCap = 64;

static std::atomic<bool> s_input_locked{false};
static std::atomic<bool> s_ui_alive{false};
static std::atomic<uint32_t> s_gen{0};

static lv_obj_t* s_root = nullptr;
static lv_obj_t* s_bar = nullptr;
static lv_obj_t* s_pct_lbl = nullptr;
static lv_obj_t* s_hint_lbl = nullptr;
static int s_shown_percent = -1;

static std::atomic<int> s_pending_percent{-1};
static std::atomic<bool> s_prog_queued{false};
static std::atomic<int64_t> s_last_prog_ui_us{0};

/** Show/Dismiss 分信号量：超时后迟到的 Show Give 不会误唤醒 Dismiss */
struct OtaUpgradeSyncSlot {
    bool ok = false;
    uint32_t gen = 0;
    char current[kVerCap] = {};
    char next[kVerCap] = {};
};
// Show/Dismiss 用分离信号量，避免超时后的旧唤醒打断当前流程。
static OtaUpgradeSyncSlot s_sync{};
static SemaphoreHandle_t s_show_done = nullptr;
static SemaphoreHandle_t s_dismiss_done = nullptr;

struct OtaUpgradeHintMsg {
    uint32_t gen = 0;
    char text[kHintCap] = {};
};

static const lv_font_t* UiFont() {
    const lv_font_t* f = fontpack_lv_font_ui();
    return f != nullptr ? f : LV_FONT_DEFAULT;
}
static void CopyCapped(char* dst, size_t cap, const char* src) {
    if (dst == nullptr || cap == 0) {
        return;
    }
    if (src == nullptr || src[0] == '\0') {
        std::snprintf(dst, cap, "%s", "—");
        return;
    }
    std::snprintf(dst, cap, "%s", src);
}

static bool EnsureSyncSems() {
    if (s_show_done == nullptr) {
        s_show_done = xSemaphoreCreateBinary();
    }
    if (s_dismiss_done == nullptr) {
        s_dismiss_done = xSemaphoreCreateBinary();
    }
    return s_show_done != nullptr && s_dismiss_done != nullptr;
}

static void DrainSem(SemaphoreHandle_t sem) {
    if (sem == nullptr) {
        return;
    }
    while (xSemaphoreTake(sem, 0) == pdTRUE) {
    }
}

static void ClearWidgetPtrs() {
    s_root = nullptr;
    s_bar = nullptr;
    s_pct_lbl = nullptr;
    s_hint_lbl = nullptr;
    s_shown_percent = -1;
}

static void TeardownUi() {
    lv_obj_t* root = s_root;
    ClearWidgetPtrs();
    s_ui_alive.store(false, std::memory_order_release);
    if (root != nullptr && lv_obj_is_valid(root)) {
        lv_obj_delete(root);
    }
}

static void ApplyPercentLocked(int percent) {
    if (!s_ui_alive.load(std::memory_order_acquire) || s_bar == nullptr || s_pct_lbl == nullptr) {
        return;
    }
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    if (percent == s_shown_percent) {
        return;
    }
    s_shown_percent = percent;
    lv_bar_set_value(s_bar, percent, LV_ANIM_OFF);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d%%", percent);
    lv_label_set_text(s_pct_lbl, buf);
}

static void BuildUi(const char* current_ver, const char* new_ver) {
    TeardownUi();

    // 待机若冻结 EPD flush，进度无法上屏
    if (StandbyScreen::IsActive()) {
        StandbyScreen::ExitEpdSleep();
    }

    lv_obj_t* parent = lv_layer_top();
    if (parent == nullptr) {
        ESP_LOGE(TAG, "lv_layer_top null");
        return;
    }

    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(root);

    lv_obj_t* panel = lv_obj_create(root);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, LV_HOR_RES - kPanelMargin, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(panel, kPanelRowGap, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(panel);

    const lv_font_t* font = UiFont();

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, Lang::Strings::OTA_TITLE);
    lv_obj_set_style_text_font(title, font, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    char line[kVerCap + 16];
    lv_obj_t* cur_lbl = lv_label_create(panel);
    std::snprintf(line, sizeof(line), Lang::Strings::OTA_CUR_VER_FMT, current_ver);
    lv_label_set_text(cur_lbl, line);
    lv_obj_set_width(cur_lbl, LV_HOR_RES - kLabelMargin);
    lv_label_set_long_mode(cur_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(cur_lbl, font, 0);
    lv_obj_set_style_text_color(cur_lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(cur_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(cur_lbl, LV_OBJ_FLAG_CLICKABLE);

    // new_ver 为空：SD 本地升级，不展示「最新版本」
    if (new_ver != nullptr && new_ver[0] != '\0') {
        lv_obj_t* new_lbl = lv_label_create(panel);
        std::snprintf(line, sizeof(line), Lang::Strings::OTA_NEW_VER_FMT, new_ver);
        lv_label_set_text(new_lbl, line);
        lv_obj_set_width(new_lbl, LV_HOR_RES - kLabelMargin);
        lv_label_set_long_mode(new_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(new_lbl, font, 0);
        lv_obj_set_style_text_color(new_lbl, lv_color_black(), 0);
        lv_obj_set_style_text_align(new_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_clear_flag(new_lbl, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_coord_t bar_w = kBarW;
    if (bar_w > LV_HOR_RES - kLabelMargin) {
        bar_w = LV_HOR_RES - kLabelMargin;
    }
    lv_obj_t* bar = lv_bar_create(panel);
    lv_obj_set_size(bar, bar_w, kBarH);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_mode(bar, LV_BAR_MODE_NORMAL);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, kBarRadius, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, kBarBorderW, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, kBarPad, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, kBarIndRadius, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* pct = lv_label_create(panel);
    lv_label_set_text(pct, "0%");
    lv_obj_set_style_text_font(pct, font, 0);
    lv_obj_set_style_text_color(pct, lv_color_black(), 0);
    lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(pct, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* hint = lv_label_create(panel);
    lv_label_set_text(hint, Lang::Strings::OTA_HINT);
    lv_obj_set_width(hint, LV_HOR_RES - 64);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(hint, font, 0);
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);

    s_root = root;
    s_bar = bar;
    s_pct_lbl = pct;
    s_hint_lbl = hint;
    s_shown_percent = 0;
    s_ui_alive.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "overlay shown cur=%s new=%s", current_ver, new_ver);
}

static bool LvAsyncCallLocked(lv_async_cb_t cb, void* user_data) {
    if (cb == nullptr) {
        return false;
    }
    if (!esp_lv_adapter_is_initialized()) {
        ESP_LOGW(TAG, "adapter not ready");
        return false;
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        ESP_LOGW(TAG, "adapter lock failed");
        return false;
    }
    const bool ok = (lv_async_call(cb, user_data) == LV_RESULT_OK);
    esp_lv_adapter_unlock();
    if (!ok) {
        ESP_LOGW(TAG, "lv_async_call failed");
    }
    return ok;
}

static void AsyncShow(void* p) {
    auto* slot = static_cast<OtaUpgradeSyncSlot*>(p);
    if (slot == nullptr) {
        return;
    }
    // 过期 Show（已被更新 gen / 已 Dismiss）直接跳过建页
    if (slot->gen == s_gen.load(std::memory_order_acquire) &&
        s_input_locked.load(std::memory_order_acquire)) {
        BuildUi(slot->current, slot->next);
    }
    slot->ok = s_ui_alive.load(std::memory_order_acquire);
    if (s_show_done != nullptr) {
        xSemaphoreGive(s_show_done);
    }
}

static void AsyncDismiss(void* p) {
    auto* slot = static_cast<OtaUpgradeSyncSlot*>(p);
    TeardownUi();
    if (slot != nullptr) {
        slot->ok = true;
    }
    if (s_dismiss_done != nullptr) {
        xSemaphoreGive(s_dismiss_done);
    }
}

static void AsyncApplyProgress(void* /*p*/) {
    s_prog_queued.store(false, std::memory_order_release);
    if (!s_ui_alive.load(std::memory_order_acquire)) {
        return;
    }
    const int percent = s_pending_percent.exchange(-1, std::memory_order_acq_rel);
    if (percent < 0) {
        return;
    }
    ApplyPercentLocked(percent);

    // 已在 LVGL 任务：禁止再取 adapter lock（可能非递归死锁）
    const int again = s_pending_percent.load(std::memory_order_acquire);
    if (again >= 0 && !s_prog_queued.exchange(true, std::memory_order_acq_rel)) {
        if (lv_async_call(AsyncApplyProgress, nullptr) != LV_RESULT_OK) {
            s_prog_queued.store(false, std::memory_order_release);
        }
    }
}

static void AsyncHint(void* p) {
    auto* msg = static_cast<OtaUpgradeHintMsg*>(p);
    if (msg == nullptr) {
        return;
    }
    if (msg->gen == s_gen.load(std::memory_order_acquire) && s_ui_alive.load(std::memory_order_acquire) &&
        s_hint_lbl != nullptr && lv_obj_is_valid(s_hint_lbl)) {
        lv_label_set_text(s_hint_lbl, msg->text);
    }
    delete msg;
}

static bool WaitSem(SemaphoreHandle_t sem, uint32_t timeout_ms) {
    if (sem == nullptr) {
        return false;
    }
    const bool got = (xSemaphoreTake(sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE);
    if (!got) {
        ESP_LOGW(TAG, "wait UI timeout %ums", static_cast<unsigned>(timeout_ms));
        return false;
    }
    return s_sync.ok;
}


bool OtaUpgradeScreen::Show(const char* current_version, const char* new_version) {
    // 先锁键，再建 UI：即使建页失败也不应让用户乱点
    s_input_locked.store(true, std::memory_order_release);
    const uint32_t gen = s_gen.fetch_add(1, std::memory_order_acq_rel) + 1;
    s_pending_percent.store(0, std::memory_order_release);
    s_last_prog_ui_us.store(0, std::memory_order_release);
    s_prog_queued.store(false, std::memory_order_release);

    if (!EnsureSyncSems()) {
        ESP_LOGE(TAG, "Show: no semaphore");
        return false;
    }
    DrainSem(s_show_done);
    s_sync.ok = false;
    s_sync.gen = gen;
    CopyCapped(s_sync.current, sizeof(s_sync.current), current_version);
    // 空串保留为空：SD 升级不展示「最新版本」行（勿落成 "—"）
    if (new_version == nullptr || new_version[0] == '\0') {
        s_sync.next[0] = '\0';
    } else {
        CopyCapped(s_sync.next, sizeof(s_sync.next), new_version);
    }

    if (!LvAsyncCallLocked(AsyncShow, &s_sync)) {
        return false;
    }

    const bool ok = WaitSem(s_show_done, kShowTimeoutMs);
    if (!ok) {
        ESP_LOGW(TAG, "Show incomplete; keys still locked");
    }
    return ok;
}

void OtaUpgradeScreen::SetProgress(int percent) {
    if (!s_input_locked.load(std::memory_order_acquire)) {
        return;
    }
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    s_pending_percent.store(percent, std::memory_order_release);

    const int64_t now_us = esp_timer_get_time();
    const int64_t last_us = s_last_prog_ui_us.load(std::memory_order_relaxed);
    const int shown = s_shown_percent;
    const bool force = (percent <= 0) || (percent >= 100) || (shown >= 0 && (percent - shown) >= 5) ||
                       (shown < 0);
    if (!force && last_us != 0 && (now_us - last_us) < kProgUiIntervalUs) {
        return;
    }
    s_last_prog_ui_us.store(now_us, std::memory_order_relaxed);

    if (s_prog_queued.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (!LvAsyncCallLocked(AsyncApplyProgress, nullptr)) {
        s_prog_queued.store(false, std::memory_order_release);
    }
}

void OtaUpgradeScreen::SetHint(const char* hint) {
    if (!s_input_locked.load(std::memory_order_acquire) || hint == nullptr) {
        return;
    }
    auto* msg = new OtaUpgradeHintMsg{};
    msg->gen = s_gen.load(std::memory_order_acquire);
    std::snprintf(msg->text, sizeof(msg->text), "%s", hint);
    if (!LvAsyncCallLocked(AsyncHint, msg)) {
        delete msg;
    }
}

void OtaUpgradeScreen::Dismiss() {
    if (!s_input_locked.load(std::memory_order_acquire) &&
        !s_ui_alive.load(std::memory_order_acquire)) {
        return;
    }

    s_gen.fetch_add(1, std::memory_order_acq_rel);
    s_pending_percent.store(-1, std::memory_order_release);
    s_prog_queued.store(false, std::memory_order_release);

    if (!EnsureSyncSems()) {
        ESP_LOGE(TAG, "Dismiss: no semaphore; force unlock");
        s_input_locked.store(false, std::memory_order_release);
        return;
    }
    DrainSem(s_dismiss_done);
    s_sync.ok = false;
    s_sync.gen = s_gen.load(std::memory_order_acquire);

    if (!LvAsyncCallLocked(AsyncDismiss, &s_sync)) {
        s_input_locked.store(false, std::memory_order_release);
        return;
    }

    WaitSem(s_dismiss_done, kDismissTimeoutMs);
    // 无论 UI 是否拆干净，都必须放行按键，避免永久锁死
    s_input_locked.store(false, std::memory_order_release);
    ESP_LOGI(TAG, "dismissed");
}

bool OtaUpgradeScreen::IsActive() {
    return s_input_locked.load(std::memory_order_acquire);
}
