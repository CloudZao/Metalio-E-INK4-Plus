#include "settings_pager_tab.h"
#include "settings_common.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "ble_page_turner.h"
#include "pager_screen/pager_screen.h"
#include "pager_key_handler.h"
#include "screen_common.h"
#include "screen_text.h"

#include <algorithm>
#include <cstdio>

namespace {
lv_obj_t* s_root = nullptr;
lv_obj_t* s_switch = nullptr;
lv_obj_t* s_state = nullptr;
lv_obj_t* s_current = nullptr;
lv_obj_t* s_connection_state = nullptr;
lv_obj_t* s_current_box = nullptr;
lv_timer_t* s_timer = nullptr;
uint32_t s_revision = UINT32_MAX;

lv_obj_t* Text(lv_obj_t* parent, const char* value, const lv_font_t* font = nullptr) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, value);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, font ? font : fontpack_lv_font_ui(), 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

void Rule(lv_obj_t* parent) {
    auto* rule = lv_obj_create(parent);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, LV_PCT(100), kSettingsBorderW);
    lv_obj_set_style_bg_color(rule, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_CLICKABLE);
}

void Refresh(lv_timer_t*) {
    if (!s_root) return;
    const auto state = BlePageTurner::GetSnapshot();
    if (state.revision == s_revision) return;
    s_revision = state.revision;
    auto* label = lv_obj_get_child(s_switch, 0);
    lv_label_set_text(label, state.enabled ? Lang::Strings::PAGER_ENABLED : Lang::Strings::QUICK_DISABLED);
    SettingsStyleSelectable(s_switch, label, state.enabled);
    lv_label_set_text(s_state, state.timed_out ? Lang::Strings::PAGER_AUTO_TIMEOUT : Lang::Strings::PAGER_AUTO_HINT);
    const bool connected = state.state == BlePageTurner::State::kConnected;
    const bool connecting = state.state == BlePageTurner::State::kConnecting;
    if (connected || connecting)
        lv_label_set_text_fmt(s_current, "%s\n%s", BlePageTurner::ModelName(state.target_model), state.target_address);
    else lv_label_set_text(s_current, Lang::Strings::PAGER_NO_CURRENT);
    lv_label_set_text(s_connection_state, connected ? Lang::Strings::PAGER_CONNECTED_TAG :
        connecting ? Lang::Strings::PAGER_CONNECTING_TAG : "");
    lv_obj_set_flag(s_connection_state, LV_OBJ_FLAG_HIDDEN, !connected && !connecting);
    lv_obj_set_width(s_connection_state, LV_SIZE_CONTENT);
    const auto* font = fontpack_lv_font_get(25, 2);
    if (!font) font = LV_FONT_DEFAULT;
    lv_obj_set_style_text_font(s_connection_state, font, 0);
    lv_obj_update_layout(s_root);
    const int width = lv_obj_get_content_width(s_current_box);
    const int tag_width = connected || connecting ? lv_obj_get_width(s_connection_state) + UiSx(8) : 0;
    ScreenFitLabel(s_current, width - tag_width, 2 * font->line_height, font);
    lv_obj_set_style_text_align(s_current, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_bg_color(s_current_box, connected ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_current_box, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_current, connected ? lv_color_white() : lv_color_black(), 0);
    lv_obj_set_style_text_color(s_connection_state, connected ? lv_color_white() : lv_color_black(), 0);
    lv_obj_update_layout(s_current_box);
    ScreenFitLabel(label, lv_obj_get_content_width(s_switch), lv_obj_get_content_height(s_switch));
}

void Deleted(lv_event_t* event) {
    if (lv_event_get_target(event) == s_root) SettingsPagerTab_Reset();
}
}

void SettingsPagerTab_Reset() {
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = nullptr;
    s_root = s_switch = s_state = s_current = nullptr;
    s_connection_state = s_current_box = nullptr;
    s_revision = UINT32_MAX;
}

void SettingsPagerTab_Build(lv_obj_t* page) {
    SettingsPagerTab_Reset();
    BlePageTurner::Init(PagerKey_Receive);
    lv_obj_update_layout(page);
    s_root = lv_obj_create(page);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, lv_obj_get_content_width(page), lv_obj_get_content_height(page));
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_root, UiSy(5), 0);
    lv_obj_add_event_cb(s_root, Deleted, LV_EVENT_DELETE, nullptr);
    Text(s_root, Lang::Strings::PAGER_AUTO);
    s_switch = SettingsCreateSelectableOption(s_root, "", [](lv_event_t*) {
        if (!BlePageTurner::SetEnabled(!BlePageTurner::GetSnapshot().enabled))
            ScreenShowToast(Lang::Strings::SETTINGS_PAGER_SAVE_ERROR);
    }, 0);
    auto* ui = fontpack_lv_font_ui();
    if (!ui) ui = LV_FONT_DEFAULT;
    auto* small = fontpack_lv_font_get(25, 2);
    if (!small) small = ui;
    s_state = Text(s_root, "", small);
    Rule(s_root);
    Text(s_root, Lang::Strings::PAGER_CURRENT);
    auto* current = lv_obj_create(s_root);
    s_current_box = current;
    lv_obj_remove_style_all(current);
    const int height = std::max<int>(UiSy(68), 2 * small->line_height + 2 * (kSettingsOptionPad + kSettingsBorderW));
    lv_obj_set_size(current, LV_PCT(100), height);
    lv_obj_set_style_border_width(current, kSettingsBorderW, 0);
    lv_obj_set_style_border_color(current, lv_color_black(), 0);
    lv_obj_set_style_radius(current, kSettingsOptionRadius, 0);
    lv_obj_set_style_pad_all(current, kSettingsOptionPad, 0);
    lv_obj_set_style_pad_column(current, UiSx(8), 0);
    lv_obj_set_flex_flow(current, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(current, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(current, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(current, LV_OBJ_FLAG_CLICKABLE);
    s_current = Text(current, "", small);
    lv_obj_set_width(s_current, 1);
    lv_obj_set_flex_grow(s_current, 1);
    s_connection_state = Text(current, "", small);
    lv_obj_set_width(s_connection_state, LV_SIZE_CONTENT);
    SettingsCreateSelectableOption(s_root, Lang::Strings::SETTINGS_PAGER_MANAGE,
        [](lv_event_t*) { ScreenNavigateTo(PagerScreen::Create); }, 0);
    s_timer = lv_timer_create(Refresh, 250, nullptr);
    Refresh(nullptr);
}
