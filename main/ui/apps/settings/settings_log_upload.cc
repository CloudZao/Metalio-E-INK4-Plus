#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include "ui/apps/settings/settings_log_upload.h"
#include "assets/lang_config.h"
#include "business/diagnostics/log_upload.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/runtime/modal.h"
#include "ui/common/drawing/ui_scale.h"
#include "ui/common/drawing/ui_theme.h"

static constexpr const char* TAG = "SettingsLogUp";
static std::atomic<bool> s_active{false};

struct SettingsLogUploadUploadUi {
    lv_obj_t* page = nullptr;
    lv_obj_t* title = nullptr;
    lv_obj_t* percent = nullptr;
    lv_obj_t* filename = nullptr;
    lv_obj_t* count = nullptr;
    unsigned shown_jobs = ~0U;
    lv_obj_t* bar = nullptr;
    lv_timer_t* timer = nullptr;
    LogUploadSessionPtr session;
    int shown_percent = -1;
    const char* shown_title = nullptr;
    int64_t done_us = 0;
} s_ui;

static UiModal s_window_modal;

static void StopUiTimer() {
    if (s_ui.timer) {
        lv_timer_delete(s_ui.timer);
        s_ui.timer = nullptr;
    }
}

static void PageDeleted(lv_event_t*) {
    if (s_ui.session && s_ui.session->running.load()) {
        s_ui.session->cancel.store(true);
    }
    StopUiTimer();
    s_ui = {};
    s_active.store(false);
    HapticSetZoneEarlyPulseEnabled(true);
}

static void ClosePage() {
    if (s_ui.page) {
        lv_obj_delete(s_ui.page);  // DELETE callback invalidates timer/pointers
    }
}

static void CancelClicked(lv_event_t*) {
    LogUploadCancel();
    ClosePage();
}

static void Poll(lv_timer_t*) {
    const auto& session = s_ui.session;
    if (!session) {
        return;
    }
    const char* title;
    bool running = session->running.load(std::memory_order_acquire);
    if (running) {
        if (session->cancel.load()) {
            title = LangString_SETTINGS_LOG_CANCELLING;
        } else if (session->phase.load() == LogUploadPhase::Snapshot) {
            title = LangString_SETTINGS_LOG_PREPARING;
        } else if (session->phase.load() == LogUploadPhase::WaitingNetwork) {
            title = LangString_SETTINGS_LOG_CONNECTING_NET;
        } else {
            title = LangString_SETTINGS_LOG_UPLOADING;
        }
    } else {
        switch (session->result.load()) {
            case LogUploadResult::Ok:
                title = LangString_SETTINGS_LOG_UPLOAD_OK;
                break;
            case LogUploadResult::Cancelled:
                title = LangString_CLOUD_CANCELLED;
                break;
            case LogUploadResult::NoFile:
                title = LangString_SETTINGS_LOG_UPLOAD_NO_FILE;
                break;
            case LogUploadResult::NetFailed:
                title = LangString_SETTINGS_CONV_NET_NOT_READY;
                break;
            case LogUploadResult::StorageUnavailable:
                title = LangString_SETTINGS_LOG_SD_UNAVAILABLE;
                break;
            case LogUploadResult::NoMemory:
                title = LangString_SETTINGS_LOG_NO_MEMORY;
                break;
            default:
                title = LangString_SETTINGS_LOG_UPLOAD_FAIL;
                break;
        }
        if (!s_ui.done_us) {
            s_ui.done_us = esp_timer_get_time();
        }
        if (esp_timer_get_time() - s_ui.done_us >= 2000000) {
            ClosePage();
            return;
        }
    }
    if (title != s_ui.shown_title) {
        lv_label_set_text(s_ui.title, title);
        s_ui.shown_title = title;
    }
    const unsigned jobs = session->jobs.load();
    if (jobs != s_ui.shown_jobs) {
        const unsigned total = jobs & 0xffU;
        const unsigned done = (jobs >> 8) & 0xffU;
        const unsigned file = (jobs >> 16) & 0xffU;
        lv_label_set_text(s_ui.filename, file == 1 ? "metalio_logs.txt" : file == 2 ? "metalio_coredump.elf" : "");
        char text[96];
        snprintf(text, sizeof(text), LangString_SETTINGS_LOG_UPLOAD_COUNT_FMT, done, total >= done ? total - done : 0);
        lv_label_set_text(s_ui.count, text);
        s_ui.shown_jobs = jobs;
    }
    int percent = !running && session->result.load() == LogUploadResult::Ok ? 100 : session->percent.load();
    if (percent != s_ui.shown_percent) {
        char text[16];
        snprintf(text, sizeof(text), "%d%%", percent);
        lv_label_set_text(s_ui.percent, text);
        lv_bar_set_value(s_ui.bar, percent, LV_ANIM_OFF);
        s_ui.shown_percent = percent;
    }
}

static bool ShowPage(const LogUploadSessionPtr& session) {
    if (s_ui.page) {
        return true;
    }
    lv_obj_t* parent = lv_screen_active();
    if (!parent) {
        return false;
    }
    s_ui.session = session;
    s_ui.page = lv_obj_create(parent);
    s_window_modal.Bind(s_ui.page, [](void*) { CancelClicked(nullptr); });
    lv_obj_remove_style_all(s_ui.page);
    lv_obj_set_size(s_ui.page, LV_HOR_RES, LV_VER_RES);
    ScreenApplyDotBackdrop(s_ui.page);
    lv_obj_add_flag(s_ui.page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_ui.page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_ui.page, PageDeleted, LV_EVENT_DELETE, nullptr);
    lv_obj_t* card = lv_obj_create(s_ui.page);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(card, LV_HOR_RES - UiSx(48), LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, UiColorForeground(), 0);
    lv_obj_set_style_border_width(card, UiSx(2), 0);
    lv_obj_set_style_pad_all(card, UiSx(18), 0);
    lv_obj_set_style_pad_row(card, UiSy(14), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_ui.title = lv_label_create(card);
    lv_obj_set_width(s_ui.title, lv_pct(100));
    lv_obj_set_style_text_font(s_ui.title, UiFont(), 0);
    lv_obj_set_style_text_align(s_ui.title, LV_TEXT_ALIGN_CENTER, 0);
    s_ui.filename = lv_label_create(card);
    lv_obj_set_width(s_ui.filename, lv_pct(100));
    lv_label_set_long_mode(s_ui.filename, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_ui.filename, UiFont(), 0);
    lv_obj_set_style_text_align(s_ui.filename, LV_TEXT_ALIGN_CENTER, 0);
    s_ui.count = lv_label_create(card);
    lv_obj_set_width(s_ui.count, lv_pct(100));
    lv_obj_set_style_text_font(s_ui.count, UiFont(), 0);
    lv_obj_set_style_text_align(s_ui.count, LV_TEXT_ALIGN_CENTER, 0);
    s_ui.bar = lv_bar_create(card);
    lv_obj_set_size(s_ui.bar, UiSx(300), UiSy(28));
    lv_bar_set_range(s_ui.bar, 0, 100);
    lv_obj_set_style_bg_color(s_ui.bar, UiColorBackground(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.bar, UiColorForeground(), LV_PART_INDICATOR);
    lv_obj_set_style_border_color(s_ui.bar, UiColorForeground(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.bar, UiSx(2), LV_PART_MAIN);
    s_ui.percent = lv_label_create(card);
    lv_obj_set_style_text_font(s_ui.percent, UiFont(), 0);
    lv_obj_t* cancel = lv_obj_create(card);
    lv_obj_clear_flag(cancel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(cancel, UiSx(160), UiSy(44));
    lv_obj_add_flag(cancel, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(cancel);
    lv_obj_add_event_cb(cancel, CancelClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* text = lv_label_create(cancel);
    lv_label_set_text(text, LangString_COMMON_CANCEL);
    lv_obj_set_style_text_font(text, UiFont(), 0);
    lv_obj_center(text);
    lv_obj_clear_flag(text, LV_OBJ_FLAG_CLICKABLE);
    s_ui.timer = lv_timer_create(Poll, 250, nullptr);
    if (!s_ui.timer) {
        ClosePage();
        return false;
    }
    s_active.store(true);
    HapticSetZoneEarlyPulseEnabled(false);
    Poll(nullptr);
    return true;
}

bool SettingsLogUpload_IsActive() {
    return s_active.load();
}

void SettingsLogUpload_Start() {
    if (s_ui.page) {
        return;
    }
    auto session = LogUploadStart();
    if (!session) {
        return;
    }
    if (!ShowPage(session)) {
        LogUploadCancel();
    }
}

void SettingsLogUpload_Teardown() {
    LogUploadCancel();
    ClosePage();
}
