#include "settings_test_battery_screen.h"

#include "assets/lang_config.h"
#include "bq27220_gauge.h"
#include "cx25601n.h"
#include "fontpack_lvgl.h"
#include "screen_common.h"
#include "settings_common.h"
#include "ui_scale.h"
#include "vk_key_handler.h"

#include <cstdio>

#include <esp_log.h>

static constexpr const char* TAG = "SettingsTestBat";
static constexpr const char* kScreenId = "settings_test_battery";
static constexpr uint32_t kPollPeriodMs = 1000;
static constexpr lv_coord_t kRowH = UiSy(48);
static constexpr lv_coord_t kTitleW = UiSx(168);

// 与 bq27220_gauge.cc 电压→SOC 曲线一致，用于显示瞬时电量（未平滑）。
static constexpr float kBatteryEmptyV = 3.3f;
static constexpr float kBatteryFullV = 4.33f; // 显示 100%；充电恒压目标见 CX25601N VREG（默认 4400mV）

struct SettingsTestBatRow {
    lv_obj_t* value = nullptr;
};

static lv_obj_t* s_scr = nullptr;
static lv_timer_t* s_timer = nullptr;
static bool s_alive = false;

static SettingsTestBatRow s_gauge;
static SettingsTestBatRow s_soc;
static SettingsTestBatRow s_soc_raw;
static SettingsTestBatRow s_volt;
static SettingsTestBatRow s_volt_range;
static SettingsTestBatRow s_curr;
static SettingsTestBatRow s_gauge_chg;
static SettingsTestBatRow s_vbus;
static SettingsTestBatRow s_chrg;
static SettingsTestBatRow s_chg_en;
static SettingsTestBatRow s_ichg;
static SettingsTestBatRow s_vreg;
static SettingsTestBatRow s_mismatch;

static bool s_have_volt = false;
static uint16_t s_volt_min = 0;
static uint16_t s_volt_max = 0;

static const char* ChrgStatUi(uint8_t stat) {
    switch (stat) {
        case CX25601N_CHG_STAT_NOT:
            return Lang::Strings::SETTINGS_TEST_BATTERY_CHG_NOT;
        case CX25601N_CHG_STAT_CC:
            return Lang::Strings::SETTINGS_TEST_BATTERY_CHG_CC;
        case CX25601N_CHG_STAT_CV:
            return Lang::Strings::SETTINGS_TEST_BATTERY_CHG_CV;
        case CX25601N_CHG_STAT_TOPOFF:
            return Lang::Strings::SETTINGS_TEST_BATTERY_CHG_TOPOFF;
        default:
            return Lang::Strings::COMMON_UNKNOWN;
    }
}

static const char* VbusStatUi(uint8_t stat) {
    switch (stat) {
        case 0:
            return Lang::Strings::SETTINGS_TEST_BATTERY_VBUS_NONE;
        case 1:
            return Lang::Strings::SETTINGS_TEST_BATTERY_VBUS_SDP;
        case 2:
            return Lang::Strings::SETTINGS_TEST_BATTERY_VBUS_CDP;
        case 3:
            return Lang::Strings::SETTINGS_TEST_BATTERY_VBUS_DCP;
        case 4:
            return Lang::Strings::SETTINGS_TEST_BATTERY_VBUS_UNK_ADP;
        case 5:
            return Lang::Strings::SETTINGS_TEST_BATTERY_VBUS_NONSTD;
        case 7:
            return Lang::Strings::SETTINGS_TEST_BATTERY_VBUS_OTG;
        default:
            return Lang::Strings::SETTINGS_TEST_BATTERY_VBUS_ADP;
    }
}

static void SetValue(SettingsTestBatRow& row, const char* text) {
    if (!s_alive || row.value == nullptr || !lv_obj_is_valid(row.value)) {
        return;
    }
    lv_label_set_text(row.value, text != nullptr ? text : "--");
}

static SettingsTestBatRow CreateRow(lv_obj_t* parent, const char* title) {
    SettingsTestBatRow out;

    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, kRowH);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_border_color(row, lv_color_black(), 0);
    lv_obj_set_style_pad_hor(row, 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title_lbl = lv_label_create(row);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_width(title_lbl, kTitleW);
    lv_obj_set_height(title_lbl, lv_font_get_line_height(fontpack_lv_font_ui()));
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(title_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(title_lbl, LV_OBJ_FLAG_CLICKABLE);

    out.value = lv_label_create(row);
    lv_label_set_text(out.value, "…");
    lv_obj_set_flex_grow(out.value, 1);
    lv_obj_set_height(out.value, lv_font_get_line_height(fontpack_lv_font_ui()));
    lv_label_set_long_mode(out.value, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(out.value, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(out.value, lv_color_black(), 0);
    lv_obj_set_style_text_align(out.value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_clear_flag(out.value, LV_OBJ_FLAG_CLICKABLE);

    return out;
}

static int VoltageToRawPct(uint16_t mv) {
    const float bat_v = static_cast<float>(mv) / 1000.0f;
    float raw_pct;
    if (bat_v >= kBatteryFullV) {
        raw_pct = 100.0f;
    } else if (bat_v <= kBatteryEmptyV) {
        raw_pct = 0.0f;
    } else {
        raw_pct = (bat_v - kBatteryEmptyV) / (kBatteryFullV - kBatteryEmptyV) * 100.0f;
    }
    int pct = static_cast<int>(raw_pct + 0.5f);
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    return pct;
}

static void TrackVoltage(uint16_t mv) {
    if (!s_have_volt) {
        s_volt_min = mv;
        s_volt_max = mv;
        s_have_volt = true;
    } else {
        if (mv < s_volt_min) {
            s_volt_min = mv;
        }
        if (mv > s_volt_max) {
            s_volt_max = mv;
        }
    }
}

static void Poll() {
    if (!s_alive) {
        return;
    }

    char buf[48];
    auto& gauge = Bq27220Gauge::GetInstance();

    bool gauge_ok = false;
    int level = 0;
    bool charging = false;
    bool discharging = false;
    uint16_t mv = 0;
    int16_t ma = 0;
    bool have_mv = false;
    bool have_ma = false;

    if (!gauge.IsReady()) {
        SetValue(s_gauge, Lang::Strings::SETTINGS_TEST_NOT_DETECTED);
        SetValue(s_soc, "--");
        SetValue(s_soc_raw, "--");
        SetValue(s_volt, "--");
        SetValue(s_volt_range, "--");
        SetValue(s_curr, "--");
        SetValue(s_gauge_chg, "--");
    } else {
        SetValue(s_gauge, Lang::Strings::SETTINGS_TEST_OK);
        gauge_ok = gauge.GetBatteryLevel(level, charging, discharging);
        have_mv = gauge.ReadVoltageMv(mv);
        have_ma = gauge.ReadCurrentMa(ma);

        if (gauge_ok) {
            std::snprintf(buf, sizeof(buf), "%d%%", level);
            SetValue(s_soc, buf);
            SetValue(s_gauge_chg,
                     charging ? Lang::Strings::SETTINGS_TEST_BATTERY_CHARGING
                              : (discharging ? Lang::Strings::SETTINGS_TEST_BATTERY_DISCHARGING
                                             : Lang::Strings::SETTINGS_TEST_BATTERY_IDLE_LOAD));
        } else {
            SetValue(s_soc, Lang::Strings::SETTINGS_TEST_READ_FAIL);
            SetValue(s_gauge_chg, "--");
        }

        if (have_mv) {
            TrackVoltage(mv);
            std::snprintf(buf, sizeof(buf), "%u mV", static_cast<unsigned>(mv));
            SetValue(s_volt, buf);
            std::snprintf(buf, sizeof(buf), "%d%%", VoltageToRawPct(mv));
            SetValue(s_soc_raw, buf);
            std::snprintf(buf, sizeof(buf), "%u~%u Δ%u", static_cast<unsigned>(s_volt_min),
                          static_cast<unsigned>(s_volt_max),
                          static_cast<unsigned>(s_volt_max - s_volt_min));
            SetValue(s_volt_range, buf);
        } else {
            SetValue(s_volt, Lang::Strings::SETTINGS_TEST_READ_FAIL);
            SetValue(s_soc_raw, "--");
            SetValue(s_volt_range, "--");
        }

        if (have_ma) {
            std::snprintf(buf, sizeof(buf), "%+d mA", static_cast<int>(ma));
            SetValue(s_curr, buf);
        } else {
            SetValue(s_curr, Lang::Strings::SETTINGS_TEST_READ_FAIL);
        }
    }

    bool cx_ok = cx25601n_is_ready();
    uint8_t chrg_stat = 0;
    uint8_t vbus_stat = 0;
    bool chg_en = false;
    uint32_t ichg_ma = 0;
    uint32_t vreg_mv = 0;
    bool have_chrg = false;
    bool have_vbus = false;

    if (!cx_ok) {
        SetValue(s_vbus, Lang::Strings::SETTINGS_TEST_NOT_DETECTED);
        SetValue(s_chrg, "--");
        SetValue(s_chg_en, "--");
        SetValue(s_ichg, "--");
        SetValue(s_vreg, "--");
    } else {
        have_vbus = (cx25601n_get_vbus_stat(&vbus_stat) == ESP_OK);
        have_chrg = (cx25601n_get_chrg_stat(&chrg_stat) == ESP_OK);
        const bool chip_active =
            have_chrg && (chrg_stat == CX25601N_CHG_STAT_CC || chrg_stat == CX25601N_CHG_STAT_CV ||
                          chrg_stat == CX25601N_CHG_STAT_TOPOFF);
        // 初始化关了 DPDM：VBUS_STAT 常为 0，但芯片仍可充电；有充电流/STAT 时按有输入显示
        if (have_vbus) {
            if (vbus_stat == 0 && (chip_active || charging)) {
                SetValue(s_vbus, Lang::Strings::SETTINGS_TEST_BATTERY_VBUS_UNKNOWN_IN);
            } else {
                SetValue(s_vbus, VbusStatUi(vbus_stat));
            }
        } else {
            SetValue(s_vbus, Lang::Strings::SETTINGS_TEST_READ_FAIL);
        }
        SetValue(s_chrg, have_chrg ? ChrgStatUi(chrg_stat) : Lang::Strings::SETTINGS_TEST_READ_FAIL);

        if (cx25601n_is_charge_enabled(&chg_en) == ESP_OK) {
            SetValue(s_chg_en, chg_en ? Lang::Strings::SETTINGS_TEST_BATTERY_EN_ON
                                      : Lang::Strings::SETTINGS_TEST_BATTERY_EN_OFF);
        } else {
            SetValue(s_chg_en, Lang::Strings::SETTINGS_TEST_READ_FAIL);
        }
        if (cx25601n_get_ichg_ma(&ichg_ma) == ESP_OK) {
            std::snprintf(buf, sizeof(buf), "%lu mA", static_cast<unsigned long>(ichg_ma));
            SetValue(s_ichg, buf);
        } else {
            SetValue(s_ichg, Lang::Strings::SETTINGS_TEST_READ_FAIL);
        }
        if (cx25601n_get_vreg_mv(&vreg_mv) == ESP_OK) {
            std::snprintf(buf, sizeof(buf), "%lu mV", static_cast<unsigned long>(vreg_mv));
            SetValue(s_vreg, buf);
        } else {
            SetValue(s_vreg, Lang::Strings::SETTINGS_TEST_READ_FAIL);
        }
    }

    // 表计电流判定 vs 充电芯片状态
    if (gauge_ok && cx_ok && have_chrg) {
        const bool chip_active =
            (chrg_stat == CX25601N_CHG_STAT_CC || chrg_stat == CX25601N_CHG_STAT_CV ||
             chrg_stat == CX25601N_CHG_STAT_TOPOFF);
        const bool chip_idle = (chrg_stat == CX25601N_CHG_STAT_NOT);
        const bool vbus_typed = have_vbus && (vbus_stat != 0);
        const bool power_present = vbus_typed || chip_active || charging;

        if (chip_active && !charging) {
            SetValue(s_mismatch, Lang::Strings::SETTINGS_TEST_BATTERY_MISMATCH_CHIP);
        } else if (charging && chip_idle) {
            SetValue(s_mismatch, Lang::Strings::SETTINGS_TEST_BATTERY_MISMATCH_GAUGE);
        } else if (vbus_typed && chip_idle && !charging) {
            SetValue(s_mismatch, Lang::Strings::SETTINGS_TEST_BATTERY_MISMATCH_NO_CHG);
        } else if (!power_present && (charging || chip_active)) {
            SetValue(s_mismatch, Lang::Strings::SETTINGS_TEST_BATTERY_MISMATCH_NO_IN);
        } else if (!vbus_typed && (charging || chip_active)) {
            SetValue(s_mismatch, Lang::Strings::SETTINGS_TEST_BATTERY_MATCH_DPDM);
        } else {
            SetValue(s_mismatch, Lang::Strings::SETTINGS_TEST_BATTERY_MATCH_OK);
        }
    } else {
        SetValue(s_mismatch, "--");
    }

    if (have_mv || have_ma) {
        ESP_LOGI(TAG, "soc=%d%% raw=%d%% mv=%u(%u~%u) ma=%+d chg=%d cx=%s/%s en=%d",
                 gauge_ok ? level : -1, have_mv ? VoltageToRawPct(mv) : -1, static_cast<unsigned>(mv),
                 static_cast<unsigned>(s_volt_min), static_cast<unsigned>(s_volt_max),
                 have_ma ? static_cast<int>(ma) : 0, charging ? 1 : 0,
                 have_vbus ? cx25601n_vbus_stat_str(vbus_stat) : "-",
                 have_chrg ? cx25601n_chrg_stat_str(chrg_stat) : "-", chg_en ? 1 : 0);
    }
}

static void OnPollTimer(lv_timer_t* /*t*/) {
    Poll();
}

static void StopTimer() {
    if (s_timer != nullptr) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
}

static void OnDeleted(lv_event_t* e) {
    if (lv_event_get_target(e) != s_scr) {
        return;
    }
    s_alive = false;
    StopTimer();
    s_scr = nullptr;
    s_gauge = {};
    s_soc = {};
    s_soc_raw = {};
    s_volt = {};
    s_volt_range = {};
    s_curr = {};
    s_gauge_chg = {};
    s_vbus = {};
    s_chrg = {};
    s_chg_en = {};
    s_ichg = {};
    s_vreg = {};
    s_mismatch = {};
    s_have_volt = false;
    s_volt_min = 0;
    s_volt_max = 0;
    ESP_LOGI(TAG, "battery test screen deleted");
}

lv_obj_t* SettingsTestBatteryScreen::Create() {
    ESP_LOGI(TAG, "create battery test screen");

    s_alive = false;
    StopTimer();
    s_have_volt = false;
    s_volt_min = 0;
    s_volt_max = 0;

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_scr = scr;
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, OnDeleted, LV_EVENT_DELETE, nullptr);

    EpdStatusBar status = ScreenCreateStatusBar(scr);

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, LV_VER_RES - status.height);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(body, kSettingsContentPad, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(body, 0, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);

    lv_obj_t* title = lv_label_create(body);
    lv_label_set_text(title, Lang::Strings::SETTINGS_TEST_BATTERY);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* hint = lv_label_create(body);
    lv_label_set_text(hint, Lang::Strings::SETTINGS_TEST_BATTERY_HINT);
    lv_obj_set_style_text_font(hint, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_set_style_pad_bottom(hint, 4, 0);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);

    s_gauge = CreateRow(body, Lang::Strings::SETTINGS_TEST_BATTERY_GAUGE);
    s_soc = CreateRow(body, Lang::Strings::SETTINGS_TEST_BATTERY_SOC_SMOOTH);
    s_soc_raw = CreateRow(body, Lang::Strings::SETTINGS_TEST_BATTERY_SOC_RAW);
    s_volt = CreateRow(body, Lang::Strings::SETTINGS_TEST_BATTERY_VOLT);
    s_volt_range = CreateRow(body, Lang::Strings::SETTINGS_TEST_BATTERY_VOLT_RANGE);
    s_curr = CreateRow(body, Lang::Strings::SETTINGS_TEST_BATTERY_CURR);
    s_gauge_chg = CreateRow(body, Lang::Strings::SETTINGS_TEST_BATTERY_GAUGE_STAT);
    s_vbus = CreateRow(body, Lang::Strings::SETTINGS_TEST_BATTERY_VBUS);
    s_chrg = CreateRow(body, Lang::Strings::SETTINGS_TEST_BATTERY_CHIP_CHG);
    s_chg_en = CreateRow(body, Lang::Strings::SETTINGS_TEST_BATTERY_CHG_EN);
    s_ichg = CreateRow(body, Lang::Strings::SETTINGS_TEST_BATTERY_ICHG);
    s_vreg = CreateRow(body, Lang::Strings::SETTINGS_TEST_BATTERY_VREG);
    s_mismatch = CreateRow(body, Lang::Strings::SETTINGS_TEST_BATTERY_MATCH);

    ScreenSetIsHome(false);
    VkKey_AttachScreen(scr, kScreenId, VkKeyScreenDesc{SettingsTestBatteryScreen::Create});

    s_alive = true;
    Poll();
    s_timer = lv_timer_create(OnPollTimer, kPollPeriodMs, nullptr);
    return scr;
}

static bool s_paused_for_standby = false;

void SettingsTestBatteryScreen_PauseForStandby() {
    if (s_paused_for_standby) {
        return;
    }
    s_paused_for_standby = true;
    StopTimer();
}

void SettingsTestBatteryScreen_ResumeFromStandby() {
    if (!s_paused_for_standby) {
        return;
    }
    s_paused_for_standby = false;
    if (s_alive && s_scr != nullptr && s_timer == nullptr) {
        s_timer = lv_timer_create(OnPollTimer, kPollPeriodMs, nullptr);
    }
}
