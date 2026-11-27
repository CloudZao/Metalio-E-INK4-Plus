// network_screen.cc — Create / VK / lifecycle / overlays
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "sysmgmt/system_control.h"
#include "ui/apps/settings/network/network_screen.h"
#include "assets/lang_config.h"
#include "ui/apps/settings/network/network_keyboard.h"
#include "ui/apps/settings/network/network_list.h"
#include "ui/apps/settings/network/network_screen_priv.h"
#include "ui/apps/settings/network/network_wifi.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/runtime/modal.h"
#include "ui/common/widgets/screen_text.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/runtime/screen_common_nav.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"

static UiModal s_window_modal;

static NetworkUiState s_ui_state;

NetworkUiState& Network_State() {
    return s_ui_state;
}

void Network_CloseStatusOverlay() {
    if (Network_State().fail_close_timer != nullptr) {
        lv_timer_delete(Network_State().fail_close_timer);
        Network_State().fail_close_timer = nullptr;
    }
    if (Network_State().ui.status_overlay != nullptr && lv_obj_is_valid(Network_State().ui.status_overlay)) {
        lv_obj_delete(Network_State().ui.status_overlay);
    }
    Network_State().ui.status_overlay = nullptr;
    Network_State().ui.status_msg = nullptr;
}

void Network_OpenStatusOverlay(const char* msg) {
    if (Network_State().ui.screen == nullptr) {
        return;
    }
    Network_CloseStatusOverlay();
    lv_obj_t* owner = lv_obj_get_screen(Network_State().ui.screen);
    lv_obj_update_layout(owner);
    lv_obj_t* mask = lv_obj_create(owner);
    Network_Strip(mask);
    lv_obj_set_size(mask, lv_obj_get_width(owner), lv_obj_get_height(owner));
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(mask, -lv_obj_get_style_space_left(owner, LV_PART_MAIN),
                    -lv_obj_get_style_space_top(owner, LV_PART_MAIN));
    lv_obj_set_style_bg_opa(mask, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    Network_State().ui.status_overlay = mask;
    lv_obj_add_event_cb(mask, [](lv_event_t* event) {
        if (Network_State().ui.status_overlay == lv_event_get_target(event)) {
            Network_State().ui.status_overlay = nullptr;
            Network_State().ui.status_msg = nullptr;
            if (Network_State().fail_close_timer) {
                lv_timer_delete(Network_State().fail_close_timer);
                Network_State().fail_close_timer = nullptr;
            }
        }
    }, LV_EVENT_DELETE, nullptr);
    s_window_modal.Bind(mask, [](void*) {
        if (!Network_State().restart_pending.load()) {
            Network_CloseStatusOverlay();
        }
    }, nullptr, UiModalPolicy::Dismissible, [](void*) { Network_CloseStatusOverlay(); });
    lv_obj_update_layout(mask);

    lv_obj_t* card = lv_obj_create(mask);
    Network_Strip(card);
    const lv_coord_t width = lv_obj_get_content_width(mask);
    lv_obj_set_size(card, width - kOverlayCardMargin, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(card, kOverlayCardMinH, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ScreenStyleDialogCard(card, kOverlayCardPad);
    lv_obj_set_style_max_height(card, lv_obj_get_content_height(mask) - UiSy(32), 0);
    lv_obj_center(card);

    lv_obj_t* lbl = lv_label_create(card);
    Network_State().ui.status_msg = lbl;
    lv_label_set_text(lbl, msg != nullptr ? msg : "");
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl, UiFont(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
}

void Network_SetStatusOverlayText(const char* msg) {
    if (Network_State().ui.status_msg != nullptr) {
        lv_label_set_text(Network_State().ui.status_msg, msg != nullptr ? msg : "");
    }
}

void Network_OnScreenUnload(lv_event_t* /*e*/) {
    ESP_LOGI(TAG, "unload");
    Network_State().screen_active = false;
    Network_State().generation.fetch_add(1);
    if (Network_State().transfer_scan_timer) {
        lv_timer_delete(Network_State().transfer_scan_timer);
        Network_State().transfer_scan_timer = nullptr;
    }
    Network_State().transfer_scan_pending = false;
    Network_State().transfer_scan_message.clear();
    Network_ClosePasswordPage();
    Network_CloseStatusOverlay();
    Network_StopForScreen();
    Network_State().ui = {};
    Network_State().scan_results.clear();
    Network_State().scan_in_progress = false;
    Network_State().connect_in_progress = false;
}

bool Network_ScreenAlive() {
    return Network_State().screen_active && Network_State().ui.screen != nullptr &&
           lv_obj_is_valid(Network_State().ui.screen);
}

const lv_font_t* Network_UiFont() {
    return UiFont();
}

bool Network_IsPasswordPageOpen() {
    return Network_State().ui.pwd_page != nullptr && !lv_obj_has_flag(Network_State().ui.pwd_page, LV_OBJ_FLAG_HIDDEN);
}

const char* Network_AuthLabel(wifi_auth_mode_t mode) {
    return (mode == WIFI_AUTH_OPEN) ? LangString_NETWORK_AUTH_OPEN : LangString_NETWORK_AUTH_SECURE;
}

const char* Network_DisconnectReasonText(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_AUTH_LEAVE:
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
        case WIFI_REASON_IE_IN_4WAY_DIFFERS:
        case WIFI_REASON_GROUP_CIPHER_INVALID:
        case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
        case WIFI_REASON_AKMP_INVALID:
        case WIFI_REASON_802_1X_AUTH_FAILED:
            return LangString_NETWORK_ERR_BAD_PASSWORD;
        case WIFI_REASON_NO_AP_FOUND:
            return LangString_NETWORK_ERR_AP_GONE;
        case WIFI_REASON_ASSOC_LEAVE:
        case WIFI_REASON_ASSOC_TOOMANY:
        case WIFI_REASON_ASSOC_FAIL:
        case WIFI_REASON_ASSOC_NOT_AUTHED:
            return LangString_NETWORK_ERR_ASSOC;
        case WIFI_REASON_BEACON_TIMEOUT:
            return LangString_NETWORK_ERR_WEAK;
        default:
            return nullptr;
    }
}

void Network_Strip(lv_obj_t* obj) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

lv_coord_t Network_UiLineH() {
    const lv_font_t* font = UiFont();
    return font != nullptr ? static_cast<lv_coord_t>(font->line_height) : 28;
}

// 附近列表按单行高度估条数，尽量填满刷新按钮上方后再换页。
lv_coord_t Network_NearbyRowHEstimate() {
    return Network_UiLineH() + kRowPadVer * 2 + 1;
}

// 已保存行含操作按钮，按按钮高度估，避免一页挤爆。
lv_coord_t Network_SavedRowHEstimate() {
    return std::max(Network_NearbyRowHEstimate(), kSavedActionBtnH + kRowPadVer * 2 + 1);
}

int Network_ComputeListPageSize(lv_coord_t row_h) {
    if (Network_State().ui.list == nullptr) {
        return kPageSizeFallback;
    }
    if (Network_State().ui.screen != nullptr) {
        lv_obj_update_layout(Network_State().ui.screen);
    } else {
        lv_obj_update_layout(Network_State().ui.list);
    }
    lv_coord_t h = lv_obj_get_content_height(Network_State().ui.list);
    if (h <= 0) {
        h = lv_obj_get_height(Network_State().ui.list);
    }
    // 行高略放大，吸收字高/底边框取整误差，避免末行压到刷新按钮
    const lv_coord_t fit_h = std::max<lv_coord_t>(1, row_h + 2);
    const int n = static_cast<int>(h / fit_h);
    return std::max(1, n);
}

void Network_RefreshPageSizes() {
    Network_State().nearby_page_size = Network_ComputeListPageSize(Network_NearbyRowHEstimate());
    Network_State().saved_page_size = Network_ComputeListPageSize(Network_SavedRowHEstimate());
}

// 名称可折两行：行高随内容，上下对称内边距避免贴分割线，同时保证单/两行垂直居中。
void Network_StyleWifiNameRow(lv_obj_t* row, lv_obj_t* name) {
    const lv_coord_t line_h = Network_UiLineH();
    lv_obj_set_style_pad_hor(row, kRowPadHor, 0);
    lv_obj_set_style_pad_ver(row, kRowPadVer, 0);
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_width(name, 0);
    lv_obj_set_flex_grow(name, 1);
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_height(name, line_h * 2, 0);
}

lv_obj_t* Network_MakeBtn(lv_obj_t* parent, const char* text, lv_coord_t w, lv_coord_t h, lv_event_cb_t cb,
                          void* user_data) {
    lv_obj_t* btn = lv_obj_create(parent);
    Network_Strip(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, UiColorForeground(), 0);
    lv_obj_set_style_border_width(btn, kBtnBorderW, 0);
    lv_obj_set_style_radius(btn, kBtnRadius, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, UiFont(), 0);
    lv_obj_set_style_text_color(lbl, UiColorForeground(), 0);
    ScreenFitLabel(lbl, w - 12, h - 8);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

void Network_StyleTabBtn(lv_obj_t* btn, bool selected) {
    if (btn == nullptr) {
        return;
    }
    lv_obj_t* lbl = lv_obj_get_child(btn, 0);
    if (selected) {
        lv_obj_set_style_bg_color(btn, UiColorForeground(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        if (lbl != nullptr) {
            lv_obj_set_style_text_color(lbl, UiColorBackground(), 0);
        }
    } else {
        lv_obj_set_style_bg_color(btn, UiColorBackground(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        if (lbl != nullptr) {
            lv_obj_set_style_text_color(lbl, UiColorForeground(), 0);
        }
    }
}

void Network_StyleScanBtn(bool enabled) {
    if (Network_State().ui.scan_btn == nullptr) {
        return;
    }
    if (enabled) {
        lv_obj_add_flag(Network_State().ui.scan_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(Network_State().ui.scan_btn, LV_OPA_COVER, 0);
    } else {
        lv_obj_clear_flag(Network_State().ui.scan_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(Network_State().ui.scan_btn, LV_OPA_50, 0);
    }
}

lv_obj_t* NetworkScreen::Create() {
    CloseTransfer();
    Network_State().generation.fetch_add(1);
    Network_State().transfer_mode = false;
    Network_State().retry_ssid.clear();
    Network_State().transfer_info = {};
    Network_State().screen_active = true;
    Network_State().scan_in_progress = false;
    Network_State().connect_in_progress = false;
    Network_State().scan_results.clear();
    Network_State().tab = Tab::kNearby;
    Network_State().nearby_page = 0;
    Network_State().saved_page = 0;
    Network_State().nearby_page_size = kPageSizeFallback;
    Network_State().saved_page_size = kPageSizeFallback;
    Network_State().ui = {};

    lv_obj_t* scr = lv_obj_create(nullptr);
    Network_State().ui.screen = scr;
    Network_Strip(scr);
    lv_obj_set_style_bg_color(scr, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    auto status = ScreenCreateStatusBar(scr);

    lv_obj_t* body = lv_obj_create(scr);
    Network_Strip(body);
    Network_State().ui.list_panel = body;
    lv_obj_set_size(body, LV_HOR_RES, LV_VER_RES - status.height);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_pad_all(body, kBodyPad, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, kBodyRowGap, 0);

    lv_obj_t* title = lv_label_create(body);
    lv_label_set_text(title, LangString_NETWORK_TITLE);
    lv_obj_set_style_text_font(title, UiFont(), 0);

    Network_State().ui.status_lbl = lv_label_create(body);
    lv_label_set_text(Network_State().ui.status_lbl, "");
    lv_obj_set_width(Network_State().ui.status_lbl, LV_PCT(100));
    lv_label_set_long_mode(Network_State().ui.status_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(Network_State().ui.status_lbl, UiFont(), 0);

    lv_obj_t* tab_row = lv_obj_create(body);
    Network_Strip(tab_row);
    lv_obj_set_width(tab_row, LV_PCT(100));
    lv_obj_set_height(tab_row, kTabRowH);
    lv_obj_set_flex_flow(tab_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tab_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tab_row, kTabGap, 0);

    Network_State().ui.nearby_tab_btn =
        Network_MakeBtn(tab_row, LangString_NETWORK_TAB_NEARBY, kTabBtnW, kTabBtnH, Network_OnNearbyTab);
    Network_State().ui.saved_tab_btn =
        Network_MakeBtn(tab_row, LangString_NETWORK_TAB_SAVED, kTabBtnW, kTabBtnH, Network_OnSavedTab);
    Network_StyleTabBtn(Network_State().ui.nearby_tab_btn, true);
    Network_StyleTabBtn(Network_State().ui.saved_tab_btn, false);

    Network_State().ui.list = lv_obj_create(body);
    Network_Strip(Network_State().ui.list);
    lv_obj_set_width(Network_State().ui.list, LV_PCT(100));
    lv_obj_set_flex_grow(Network_State().ui.list, 1);
    lv_obj_set_flex_flow(Network_State().ui.list, LV_FLEX_FLOW_COLUMN);

    // 底栏纵向：刷新/清空居中在上，页码居中在下
    lv_obj_t* foot = lv_obj_create(body);
    Network_Strip(foot);
    lv_obj_set_width(foot, LV_PCT(100));
    lv_obj_set_height(foot, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(foot, kFootRowGap, 0);

    lv_obj_t* action_row = lv_obj_create(foot);
    Network_Strip(action_row);
    lv_obj_set_width(action_row, LV_PCT(100));
    lv_obj_set_height(action_row, kActionRowH);
    lv_obj_set_flex_flow(action_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(action_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    Network_State().ui.scan_btn =
        Network_MakeBtn(action_row, LangString_NETWORK_SCAN, kActionBtnW, kActionBtnH, Network_OnScanClicked);
    Network_State().ui.clear_btn =
        Network_MakeBtn(action_row, LangString_NETWORK_CLEAR_ALL, kActionBtnW, kActionBtnH, Network_OnClearSaved);
    lv_obj_add_flag(Network_State().ui.clear_btn, LV_OBJ_FLAG_HIDDEN);

    Network_State().ui.page_lbl = lv_label_create(foot);
    // 先占位再量列表高：空字符串会让底栏偏矮，多算出一条压住刷新按钮
    lv_label_set_text(Network_State().ui.page_lbl, "1 / 1");
    lv_obj_set_width(Network_State().ui.page_lbl, LV_PCT(100));
    lv_obj_set_style_text_align(Network_State().ui.page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(Network_State().ui.page_lbl, UiFont(), 0);

    Network_BuildPasswordPage(scr, status.height);

    Network_RefreshPageSizes();
    Network_RebuildListPage();

    ScreenSetIsHome(false);
    VkKeyScreenDesc keys{NetworkScreen::Create, NetworkScreen::OnVkKey};
    keys.on_page_key = NetworkScreen::OnPageKey;
    VkKey_AttachScreen(scr, kScreenId, keys);

    Network_BindWifiSetup(scr);
    Network_ScheduleScan();
    return scr;
}

void NetworkScreen::BuildTransfer(lv_obj_t* parent) {
    auto& st = Network_State();
    if (st.screen_active || parent == nullptr) {
        return;
    }
    st.generation.fetch_add(1);
    st.settings_mode = false;
    st.transfer_mode = true;
    st.transfer_info = {};
    st.transfer_scan_pending = false;
    st.transfer_scan_message.clear();
    st.retry_ssid.clear();
    st.scan_results.clear();
    st.nearby_page = 0;
    st.saved_page = 0;
    st.tab = Tab::kNearby;
    st.scan_in_progress = false;
    st.connect_in_progress = false;
    st.ui = {};
    st.ui.screen = parent;
    st.screen_active = true;

    lv_obj_t* body = lv_obj_create(parent);
    Network_Strip(body);
    st.ui.list_panel = body;
    lv_obj_set_size(body, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(body, kBodyPad, 0);
    lv_obj_set_style_pad_bottom(body, kBodyPad / 4, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, kBodyRowGap, 0);
    st.ui.status_lbl = lv_label_create(body);
    lv_obj_set_width(st.ui.status_lbl, LV_PCT(100));
    lv_label_set_long_mode(st.ui.status_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(st.ui.status_lbl, Network_UiFont(), 0);
    lv_label_set_text(st.ui.status_lbl, "");
    lv_obj_add_flag(st.ui.status_lbl, LV_OBJ_FLAG_HIDDEN);

    st.ui.list = lv_obj_create(body);
    Network_Strip(st.ui.list);
    lv_obj_set_width(st.ui.list, LV_PCT(100));
    lv_obj_set_height(st.ui.list, 0);
    lv_obj_set_flex_grow(st.ui.list, 1);
    lv_obj_set_flex_flow(st.ui.list, LV_FLEX_FLOW_COLUMN);
    Network_BuildPasswordPage(parent, 0);
    lv_obj_set_size(st.ui.pwd_page, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(st.ui.pwd_page, LV_OBJ_FLAG_FLOATING);
    Network_BindWifiSetup(parent);
    Network_ScheduleScan();
    Network_RebuildListPage();
}

bool NetworkScreen::BuildSettings(lv_obj_t* parent) {
    auto& state = Network_State();
    if (!parent) {
        return false;
    }
    if (state.screen_active) {
        return state.settings_mode && state.ui.screen == parent;
    }
    lv_obj_update_layout(parent);
    state.generation.fetch_add(1);
    state.settings_mode = true;
    state.settings_detail = false;
    state.settings_list_dirty = false;
    state.transfer_mode = false;
    state.scan_results.clear();
    state.ui = {};
    state.ui.screen = parent;
    state.screen_active = true;
    auto* body = lv_obj_create(parent);
    Network_Strip(body);
    state.ui.list_panel = body;
    lv_obj_set_size(body, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    auto* list = lv_obj_create(body);
    Network_Strip(list);
    lv_obj_set_size(list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, UiSy(8), 0);
    state.ui.list = list;
    Network_BuildPasswordPage(parent, 0);
    lv_obj_set_size(state.ui.pwd_page, LV_PCT(100), lv_obj_get_content_height(parent));
    lv_obj_add_flag(state.ui.pwd_page, LV_OBJ_FLAG_FLOATING);
    Network_BindWifiSetup(parent);
    Network_ScheduleScan();
    Network_RebuildListPage();
    return true;
}

void NetworkScreen::CloseSettings() {
    auto& state = Network_State();
    if (!state.settings_mode) {
        return;
    }
    auto* body = state.ui.list_panel;
    auto* password = state.ui.pwd_page;
    Network_OnScreenUnload(nullptr);
    state.settings_mode = false;
    state.settings_detail = false;
    if (body && lv_obj_is_valid(body)) {
        lv_obj_delete(body);
    }
    if (password && lv_obj_is_valid(password)) {
        lv_obj_delete(password);
    }
}

void NetworkScreen::CloseTransfer() {
    auto& st = Network_State();
    if (!st.transfer_mode || !st.screen_active) {
        return;
    }
    lv_obj_t* parent = st.ui.screen;
    Network_OnScreenUnload(nullptr);
    st.transfer_mode = false;
    if (parent != nullptr && lv_obj_is_valid(parent)) {
        lv_obj_clean(parent);
    }
}

bool NetworkScreen::OnPageKey(const char* key_name, bool long_press) {
    if (!Network_ScreenAlive()) {
        return false;
    }
    if (Network_IsPasswordPageOpen()) {
        return true;
    }
    if (Network_State().settings_mode || Network_State().transfer_mode) {
        return false;
    }
    if (!long_press) {
        OnVkKey(key_name);
    }
    return true;
}

bool NetworkScreen::OnVkKey(const char* key_name) {
    if (key_name == nullptr || !Network_ScreenAlive()) {
        return false;
    }
    if (Network_IsPasswordPageOpen()) {
        if (std::strcmp(key_name, "vk_prev") == 0 || std::strcmp(key_name, "vk_home") == 0) {
            ScreenLvAsync([](void*) {
                if (Network_ScreenAlive()) {
                    Network_ClosePasswordPage();
                }
            });
            return true;
        }
        return true;  // 密码页吞掉翻页键
    }
    if (Network_State().settings_mode && std::strcmp(key_name, "vk_home") == 0 && Network_SettingsBack()) {
        return true;
    }
    if (Network_State().settings_mode) {
        return false;
    }
    if (Network_State().transfer_mode) {
        return std::strcmp(key_name, "vk_next") == 0;
    }
    if (std::strcmp(key_name, "vk_prev") == 0) {
        if (Network_State().tab == Tab::kNearby) {
            if (Network_State().nearby_page > 0) {
                --Network_State().nearby_page;
                Network_PostRebuildList();
                return true;
            }
        } else if (Network_State().saved_page > 0) {
            --Network_State().saved_page;
            Network_PostRebuildList();
            return true;
        }
        return false;  // 首页交给默认返回
    }
    if (std::strcmp(key_name, "vk_next") == 0) {
        if (Network_State().tab == Tab::kNearby) {
            if (Network_State().nearby_page + 1 < Network_NearbyPageCount()) {
                ++Network_State().nearby_page;
                Network_PostRebuildList();
            }
        } else if (Network_State().saved_page + 1 < Network_SavedPageCount()) {
            ++Network_State().saved_page;
            Network_PostRebuildList();
        }
        return true;
    }
    return false;
}
