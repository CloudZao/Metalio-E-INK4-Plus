#include "settings_power_tab.h"

#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "power_policy.h"
#include "settings_common.h"
#include "ui_scale.h"
#include "screen_text.h"

namespace {

constexpr lv_coord_t kCompactOptionH = UiSy(44);
constexpr lv_coord_t kRowGap = UiSx(8);
constexpr lv_coord_t kPagePadRow = UiSy(8); // 本页行距略紧于全局 kSettingsOptionGap

struct MhzOption {
    int mhz; // CPU 频率，单位 MHz
    const char* title; // 选项显示文本
};

struct SecOption {
    int sec; // 时长，单位秒；0 表示永不
};

constexpr MhzOption kMhzOptions[] = {
    {160, "160"},
    {240, "240"},
    {320, "320"},
};
constexpr int kMhzCount = sizeof(kMhzOptions) / sizeof(kMhzOptions[0]);

constexpr SecOption kNetGraceOptions[] = {
    {30},
    {60},
    {120},
    {0}, // 永不因空闲断网
};
constexpr int kNetGraceCount = sizeof(kNetGraceOptions) / sizeof(kNetGraceOptions[0]);

const char* SecOptionTitle(int sec) {
    switch (sec) {
        case 0:
            return Lang::Strings::SETTINGS_POWER_OFF_NEVER;
        case 30:
            return Lang::Strings::SETTINGS_POWER_30_SEC;
        case 60:
            return Lang::Strings::SETTINGS_POWER_60_SEC;
        case 120:
            return Lang::Strings::SETTINGS_POWER_120_SEC;
        default:
            return Lang::Strings::SETTINGS_POWER_30_SEC;
    }
}

struct PowerUi {
    lv_obj_t* mhz_btns[kMhzCount] = {};
    lv_obj_t* grace_btns[kNetGraceCount] = {};
};

PowerUi s_ui;

void RefreshOptions() {
    auto& pp = PowerPolicy::GetInstance();
    const int mhz = pp.GetIdleCpuMhz();
    const int grace_s = pp.GetNetGraceSec();

    for (int i = 0; i < kMhzCount; ++i) {
        if (s_ui.mhz_btns[i] != nullptr) {
            SettingsStyleSelectable(s_ui.mhz_btns[i], lv_obj_get_child(s_ui.mhz_btns[i], 0),
                                    kMhzOptions[i].mhz == mhz);
        }
    }
    for (int i = 0; i < kNetGraceCount; ++i) {
        if (s_ui.grace_btns[i] != nullptr) {
            SettingsStyleSelectable(s_ui.grace_btns[i], lv_obj_get_child(s_ui.grace_btns[i], 0),
                                    kNetGraceOptions[i].sec == grace_s);
        }
    }
}

void OnMhzClicked(lv_event_t* e) {
    const int mhz = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (PowerPolicy::GetInstance().GetIdleCpuMhz() == mhz) {
        return;
    }
    PowerPolicy::GetInstance().SetIdleCpuMhz(mhz);
    RefreshOptions();
}

void OnNetGraceClicked(lv_event_t* e) {
    const int sec = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (PowerPolicy::GetInstance().GetNetGraceSec() == sec) {
        return;
    }
    PowerPolicy::GetInstance().SetNetGraceSec(sec);
    RefreshOptions();
}

void AddSectionTitle(lv_obj_t* page, const char* text) {
    lv_obj_t* title = lv_label_create(page);
    lv_label_set_text(title, text);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);
}

void AddSectionHint(lv_obj_t* page, const char* text) {
    lv_obj_t* hint = lv_label_create(page);
    lv_label_set_text(hint, text);
    lv_obj_set_style_text_font(hint, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
}

void AddSectionDivider(lv_obj_t* page) {
    lv_obj_t* split = lv_obj_create(page);
    lv_obj_remove_style_all(split);
    lv_obj_set_size(split, lv_pct(100), kSettingsSplitLineW);
    lv_obj_set_style_bg_color(split, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(split, LV_OPA_COVER, 0);
    lv_obj_clear_flag(split, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t* CreateCompactOption(lv_obj_t* row, const char* title, lv_event_cb_t cb, intptr_t user_data) {
    lv_obj_t* btn = lv_obj_create(row);
    lv_obj_remove_style_all(btn);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, kCompactOptionH);
    lv_obj_set_style_pad_all(btn, UiSx(4), 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, reinterpret_cast<void*>(user_data));

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, ScreenFitButtonLabelOnSize, LV_EVENT_SIZE_CHANGED, lbl);

    SettingsStyleSelectable(btn, lbl, false);
    return btn;
}

lv_obj_t* CreateOptionRow(lv_obj_t* page) {
    lv_obj_t* row = lv_obj_create(page);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, kCompactOptionH);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, kRowGap, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    return row;
}

}  // namespace

void SettingsPowerTab_Reset() {
    s_ui = {};
}

void SettingsPowerTab_Build(lv_obj_t* page) {
    s_ui = {};
    lv_obj_set_style_pad_row(page, kPagePadRow, 0);

    AddSectionTitle(page, Lang::Strings::SETTINGS_POWER_IDLE_MHZ_TITLE);
    AddSectionHint(page, Lang::Strings::SETTINGS_POWER_IDLE_MHZ_HINT);
    {
        lv_obj_t* row = CreateOptionRow(page);
        for (int i = 0; i < kMhzCount; ++i) {
            s_ui.mhz_btns[i] = CreateCompactOption(row, kMhzOptions[i].title, OnMhzClicked,
                                                   static_cast<intptr_t>(kMhzOptions[i].mhz));
        }
    }

    AddSectionDivider(page);

    AddSectionTitle(page, Lang::Strings::SETTINGS_POWER_NET_GRACE_TITLE);
    AddSectionHint(page, Lang::Strings::SETTINGS_POWER_NET_GRACE_HINT);
    {
        lv_obj_t* row = CreateOptionRow(page);
        for (int i = 0; i < kNetGraceCount; ++i) {
            s_ui.grace_btns[i] =
                CreateCompactOption(row, SecOptionTitle(kNetGraceOptions[i].sec), OnNetGraceClicked,
                                    static_cast<intptr_t>(kNetGraceOptions[i].sec));
        }
    }

    AddSectionDivider(page);

    AddSectionTitle(page, Lang::Strings::SETTINGS_POWER_STANDBY_TITLE);
    AddSectionHint(page, Lang::Strings::SETTINGS_POWER_STANDBY_HINT);

    AddSectionDivider(page);

    AddSectionTitle(page, Lang::Strings::SETTINGS_POWER_OFF_TITLE);
    AddSectionHint(page, Lang::Strings::SETTINGS_POWER_OFF_HINT);

    RefreshOptions();
}
