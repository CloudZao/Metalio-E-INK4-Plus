#include "ota_confirm_dialog/ota_confirm_dialog.h"

#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "ota_upgrade_screen/ota_upgrade_screen.h"
#include "screen_common.h"
#include "standby_screen/standby_screen.h"
#include "ui_scale.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include <esp_log.h>
#include <esp_lv_adapter.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lvgl.h>
#include "assets/lang_config.h"


static constexpr const char* TAG = "OtaConfirmDlg";
static constexpr uint32_t kShowTimeoutMs = 120000;  // 等用户操作，最长 2 分钟
static constexpr size_t kVerCap = 48;
static constexpr size_t kMsgCap = 160;
static constexpr lv_coord_t kBorderW = UiSx(2);
static constexpr lv_coord_t kRadius = UiSx(8);
static constexpr lv_coord_t kBtnH = UiSy(44);
static constexpr lv_coord_t kCardMargin = UiSx(48);
static constexpr lv_coord_t kCardHBoot = UiSy(320);
static constexpr lv_coord_t kCardHNormal = UiSy(240);
static constexpr lv_coord_t kCardPad = UiSx(14);
static constexpr lv_coord_t kCardRowGap = UiSy(12);
static constexpr lv_coord_t kBtnRowExtra = UiSy(4);
static constexpr lv_coord_t kBtnWCompact = UiSx(120);
static constexpr lv_coord_t kBtnMargin = UiSx(96);
static constexpr lv_coord_t kColRowGap = UiSy(10);

static std::atomic<bool> s_input_locked{false};
static std::atomic<bool> s_ui_alive{false};
static std::atomic<uint32_t> s_gen{0};

static lv_obj_t* s_root = nullptr;

static SemaphoreHandle_t s_choice_done = nullptr;

struct OtaConfirmSyncState {
    uint32_t gen = 0;
    OtaConfirmDialog::Mode mode = OtaConfirmDialog::Mode::Boot;
    OtaConfirmDialog::Choice choice = OtaConfirmDialog::Choice::RemindLater;
    bool ok = false;
    char current[kVerCap] = {};
    char next[kVerCap] = {};
};
static OtaConfirmSyncState s_sync{};

static std::unique_ptr<OtaConfirmDialog::ChoiceCallback> s_async_cb;

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

static bool EnsureSems() {
    if (s_choice_done == nullptr) {
        s_choice_done = xSemaphoreCreateBinary();
    }
    return s_choice_done != nullptr;
}

static void DrainSem(SemaphoreHandle_t sem) {
    if (sem == nullptr) {
        return;
    }
    while (xSemaphoreTake(sem, 0) == pdTRUE) {
    }
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

static void ClearWidgetPtrs() {
    s_root = nullptr;
}

static void TeardownUi() {
    lv_obj_t* root = s_root;
    ClearWidgetPtrs();
    s_ui_alive.store(false, std::memory_order_release);
    if (root != nullptr && lv_obj_is_valid(root)) {
        lv_obj_delete(root);
    }
}

void FinishChoice(OtaConfirmDialog::Choice choice) {
    if (!s_input_locked.load(std::memory_order_acquire)) {
        return;
    }
    const uint32_t gen = s_gen.load(std::memory_order_acquire);
    if (s_sync.gen != gen) {
        return;
    }

    s_sync.choice = choice;
    TeardownUi();
    s_input_locked.store(false, std::memory_order_release);

    auto cb = std::move(s_async_cb);
    s_async_cb.reset();

    if (s_choice_done != nullptr) {
        xSemaphoreGive(s_choice_done);
    }

    if (cb && *cb) {
        (*cb)(choice);
    }
}

static void OnMaskClicked(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    FinishChoice(OtaConfirmDialog::Choice::RemindLater);
}

static void OnUpgradeClicked(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    FinishChoice(OtaConfirmDialog::Choice::Upgrade);
}

static void OnIgnoreClicked(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    FinishChoice(OtaConfirmDialog::Choice::IgnorePersist);
}

static void OnRemindClicked(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    FinishChoice(OtaConfirmDialog::Choice::RemindLater);
}

static lv_obj_t* MakeButton(lv_obj_t* parent, const char* text, lv_event_cb_t cb, lv_coord_t width) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, width, kBtnH);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, kBorderW, 0);
    lv_obj_set_style_radius(btn, kRadius, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, UiFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

void BuildUi(const char* current_ver, const char* new_ver, OtaConfirmDialog::Mode mode) {
    TeardownUi();

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
    ScreenApplyDotBackdrop(root);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, OnMaskClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_move_foreground(root);

    const bool boot = (mode == OtaConfirmDialog::Mode::Boot);
    const bool sd = (mode == OtaConfirmDialog::Mode::Sd);
    const lv_coord_t card_h = boot ? kCardHBoot : kCardHNormal;

    lv_obj_t* card = lv_obj_create(root);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_HOR_RES - kCardMargin, card_h);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, kBorderW, 0);
    lv_obj_set_style_radius(card, kRadius, 0);
    lv_obj_set_style_pad_all(card, kCardPad, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, kCardRowGap, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, [](lv_event_t* e) { lv_event_stop_bubbling(e); }, LV_EVENT_CLICKED,
                        nullptr);

    char msg[kMsgCap];
    if (sd) {
        std::snprintf(msg, sizeof(msg), Lang::Strings::OTA_SD_CONFIRM_FMT, current_ver);
    } else {
        std::snprintf(msg, sizeof(msg), Lang::Strings::OTA_CONFIRM_FMT, current_ver, new_ver);
    }

    lv_obj_t* lbl = lv_label_create(card);
    lv_label_set_text(lbl, msg);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl, UiFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    if (boot) {
        lv_obj_t* col = lv_obj_create(card);
        lv_obj_remove_style_all(col);
        lv_obj_set_width(col, lv_pct(100));
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(col, kColRowGap, 0);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);

        const lv_coord_t btn_w = LV_HOR_RES - kBtnMargin;
        MakeButton(col, Lang::Strings::OTA_UPGRADE_NOW, OnUpgradeClicked, btn_w);
        MakeButton(col, Lang::Strings::OTA_IGNORE_VERSION, OnIgnoreClicked, btn_w);
        MakeButton(col, Lang::Strings::OTA_REMIND_LATER, OnRemindClicked, btn_w);
    } else {
        lv_obj_t* row = lv_obj_create(card);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, kBtnH + kBtnRowExtra);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

        MakeButton(row, Lang::Strings::OTA_UPGRADE_NOW, OnUpgradeClicked, kBtnWCompact);
        MakeButton(row, Lang::Strings::COMMON_CANCEL, OnRemindClicked, kBtnWCompact);
    }

    s_root = root;
    s_ui_alive.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "shown mode=%s cur=%s new=%s", boot ? "boot" : (sd ? "sd" : "about"),
             current_ver, new_ver);
}

struct OtaConfirmBuildMsg {
    uint32_t gen = 0;
    OtaConfirmDialog::Mode mode = OtaConfirmDialog::Mode::Boot;
    char current[kVerCap] = {};
    char next[kVerCap] = {};
    bool signal_choice_sem = false;
};

static void AsyncBuild(void* p) {
    auto* msg = static_cast<OtaConfirmBuildMsg*>(p);
    if (msg == nullptr) {
        return;
    }
    const bool still = (msg->gen == s_gen.load(std::memory_order_acquire)) &&
                       s_input_locked.load(std::memory_order_acquire);
    if (still) {
        BuildUi(msg->current, msg->next, msg->mode);
    }
    if (msg->signal_choice_sem && s_choice_done != nullptr &&
        (!still || !s_ui_alive.load(std::memory_order_acquire))) {
        // 建页失败：放行阻塞等待，按 RemindLater
        s_sync.choice = OtaConfirmDialog::Choice::RemindLater;
        s_input_locked.store(false, std::memory_order_release);
        xSemaphoreGive(s_choice_done);
    }
    delete msg;
}

bool BeginShow(const char* current_version, const char* new_version, OtaConfirmDialog::Mode mode,
               bool blocking, OtaConfirmDialog::ChoiceCallback cb) {
    if (OtaUpgradeScreen::IsActive() || OtaConfirmDialog::IsActive()) {
        ESP_LOGW(TAG, "BeginShow rejected: overlay busy");
        return false;
    }
    if (!EnsureSems()) {
        ESP_LOGE(TAG, "no semaphore");
        return false;
    }

    s_input_locked.store(true, std::memory_order_release);
    const uint32_t gen = s_gen.fetch_add(1, std::memory_order_acq_rel) + 1;
    s_sync.gen = gen;
    s_sync.mode = mode;
    s_sync.choice = OtaConfirmDialog::Choice::RemindLater;
    s_sync.ok = false;
    CopyCapped(s_sync.current, sizeof(s_sync.current), current_version);
    CopyCapped(s_sync.next, sizeof(s_sync.next), new_version);

    if (blocking) {
        s_async_cb.reset();
        DrainSem(s_choice_done);
    } else {
        s_async_cb = std::make_unique<OtaConfirmDialog::ChoiceCallback>(std::move(cb));
    }

    // About：调用方已在 LVGL 任务（点击回调）→ 同步建页，避免 adapter 锁自死锁
    if (!blocking) {
        BuildUi(s_sync.current, s_sync.next, mode);
        if (!s_ui_alive.load(std::memory_order_acquire)) {
            s_input_locked.store(false, std::memory_order_release);
            s_async_cb.reset();
            return false;
        }
        return true;
    }

    auto* msg = new (std::nothrow) OtaConfirmBuildMsg{};
    if (msg == nullptr) {
        s_input_locked.store(false, std::memory_order_release);
        return false;
    }
    msg->gen = gen;
    msg->mode = mode;
    msg->signal_choice_sem = true;
    CopyCapped(msg->current, sizeof(msg->current), current_version);
    CopyCapped(msg->next, sizeof(msg->next), new_version);

    if (!LvAsyncCallLocked(AsyncBuild, msg)) {
        delete msg;
        s_input_locked.store(false, std::memory_order_release);
        return false;
    }
    return true;
}


OtaConfirmDialog::Choice OtaConfirmDialog::ShowBlocking(const char* current_version,
                                                        const char* new_version, Mode mode) {
    if (!BeginShow(current_version, new_version, mode, true, nullptr)) {
        return Choice::RemindLater;
    }
    if (s_choice_done == nullptr) {
        s_input_locked.store(false, std::memory_order_release);
        return Choice::RemindLater;
    }
    const bool got = (xSemaphoreTake(s_choice_done, pdMS_TO_TICKS(kShowTimeoutMs)) == pdTRUE);
    if (!got) {
        ESP_LOGW(TAG, "ShowBlocking timeout → RemindLater");
        Dismiss();
        return Choice::RemindLater;
    }
    return s_sync.choice;
}

bool OtaConfirmDialog::ShowAsync(const char* current_version, const char* new_version, Mode mode,
                                 ChoiceCallback cb) {
    if (!cb) {
        return false;
    }
    return BeginShow(current_version, new_version, mode, false, std::move(cb));
}

void OtaConfirmDialog::Dismiss() {
    if (!s_input_locked.load(std::memory_order_acquire) &&
        !s_ui_alive.load(std::memory_order_acquire)) {
        return;
    }
    s_gen.fetch_add(1, std::memory_order_acq_rel);

    auto finish = []() {
        TeardownUi();
        s_input_locked.store(false, std::memory_order_release);
        s_async_cb.reset();
        if (s_choice_done != nullptr) {
            xSemaphoreGive(s_choice_done);
        }
    };

    // 已在 LVGL（如切关于 tab）：adapter 非阻塞锁失败则直接拆，避免自死锁
    if (esp_lv_adapter_is_initialized()) {
        if (esp_lv_adapter_lock(0) == ESP_OK) {
            finish();
            esp_lv_adapter_unlock();
            ESP_LOGI(TAG, "dismissed (locked)");
            return;
        }
        finish();
        ESP_LOGI(TAG, "dismissed (lvgl nested)");
        return;
    }

    finish();
    ESP_LOGI(TAG, "dismissed");
}

bool OtaConfirmDialog::IsActive() {
    return s_input_locked.load(std::memory_order_acquire);
}
