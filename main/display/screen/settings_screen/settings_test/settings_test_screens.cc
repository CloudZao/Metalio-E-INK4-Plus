/**
 * @file settings_test_screens.cc
 * @brief S31 测试子页：自动 / 老化（触摸、电池见独立文件；无 4G）
 */

#include "settings_test_screens.h"

#include "settings_test_audio.h"

#include "assets/lang_config.h"
#include "board.h"
#include "bq27220_gauge.h"
#include "cx25601n.h"
#include "fontpack_lvgl.h"
#include "metalio_sd.h"
#include "pcf8563.h"
#include "sc7a20h.h"
#include "screen_common.h"
#include "settings_common.h"
#include "ui_scale.h"
#include "vk_key_handler.h"

#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <time.h>

#define TAG "SettingsTest"

static constexpr lv_coord_t kRowH = UiSy(48);

static lv_obj_t* MakeScr(const char* title) {
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    const lv_coord_t body_h = LV_VER_RES - status.height;

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, body_h);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_pad_all(body, kSettingsContentPad, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(body, kSettingsTabGap, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);

    lv_obj_t* title_lbl = lv_label_create(body);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_clear_flag(title_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_user_data(scr, body);
    ScreenSetIsHome(false);
    return scr;
}

static lv_obj_t* BodyOf(lv_obj_t* scr) {
    return static_cast<lv_obj_t*>(lv_obj_get_user_data(scr));
}

static lv_obj_t* AddStatusRow(lv_obj_t* parent, const char* name, const char* value) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, kRowH);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* n = lv_label_create(row);
    lv_label_set_text(n, name);
    lv_obj_set_style_text_font(n, fontpack_lv_font_ui(), 0);
    lv_obj_clear_flag(n, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* v = lv_label_create(row);
    lv_label_set_text(v, value);
    lv_obj_set_style_text_font(v, fontpack_lv_font_ui(), 0);
    lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE);
    return v;
}

static constexpr const char* kAutoScreenId = "settings_test_auto";
static lv_obj_t* s_auto_scr = nullptr;

static void OnAutoDeleted(lv_event_t* e) {
    if (lv_event_get_target(e) != s_auto_scr) {
        return;
    }
    SettingsTestAudio_Teardown();
    s_auto_scr = nullptr;
}

lv_obj_t* SettingsTestAutoScreen::Create() {
    lv_obj_t* scr = MakeScr(Lang::Strings::SETTINGS_TEST_AUTO);
    s_auto_scr = scr;
    lv_obj_t* body = BodyOf(scr);
    lv_obj_add_event_cb(scr, OnAutoDeleted, LV_EVENT_DELETE, nullptr);

    auto set_row = [&](const char* name, bool ok, const char* detail) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s %s",
                      ok ? Lang::Strings::SETTINGS_TEST_OK : Lang::Strings::SETTINGS_TEST_FAIL,
                      detail != nullptr ? detail : "");
        AddStatusRow(body, name, buf);
    };

    {
        int level = 0;
        bool charging = false;
        bool discharging = false;
        const bool ok = Bq27220Gauge::GetInstance().GetBatteryLevel(level, charging, discharging);
        char d[32];
        std::snprintf(d, sizeof(d), "%d%%", level);
        set_row("BQ27220", ok, d);
    }

    {
        struct tm dt = {};
        const bool ok = Pcf8563::GetInstance().GetTime(dt);
        char d[40] = {};
        if (ok) {
            std::snprintf(d, sizeof(d), "%04d-%02d-%02d %02d:%02d", dt.tm_year + 1900, dt.tm_mon + 1,
                          dt.tm_mday, dt.tm_hour, dt.tm_min);
        }
        set_row("PCF8563", ok, d);
    }

    {
        const bool ok = cx25601n_is_ready();
        set_row("CX25601N", ok, ok ? "" : Lang::Strings::SETTINGS_TEST_NOT_DETECTED);
    }

    {
        const bool ok = MetalioSd_Ok();
        set_row(Lang::Strings::SETTINGS_TEST_SDCARD, ok,
                ok ? "" : Lang::Strings::SETTINGS_TEST_SD_MOUNT_FAIL);
    }

    {
        int ax = 0, ay = 0, az = 0;
        const bool ok = Sc7a20h::GetInstance().ReadAccelMg(ax, ay, az);
        char d[48] = {};
        if (ok) {
            std::snprintf(d, sizeof(d), "%d,%d,%d mg", ax, ay, az);
        }
        set_row("SC7A20H", ok, d);
    }

    SettingsTestAudio_BuildRow(body, scr);

    AddStatusRow(body, "WiFi", Lang::Strings::SETTINGS_TEST_SKIP);
    AddStatusRow(body, "4G", Lang::Strings::SETTINGS_TEST_SKIP);
    AddStatusRow(body, Lang::Strings::SETTINGS_TEST_CAMERA, Lang::Strings::SETTINGS_TEST_SKIP);

    VkKey_AttachScreen(scr, kAutoScreenId, VkKeyScreenDesc{SettingsTestAutoScreen::Create});
    SettingsTestAudio_OnLoad();
    return scr;
}

struct AgingUi {
    lv_obj_t* scr = nullptr;
    lv_obj_t* lbl = nullptr;
    lv_timer_t* timer = nullptr;
    bool vibe_on = false;
    uint32_t ticks = 0;
};

static AgingUi s_aging;

static void AgingTick(lv_timer_t* /*t*/) {
    ++s_aging.ticks;
    if ((s_aging.ticks % 11) == 1) {
        Board::GetInstance().PulseVibration();
        s_aging.vibe_on = true;
    } else {
        s_aging.vibe_on = false;
    }
    if (s_aging.lbl != nullptr) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), Lang::Strings::SETTINGS_TEST_AGING_TICK_FMT,
                      (unsigned)s_aging.ticks, s_aging.vibe_on ? 1 : 0);
        lv_label_set_text(s_aging.lbl, buf);
    }
}

static void OnAgingDeleted(lv_event_t* e) {
    if (lv_event_get_target(e) != s_aging.scr) {
        return;
    }
    Board::GetInstance().SetVibration(false);
    if (s_aging.timer != nullptr) {
        lv_timer_delete(s_aging.timer);
    }
    s_aging = {};
}

lv_obj_t* SettingsTestAgingScreen::Create() {
    s_aging = {};
    lv_obj_t* scr = MakeScr(Lang::Strings::SETTINGS_TEST_AGING);
    s_aging.scr = scr;
    lv_obj_t* body = BodyOf(scr);
    lv_obj_add_event_cb(scr, OnAgingDeleted, LV_EVENT_DELETE, nullptr);

    s_aging.lbl = lv_label_create(body);
    lv_label_set_text(s_aging.lbl, Lang::Strings::SETTINGS_TEST_AGING_RUNNING);
    lv_obj_set_style_text_font(s_aging.lbl, fontpack_lv_font_ui(), 0);
    lv_obj_clear_flag(s_aging.lbl, LV_OBJ_FLAG_CLICKABLE);

    s_aging.timer = lv_timer_create(AgingTick, 1000, nullptr);
    return scr;
}
