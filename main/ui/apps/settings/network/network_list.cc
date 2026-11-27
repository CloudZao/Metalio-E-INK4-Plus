#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <lvgl.h>
#include "ssid_manager.h"
#include "wifi_station.h"
#include "ui/apps/settings/network/network_list.h"
#include "ui/apps/settings/network/network_wifi.h"
#include "assets/lang_config.h"
#include "assets/lang_network.h"
#include "ui/apps/settings/settings_common.h"
#include "ui/apps/settings/settings_screen.h"
#include "ui/apps/settings/network/network_screen_priv.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/apps/book/book_text_util.h"
#include "ui/apps/settings/network/network_screen.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"
#include "ui/common/widgets/screen_common.h"

static void ApplyScanResult(void* p) {
    auto* msg = static_cast<NetworkStatusMessage*>(p);
    auto& st = Network_State();
    if (Network_ScreenAlive() && st.transfer_mode && msg->generation == st.generation.load()) {
        st.transfer_scan_message = msg->text;
        if (st.transfer_scan_timer) {
            lv_timer_delete(st.transfer_scan_timer);
        }
        st.transfer_scan_timer = lv_timer_create(
            [](lv_timer_t* timer) {
                auto& state = Network_State();
                state.transfer_scan_timer = nullptr;
                state.transfer_scan_pending = false;
                state.transfer_scan_message.clear();
                lv_timer_delete(timer);
                if (Network_ScreenAlive() && state.transfer_mode) {
                    Network_RebuildListPage();
                }
            },
            1200, nullptr);
        if (!st.transfer_scan_timer) {
            st.transfer_scan_pending = false;
            st.transfer_scan_message.clear();
        }
        Network_RebuildListPage();
    }
    delete msg;
}

void Network_PostScanResult(const char* text) {
    auto& st = Network_State();
    if (!st.transfer_mode) {
        Network_PostStatus(text);
        return;
    }
    if (!st.screen_active) {
        return;
    }
    auto* msg = new NetworkStatusMessage{};
    msg->generation = st.generation.load();
    snprintf(msg->text, sizeof(msg->text), "%s", text ? text : "");
    if (!ScreenLvAsync(ApplyScanResult, msg)) {
        delete msg;
    }
}

static void OnTransferPasswordClicked(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto* ctx = static_cast<NearbyClickCtx*>(lv_event_get_user_data(e));
    if (!ctx || Network_State().scan_in_progress || Network_State().connect_in_progress) {
        return;
    }
    Network_OpenPasswordPage(ctx->ssid, static_cast<wifi_auth_mode_t>(ctx->authmode));
}

static void RebuildTransferLists() {
    auto& st = Network_State();
    if (st.ui.list == nullptr) {
        return;
    }
    const bool busy = st.scan_in_progress || st.transfer_scan_pending;
    st.ui.scan_btn = nullptr;
    lv_obj_scroll_to_y(st.ui.list, 0, LV_ANIM_OFF);
    lv_obj_clear_flag(st.ui.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(st.ui.list, LV_DIR_NONE);
    lv_obj_clear_flag(st.ui.list, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_clear_flag(st.ui.list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_clear_flag(st.ui.list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scrollbar_mode(st.ui.list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clean(st.ui.list);
    lv_obj_update_layout(st.ui.screen);
    if (busy) {
        auto* message = lv_label_create(st.ui.list);
        lv_obj_add_flag(message, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_width(message, LV_PCT(100));
        lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(message, Network_UiFont(), 0);
        lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(message, st.transfer_scan_message.empty() ? LangString_NETWORK_SCANNING_SHORT
                                                                    : st.transfer_scan_message.c_str());
        lv_obj_center(message);
        return;
    }
    const auto saved = SsidManager::GetInstance().GetSsidList();
    int counts[2] = {};
    for (const auto& ap : st.scan_results) {
        const bool known =
            std::any_of(saved.begin(), saved.end(), [&](const SsidItem& item) { return item.ssid == ap.ssid; });
        ++counts[known ? 0 : 1];
    }
    const lv_coord_t action_side = UiSx(30);
    const lv_coord_t row_h = std::max(Network_NearbyRowHEstimate(), action_side + UiSy(6));
    const lv_coord_t heading_h = Network_UiLineH() + kBodyRowGap;
    const lv_coord_t box_pad = kRowPadVer;
    const lv_coord_t box_border = kBtnBorderW;
    const lv_coord_t footer_pad_top = UiSy(10);
    const lv_coord_t footer_pad_bottom = UiSy(7) / 2;
    const lv_coord_t footer_h = kActionBtnH + footer_pad_top + footer_pad_bottom;
    const lv_coord_t available =
        std::max<lv_coord_t>(0, lv_obj_get_content_height(st.ui.list) - 2 * heading_h - footer_h - 2 * kBodyRowGap);
    lv_coord_t box_h[2];
    for (int group = 0; group < 2; ++group) {
        box_h[group] = std::max<lv_coord_t>(UiSy(64), counts[group] * row_h + 2 * (box_pad + box_border));
    }
    // 为标题和刷新按钮预留空间，只有各自边框内的条目参与滚动。
    if (box_h[0] + box_h[1] > available) {
        const lv_coord_t saved_h = std::min<lv_coord_t>(box_h[0], available / 2);
        box_h[1] = std::min<lv_coord_t>(box_h[1], available - saved_h);
        box_h[0] = std::min<lv_coord_t>(box_h[0], available - box_h[1]);
    }
    lv_obj_set_style_pad_row(st.ui.list, kBodyRowGap, 0);
    for (int group = 0; group < 2; ++group) {
        const bool known = group == 0;
        lv_obj_t* section = lv_obj_create(st.ui.list);
        Network_Strip(section);
        lv_obj_clear_flag(section, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_width(section, LV_PCT(100));
        lv_obj_set_height(section, heading_h + box_h[group]);
        lv_obj_set_flex_flow(section, LV_FLEX_FLOW_COLUMN);
        auto* heading = lv_label_create(section);
        lv_label_set_text_fmt(
            heading, known ? LangString_NETWORK_SAVED_COUNT_FMT : LangString_NETWORK_UNSAVED_COUNT_FMT, counts[group]);
        lv_obj_set_style_text_font(heading, Network_UiFont(), 0);
        lv_obj_set_height(heading, heading_h);
        auto* box = lv_obj_create(section);
        Network_Strip(box);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(box, LV_PCT(100), box_h[group]);
        lv_obj_set_style_border_width(box, box_border, 0);
        lv_obj_set_style_border_color(box, UiColorForeground(), 0);
        lv_obj_set_style_radius(box, kBtnRadius, 0);
        lv_obj_set_style_pad_all(box, box_pad, 0);
        auto* rows = lv_obj_create(box);
        Network_Strip(rows);
        lv_obj_set_size(rows, LV_PCT(100), LV_PCT(100));
        lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_flag(rows, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(rows, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
        lv_obj_clear_flag(rows, LV_OBJ_FLAG_SCROLL_MOMENTUM);
        lv_obj_clear_flag(rows, LV_OBJ_FLAG_SCROLL_ELASTIC);
        lv_obj_set_scroll_dir(rows, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(rows, LV_SCROLLBAR_MODE_AUTO);
        for (const auto& ap : st.scan_results) {
            const bool is_saved =
                std::any_of(saved.begin(), saved.end(), [&](const SsidItem& item) { return item.ssid == ap.ssid; });
            if (is_saved != known) {
                continue;
            }
            auto* row = lv_obj_create(rows);
            Network_Strip(row);
            lv_obj_set_size(row, LV_PCT(100), row_h);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_color(row, UiColorForeground(), 0);
            lv_obj_set_style_pad_hor(row, kRowPadHor, 0);
            lv_obj_set_style_pad_column(row, UiSx(8), 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            HapticAttachClick(row);
            auto* ctx = new NearbyClickCtx{};
            strlcpy(ctx->ssid, ap.ssid.c_str(), sizeof(ctx->ssid));
            ctx->authmode = static_cast<int>(ap.authmode);
            lv_obj_add_event_cb(row, Network_OnNearbyItemClicked, LV_EVENT_CLICKED, ctx);
            lv_obj_add_event_cb(row, Network_OnNearbyItemDelete, LV_EVENT_DELETE, ctx);
            auto* name = lv_label_create(row);
            lv_obj_set_width(name, 1);
            lv_obj_set_flex_grow(name, 1);
            lv_obj_set_style_text_font(name, Network_UiFont(), 0);
            lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);
            auto* signal = lv_label_create(row);
            lv_label_set_text_fmt(signal, "%d dBm", ap.rssi);
            lv_obj_set_style_text_font(signal, Network_UiFont(), 0);
            lv_obj_clear_flag(signal, LV_OBJ_FLAG_CLICKABLE);
            auto* action = Network_MakeBtn(row, "!", action_side, action_side, OnTransferPasswordClicked, ctx);
            lv_obj_set_style_radius(action, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_height(name, Network_UiLineH());
            Book_SetFilenameMiddle(name, ap.ssid.c_str());
        }
        if (counts[group] == 0) {
            lv_obj_set_flex_align(rows, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            auto* hint = lv_label_create(rows);
            lv_obj_set_width(hint, LV_PCT(100));
            lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_text(hint, known ? LangString_NETWORK_NO_SAVED_NEARBY : LangString_NETWORK_NO_UNSAVED_NEARBY);
            lv_obj_set_style_text_font(hint, Network_UiFont(), 0);
        }
    }
    lv_obj_t* footer = lv_obj_create(st.ui.list);
    Network_Strip(footer);
    lv_obj_set_size(footer, LV_PCT(100), footer_h);
    lv_obj_set_style_pad_top(footer, footer_pad_top, 0);
    lv_obj_set_style_pad_bottom(footer, footer_pad_bottom, 0);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    st.ui.scan_btn = Network_MakeBtn(footer, LangString_CLOUD_REFRESH, kActionBtnW, kActionBtnH, Network_OnScanClicked);
}

void Network_OnSavedSetDefault(lv_event_t* e) {
    auto* ctx = static_cast<SavedActionCtx*>(lv_event_get_user_data(e));
    if (ctx == nullptr) {
        return;
    }
    Network_ScheduleSavedChange(WifiSetupOperation::SetDefault, ctx->ssid);
}

void Network_RebuildNearbyPage() {
    if (Network_State().ui.list == nullptr) {
        return;
    }
    lv_obj_clean(Network_State().ui.list);

    if (Network_State().ui.clear_btn != nullptr) {
        lv_obj_add_flag(Network_State().ui.clear_btn, LV_OBJ_FLAG_HIDDEN);
    }

    if (Network_State().scan_in_progress) {
        lv_obj_t* hint = lv_label_create(Network_State().ui.list);
        lv_label_set_text(hint, LangString_NETWORK_SCANNING);
        lv_obj_set_style_text_font(hint, UiFont(), 0);
        // 页码保持占位高度，避免底栏变矮后列表区虚高、末行压住刷新
        if (Network_State().ui.page_lbl != nullptr) {
            lv_label_set_text(Network_State().ui.page_lbl, "1 / 1");
        }
        return;
    }

    if (Network_State().scan_results.empty()) {
        lv_obj_t* hint = lv_label_create(Network_State().ui.list);
        lv_label_set_text(hint, LangString_NETWORK_NEARBY_EMPTY);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(hint, UiFont(), 0);
        if (Network_State().ui.page_lbl != nullptr) {
            lv_label_set_text(Network_State().ui.page_lbl, "1 / 1");
        }
        return;
    }

    const int pages = Network_NearbyPageCount();
    if (Network_State().nearby_page < 0) {
        Network_State().nearby_page = 0;
    }
    if (Network_State().nearby_page >= pages) {
        Network_State().nearby_page = pages - 1;
    }
    if (Network_State().ui.page_lbl != nullptr) {
        char foot[32];
        snprintf(foot, sizeof(foot), "%d / %d", Network_State().nearby_page + 1, pages);
        lv_label_set_text(Network_State().ui.page_lbl, foot);
    }

    const int ps = std::max(1, Network_State().nearby_page_size);
    const int start = Network_State().nearby_page * ps;
    const int end = std::min(start + ps, static_cast<int>(Network_State().scan_results.size()));
    for (int i = start; i < end; ++i) {
        const auto& ap = Network_State().scan_results[static_cast<size_t>(i)];
        lv_obj_t* row = lv_obj_create(Network_State().ui.list);
        Network_Strip(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_border_color(row, UiColorForeground(), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(row);

        auto* ctx = new NearbyClickCtx{};
        strlcpy(ctx->ssid, ap.ssid.c_str(), sizeof(ctx->ssid));
        ctx->authmode = static_cast<int>(ap.authmode);
        lv_obj_add_event_cb(row, Network_OnNearbyItemClicked, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(row, Network_OnNearbyItemDelete, LV_EVENT_DELETE, ctx);

        lv_obj_t* left = lv_label_create(row);
        char title[80];
        snprintf(title, sizeof(title), "%s %s", ap.ssid.c_str(), Network_AuthLabel(ap.authmode));
        lv_label_set_text(left, title);
        lv_obj_set_style_text_font(left, UiFont(), 0);
        lv_obj_clear_flag(left, LV_OBJ_FLAG_CLICKABLE);
        Network_StyleWifiNameRow(row, left);

        char meta[24];
        snprintf(meta, sizeof(meta), "%ddBm", ap.rssi);
        lv_obj_t* right = lv_label_create(row);
        lv_label_set_text(right, meta);
        lv_obj_set_style_text_font(right, UiFont(), 0);
        lv_obj_clear_flag(right, LV_OBJ_FLAG_CLICKABLE);
    }
}

void Network_SetStatus(const char* text) {
    if (Network_State().transfer_mode) {
        return;
    }
    if (Network_State().ui.status_lbl != nullptr) {
        lv_label_set_text(Network_State().ui.status_lbl, text != nullptr ? text : "");
    }
}

void Network_AsyncSetStatus(void* p) {
    auto* msg = static_cast<NetworkStatusMessage*>(p);
    if (Network_ScreenAlive() && msg->generation == Network_State().generation.load()) {
        Network_SetStatus(msg->text);
    }
    delete msg;
}

void Network_RebuildSavedPage() {
    if (Network_State().ui.list == nullptr) {
        return;
    }
    lv_obj_clean(Network_State().ui.list);

    const auto list = SsidManager::GetInstance().GetSsidList();
    if (Network_State().ui.clear_btn != nullptr) {
        if (list.empty()) {
            lv_obj_add_flag(Network_State().ui.clear_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(Network_State().ui.clear_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (list.empty()) {
        lv_obj_t* hint = lv_label_create(Network_State().ui.list);
        lv_label_set_text(hint, LangString_NETWORK_SAVED_EMPTY);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(hint, UiFont(), 0);
        if (Network_State().ui.page_lbl != nullptr) {
            lv_label_set_text(Network_State().ui.page_lbl, "1 / 1");
        }
        return;
    }

    const int ps = std::max(1, Network_State().saved_page_size);
    const int pages = std::max(1, (static_cast<int>(list.size()) + ps - 1) / ps);
    if (Network_State().saved_page < 0) {
        Network_State().saved_page = 0;
    }
    if (Network_State().saved_page >= pages) {
        Network_State().saved_page = pages - 1;
    }
    if (Network_State().ui.page_lbl != nullptr) {
        char foot[32];
        snprintf(foot, sizeof(foot), "%d / %d", Network_State().saved_page + 1, pages);
        lv_label_set_text(Network_State().ui.page_lbl, foot);
    }

    const int start = Network_State().saved_page * ps;
    const int end = std::min(start + ps, static_cast<int>(list.size()));
    for (int i = start; i < end; ++i) {
        const auto& item = list[static_cast<size_t>(i)];
        lv_obj_t* row = lv_obj_create(Network_State().ui.list);
        Network_Strip(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_border_color(row, UiColorForeground(), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 6, 0);

        lv_obj_t* name = lv_label_create(row);
        char ttext[96];
        if (i == 0) {
            snprintf(ttext, sizeof(ttext), LangString_NETWORK_DEFAULT_FMT, item.ssid.c_str());
        } else {
            snprintf(ttext, sizeof(ttext), "%s", item.ssid.c_str());
        }
        lv_label_set_text(name, ttext);
        lv_obj_set_style_text_font(name, UiFont(), 0);
        Network_StyleWifiNameRow(row, name);

        if (i != 0) {
            auto* def_ctx = new SavedActionCtx{item.ssid};
            lv_obj_t* def_btn = Network_MakeBtn(row, LangString_NETWORK_SET_DEFAULT, kSavedDefBtnW, kSavedActionBtnH,
                                                Network_OnSavedSetDefault, def_ctx);
            lv_obj_add_event_cb(def_btn, Network_OnSavedBtnDelete, LV_EVENT_DELETE, def_ctx);
        }

        auto* del_ctx = new SavedActionCtx{item.ssid};
        lv_obj_t* del_btn = Network_MakeBtn(row, LangString_NETWORK_DELETE, kSavedDelBtnW, kSavedActionBtnH,
                                            Network_OnSavedRemove, del_ctx);
        lv_obj_add_event_cb(del_btn, Network_OnSavedBtnDelete, LV_EVENT_DELETE, del_ctx);
    }
}

void Network_SwitchTab(Tab tab) {
    if (Network_State().tab == tab) {
        return;
    }
    Network_State().tab = tab;
    Network_StyleTabBtn(Network_State().ui.nearby_tab_btn, Network_State().tab == Tab::kNearby);
    Network_StyleTabBtn(Network_State().ui.saved_tab_btn, Network_State().tab == Tab::kSaved);
    if (Network_State().ui.scan_btn != nullptr) {
        if (Network_State().tab == Tab::kNearby) {
            lv_obj_clear_flag(Network_State().ui.scan_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(Network_State().ui.scan_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    Network_RebuildListPage();
}

void Network_AsyncRebuildList(void* /*p*/) {
    if (Network_ScreenAlive()) {
        Network_RebuildListPage();
    }
}

void Network_OnNearbyItemDelete(lv_event_t* e) {
    delete static_cast<NearbyClickCtx*>(lv_event_get_user_data(e));
}

void Network_OnSavedRemove(lv_event_t* e) {
    auto* ctx = static_cast<SavedActionCtx*>(lv_event_get_user_data(e));
    if (ctx == nullptr) {
        return;
    }
    const std::string ssid = ctx->ssid;
    ScreenConfirmDelete(lv_obj_get_screen(lv_event_get_target_obj(e)), [ssid] {
        Network_ScheduleSavedChange(WifiSetupOperation::Remove, ssid);
    });
}

int Network_SavedPageCount() {
    const size_t n = SsidManager::GetInstance().GetSsidList().size();
    if (n == 0) {
        return 1;
    }
    const int ps = std::max(1, Network_State().saved_page_size);
    return static_cast<int>((n + ps - 1) / ps);
}

void Network_OnNearbyTab(lv_event_t* /*e*/) {
    Network_SwitchTab(Tab::kNearby);
}

int Network_NearbyPageCount() {
    if (Network_State().scan_results.empty()) {
        return 1;
    }
    const int ps = std::max(1, Network_State().nearby_page_size);
    return static_cast<int>((Network_State().scan_results.size() + ps - 1) / ps);
}

static const char* SettingsWifiText(const char* zh, const char* en) {
    return std::strcmp(LangCode, "zh-CN") == 0 ? zh : en;
}

static void SettingsNetworkMessage(lv_obj_t* parent, const char* text) {
    auto* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), UiSy(64));
    lv_obj_clear_flag(box, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    auto* label = lv_label_create(box);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, UiFont(), 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
}

static void ShowSettingsNetwork(std::string ssid, wifi_auth_mode_t authmode) {
    auto& state = Network_State();
    state.settings_detail = true;
    state.pending_ssid = ssid;
    state.pending_authmode = authmode;
    lv_obj_clean(state.ui.list);
    SettingsScreen::SetWifiDetail(ssid.c_str());
    auto* info = SettingsCreateListGroup(state.ui.list);
    SettingsCreateListRow(info, SettingsWifiText("网络名称", "Network name"), ssid.c_str());
    const auto saved = SsidManager::GetInstance().GetSsidList();
    const bool known = std::any_of(saved.begin(), saved.end(), [&](const SsidItem& item) {
        return item.ssid == ssid;
    });
    wifi_ap_record_t connected{};
    const bool active = esp_wifi_sta_get_ap_info(&connected) == ESP_OK &&
                        ssid == reinterpret_cast<const char*>(connected.ssid);
    SettingsCreateListRow(info, SettingsWifiText("状态", "Status"),
                          active ? SettingsWifiText("已连接", "Connected") :
                          known ? SettingsWifiText("已保存", "Saved") : SettingsWifiText("未连接", "Not connected"));
    const auto ap = std::find_if(state.scan_results.begin(), state.scan_results.end(),
                                 [&](const ApItem& item) { return item.ssid == ssid; });
    char signal[32];
    std::snprintf(signal, sizeof(signal), "%s", SettingsWifiText("未在附近", "Not nearby"));
    if (ap != state.scan_results.end()) {
        std::snprintf(signal, sizeof(signal), "%d dBm", ap->rssi);
    }
    SettingsCreateListRow(info, SettingsWifiText("信号强度", "Signal strength"), signal);
    SettingsCreateListRow(info, SettingsWifiText("安全性", "Security"), Network_AuthLabel(authmode));
    auto* actions = SettingsCreateOptionGrid(state.ui.list, known ? 2 : 1, 1);
    ScreenCreateSelectableOption(actions, SettingsWifiText("连接", "Connect"), [](lv_event_t*) {
        auto& state = Network_State();
        const auto saved = SsidManager::GetInstance().GetSsidList();
        const auto found = std::find_if(saved.begin(), saved.end(), [&](const SsidItem& item) {
            return item.ssid == state.pending_ssid;
        });
        if (found != saved.end()) {
            Network_ScheduleConnect(found->ssid, found->password);
        } else if (state.pending_authmode == WIFI_AUTH_OPEN) {
            Network_ScheduleConnect(state.pending_ssid, "");
        } else {
            Network_OpenPasswordPage(state.pending_ssid, state.pending_authmode);
        }
    }, 0);
    if (known) {
        ScreenCreateSelectableOption(actions, SettingsWifiText("忘记网络", "Forget network"), [](lv_event_t*) {
            Network_ScheduleSavedChange(WifiSetupOperation::Remove, Network_State().pending_ssid);
            Network_SettingsBack();
        }, 0);
    }
}

static void AddSettingsNetwork(lv_obj_t* group, const std::string& ssid, wifi_auth_mode_t authmode, const char* signal) {
    auto* context = new NearbyClickCtx{};
    std::snprintf(context->ssid, sizeof(context->ssid), "%s", ssid.c_str());
    context->authmode = static_cast<int>(authmode);
    auto* row = SettingsCreateListRow(group, ssid.c_str(), signal, [](lv_event_t* event) {
        const auto* context = static_cast<NearbyClickCtx*>(lv_event_get_user_data(event));
        ShowSettingsNetwork(context->ssid, static_cast<wifi_auth_mode_t>(context->authmode));
    }, reinterpret_cast<intptr_t>(context));
    lv_obj_add_event_cb(row, Network_OnNearbyItemDelete, LV_EVENT_DELETE, context);
}

static void RebuildSettingsNetworks() {
    auto& state = Network_State();
    if (!state.ui.list || state.settings_detail) {
        return;
    }
    // 周期扫描时保留上次列表，避免当前阅读位置和按下目标反复变化。
    if (state.scan_in_progress && lv_obj_get_child_count(state.ui.list) > 0) {
        return;
    }
    if (TouchUiFingerIsDown()) {
        state.settings_list_dirty = true;
        return;
    }
    state.settings_list_dirty = false;
    const int offset = lv_obj_get_scroll_y(state.ui.screen);
    lv_obj_clean(state.ui.list);
    const auto saved = SsidManager::GetInstance().GetSsidList();
    lv_obj_set_style_pad_row(state.ui.list, kSettingsSectionGap, 0);
    auto* saved_section = SettingsCreateSection(state.ui.list, SettingsWifiText("已保存的网络", "Saved networks"));
    auto* saved_group = SettingsCreateListGroup(saved_section);
    for (const auto& item : saved) {
        const auto ap = std::find_if(state.scan_results.begin(), state.scan_results.end(),
                                     [&](const ApItem& entry) { return entry.ssid == item.ssid; });
        char signal[32];
        std::snprintf(signal, sizeof(signal), "%s", SettingsWifiText("未在附近", "Not nearby"));
        if (ap != state.scan_results.end()) {
            std::snprintf(signal, sizeof(signal), "%d dBm", ap->rssi);
        }
        AddSettingsNetwork(saved_group, item.ssid, ap != state.scan_results.end() ? ap->authmode :
                           item.password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK, signal);
    }
    if (saved.empty()) {
        SettingsNetworkMessage(saved_group, LangString_NETWORK_SAVED_EMPTY);
    }
    auto* other_section = SettingsCreateSection(state.ui.list, SettingsWifiText("其它网络", "Other networks"));
    auto* other_group = SettingsCreateListGroup(other_section);
    for (const auto& ap : state.scan_results) {
        if (std::any_of(saved.begin(), saved.end(), [&](const SsidItem& item) { return item.ssid == ap.ssid; })) {
            continue;
        }
        char signal[24];
        std::snprintf(signal, sizeof(signal), "%d dBm", ap.rssi);
        AddSettingsNetwork(other_group, ap.ssid, ap.authmode, signal);
    }
    if (lv_obj_get_child_count(other_group) == 0) {
        SettingsNetworkMessage(other_group, state.scan_in_progress ?
            SettingsWifiText("正在扫描附近网络…", "Scanning nearby networks...") :
            SettingsWifiText("未发现附近网络", "No nearby networks"));
    }
    lv_obj_update_layout(state.ui.screen);
    lv_obj_scroll_to_y(state.ui.screen, offset, LV_ANIM_OFF);
}

bool Network_SettingsBack() {
    if (!Network_State().settings_mode || !Network_State().settings_detail) {
        return false;
    }
    Network_State().settings_detail = false;
    SettingsScreen::SetWifiDetail(nullptr);
    RebuildSettingsNetworks();
    return true;
}

void Network_RebuildListPage() {
    if (Network_State().settings_mode) {
        RebuildSettingsNetworks();
        return;
    }
    if (Network_State().transfer_mode) {
        RebuildTransferLists();
        return;
    }
    if (Network_State().tab == Tab::kNearby) {
        Network_RebuildNearbyPage();
    } else {
        Network_RebuildSavedPage();
    }
}

void Network_OnSavedTab(lv_event_t* /*e*/) {
    Network_SwitchTab(Tab::kSaved);
}

void Network_OnSavedBtnDelete(lv_event_t* e) {
    delete static_cast<SavedActionCtx*>(lv_event_get_user_data(e));
}

void Network_PostStatus(const char* text) {
    if (!Network_State().screen_active) {
        return;
    }
    auto* msg = new NetworkStatusMessage{};
    msg->generation = Network_State().generation.load();
    snprintf(msg->text, sizeof(msg->text), "%s", text != nullptr ? text : "");
    if (!ScreenLvAsync(Network_AsyncSetStatus, msg)) {
        delete msg;
    }
}

void Network_OnNearbyItemClicked(lv_event_t* e) {
    auto* ctx = static_cast<NearbyClickCtx*>(lv_event_get_user_data(e));
    if (ctx == nullptr) {
        return;
    }
    const auto auth = static_cast<wifi_auth_mode_t>(ctx->authmode);
    if (Network_State().transfer_mode) {
        for (const auto& saved : SsidManager::GetInstance().GetSsidList()) {
            if (saved.ssid == ctx->ssid && Network_State().retry_ssid != saved.ssid) {
                Network_ScheduleConnect(saved.ssid, saved.password);
                return;
            }
        }
    }
    if (auth == WIFI_AUTH_OPEN) {
        Network_ScheduleConnect(ctx->ssid, "");
    } else {
        Network_OpenPasswordPage(ctx->ssid, auth);
    }
}

void Network_OnClearSaved(lv_event_t* /*e*/) {
    Network_ScheduleSavedChange(WifiSetupOperation::Clear);
}

void Network_PostRebuildList() {
    if (!Network_State().screen_active) {
        return;
    }
    // 列表重建使用普通 UI 队列，与状态更新按入队顺序执行。
    if (!ScreenLvAsync(Network_AsyncRebuildList)) {
        ESP_LOGW(TAG, "Network_PostRebuildList async failed");
    }
}

void Network_OnScanClicked(lv_event_t* /*e*/) {
    Network_ScheduleScan();
}
