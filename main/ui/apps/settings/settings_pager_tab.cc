#include <cstdint>
#include <cstring>
#include "ui/apps/settings/settings_tabs.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "boards/common/bluetooth/ble_page_turner.h"
#include "ui/apps/settings/settings_common.h"
#include "ui/common/runtime/input/pager_key_handler.h"
#include "ui/apps/settings/pager/pager_screen.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/runtime/screen_common_nav.h"

static lv_obj_t* s_root = nullptr;
static lv_obj_t* s_switch = nullptr;
static lv_obj_t* s_state = nullptr;
static lv_obj_t* s_current = nullptr;
static lv_timer_t* s_timer = nullptr;
static uint32_t s_revision = UINT32_MAX;

static void Refresh(lv_timer_t*) {
    if (!s_root) {
        return;
    }
    const auto state = BlePageTurnerGetSnapshot();
    if (state.revision == s_revision) {
        return;
    }
    s_revision = state.revision;
    lv_obj_set_state(s_switch, LV_STATE_CHECKED, state.enabled);
    lv_label_set_text(s_state, state.timed_out ? LangString_PAGER_AUTO_TIMEOUT : LangString_PAGER_AUTO_HINT);
    const bool connected = state.state == BlePageTurnerState::kConnected;
    lv_label_set_text(s_current, connected ? BlePageTurnerModelName(state.target_model) :
        std::strcmp(LangCode, "zh-CN") == 0 ? "未连接" : "Not connected");
}

static void Deleted(lv_event_t* event) {
    if (lv_event_get_target(event) == s_root) {
        SettingsPagerTab_Reset();
    }
}

void SettingsPagerTab_Build(lv_obj_t* page) {
    SettingsPagerTab_Reset();
    BlePageTurnerInit(PagerKey_Receive);
    s_root = lv_obj_create(page);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_clear_flag(s_root, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_root, kSettingsSectionGap, 0);
    lv_obj_add_event_cb(s_root, Deleted, LV_EVENT_DELETE, nullptr);
    auto* auto_section = SettingsCreateSection(s_root,
        std::strcmp(LangCode, "zh-CN") == 0 ? "翻页器设置" : "Page turner settings");
    auto* automatic = SettingsCreateListGroup(auto_section);
    s_switch = SettingsCreateToggle(automatic, LangString_PAGER_AUTO, [](lv_event_t* event) {
        const bool enabled = lv_obj_has_state(lv_event_get_current_target_obj(event), LV_STATE_CHECKED);
        if (!BlePageTurnerSetEnabled(enabled)) {
            lv_obj_set_state(lv_event_get_current_target_obj(event), LV_STATE_CHECKED, !enabled);
            ScreenShowToast(LangString_SETTINGS_PAGER_SAVE_ERROR);
        }
    });
    SettingsCreateSectionHint(auto_section, "");
    s_state = lv_obj_get_child(auto_section, -1);
    auto* devices = SettingsCreateListGroup(s_root);
    auto* current = SettingsCreateListRow(devices, LangString_PAGER_CURRENT, "—");
    s_current = lv_obj_get_child(current, 1);
    SettingsCreateListRow(devices, LangString_SETTINGS_PAGER_MANAGE, "", [](lv_event_t*) {
        ScreenNavigateTo(PagerScreen::Create);
    });
    s_timer = lv_timer_create(Refresh, 250, nullptr);
    Refresh(nullptr);
}

void SettingsPagerTab_Reset() {
    if (s_timer) {
        lv_timer_delete(s_timer);
    }
    s_timer = nullptr;
    s_root = s_switch = s_state = s_current = nullptr;
    s_revision = UINT32_MAX;
}
