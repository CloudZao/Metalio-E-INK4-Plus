// network_list.cc — split from parent .cc
#include "network_screen/network_screen.h"
#include "network_screen_priv.h"
#include "network_list.h"
#include "network_screen_ui.h"
#include "book_screen/book_text_util.h"

#include "application.h"
#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "power_policy.h"
#include "screen_common.h"
#include "ssid_manager.h"
#include "vk_key_handler.h"
#include "wifi_station.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdint>
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

void Network_SetStatus(const char* text) {
    if (Network_State().transfer_mode) return;
    if (Network_State().ui.status_lbl != nullptr) {
        lv_label_set_text(Network_State().ui.status_lbl, text != nullptr ? text : "");
    }
}

void Network_AsyncSetStatus(void* p) {
    auto* msg = static_cast<AsyncStatusMsg*>(p);
    if (Network_ScreenAlive() && msg->generation == Network_State().generation.load()) {
        Network_SetStatus(msg->text);
    }
    delete msg;
}

void Network_PostStatus(const char* text) {
    if (!Network_State().screen_active) {
        return;
    }
    auto* msg = new AsyncStatusMsg{};
    msg->generation = Network_State().generation.load();
    snprintf(msg->text, sizeof(msg->text), "%s", text != nullptr ? text : "");
    if (!ScreenLvAsync(Network_AsyncSetStatus, msg)) {
        delete msg;
    }
}

static void ApplyScanResult(void* p) {
    auto* msg = static_cast<AsyncStatusMsg*>(p);
    auto& st = Network_State();
    if (Network_ScreenAlive() && st.transfer_mode && msg->generation == st.generation.load()) {
        st.transfer_scan_message = msg->text;
        if (st.transfer_scan_timer) lv_timer_delete(st.transfer_scan_timer);
        st.transfer_scan_timer = lv_timer_create([](lv_timer_t* timer) {
            auto& state = Network_State();
            state.transfer_scan_timer = nullptr;
            state.transfer_scan_pending = false;
            state.transfer_scan_message.clear();
            lv_timer_delete(timer);
            if (Network_ScreenAlive() && state.transfer_mode) Network_RebuildListPage();
        }, 1200, nullptr);
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
    if (!st.screen_active) return;
    auto* msg = new AsyncStatusMsg{};
    msg->generation = st.generation.load();
    snprintf(msg->text, sizeof(msg->text), "%s", text ? text : "");
    if (!ScreenLvAsync(ApplyScanResult, msg)) delete msg;
}

void Network_AsyncRebuildList(void* /*p*/) {
    if (Network_ScreenAlive()) {
        Network_RebuildListPage();
    }
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

void Network_AsyncStyleScan(void* p) {
    const bool enabled = (reinterpret_cast<intptr_t>(p) != 0);
    if (Network_ScreenAlive()) {
        Network_StyleScanBtn(enabled);
    }
}

void Network_PostScanBtnEnabled(bool enabled) {
    if (!Network_State().screen_active) {
        return;
    }
    ScreenLvAsync(Network_AsyncStyleScan, reinterpret_cast<void*>(static_cast<intptr_t>(enabled ? 1 : 0)));
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

void Network_OnNearbyItemDelete(lv_event_t* e) {
    delete static_cast<NearbyClickCtx*>(lv_event_get_user_data(e));
}

void Network_OnSavedSetDefault(lv_event_t* e) {
    auto* ctx = static_cast<SavedActionCtx*>(lv_event_get_user_data(e));
    if (ctx == nullptr) {
        return;
    }
    SsidManager::GetInstance().SetDefaultSsid(ctx->ssid);
    Network_State().saved_page = 0;
    Network_RebuildListPage();
    Network_SetStatus(Lang::Strings::NETWORK_SET_DEFAULT_OK);
}

void Network_OnSavedRemove(lv_event_t* e) {
    auto* ctx = static_cast<SavedActionCtx*>(lv_event_get_user_data(e));
    if (ctx == nullptr) {
        return;
    }
    SsidManager::GetInstance().RemoveSsid(ctx->ssid);
    Network_RebuildListPage();
    Network_SetStatus(Lang::Strings::NETWORK_DELETED_OK);
}

void Network_OnSavedBtnDelete(lv_event_t* e) {
    delete static_cast<SavedActionCtx*>(lv_event_get_user_data(e));
}

void Network_OnClearSaved(lv_event_t* /*e*/) {
    SsidManager::GetInstance().Clear();
    Network_State().saved_page = 0;
    Network_RebuildListPage();
    Network_SetStatus(Lang::Strings::NETWORK_CLEARED_OK);
}

static void OnTransferPasswordClicked(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto* ctx = static_cast<NearbyClickCtx*>(lv_event_get_user_data(e));
    if (!ctx || Network_State().scan_in_progress || Network_State().connect_in_progress) return;
    Network_OpenPasswordPage(ctx->ssid, static_cast<wifi_auth_mode_t>(ctx->authmode));
}

static void RebuildTransferLists() {
    auto& st = Network_State();
    if (st.ui.list == nullptr) return;
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
        lv_label_set_text(message, st.transfer_scan_message.empty() ? "正在扫描" : st.transfer_scan_message.c_str());
        lv_obj_center(message);
        return;
    }
    const auto saved = SsidManager::GetInstance().GetSsidList();
    int counts[2] = {};
    for (const auto& ap : st.scan_results) {
        const bool known = std::any_of(saved.begin(), saved.end(), [&](const SsidItem& item) { return item.ssid == ap.ssid; });
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
    const lv_coord_t available = std::max<lv_coord_t>(0,
        lv_obj_get_content_height(st.ui.list) - 2 * heading_h - footer_h - 2 * kBodyRowGap);
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
        lv_label_set_text_fmt(heading, known ? "已保存（%d）" : "未保存（%d）", counts[group]);
        lv_obj_set_style_text_font(heading, Network_UiFont(), 0);
        lv_obj_set_height(heading, heading_h);
        auto* box = lv_obj_create(section);
        Network_Strip(box);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(box, LV_PCT(100), box_h[group]);
        lv_obj_set_style_border_width(box, box_border, 0);
        lv_obj_set_style_border_color(box, lv_color_black(), 0);
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
            const bool is_saved = std::any_of(saved.begin(), saved.end(),
                [&](const SsidItem& item) { return item.ssid == ap.ssid; });
            if (is_saved != known) continue;
            auto* row = lv_obj_create(rows);
            Network_Strip(row);
            lv_obj_set_size(row, LV_PCT(100), row_h);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_color(row, lv_color_black(), 0);
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
            lv_label_set_text(hint, known ? "附近暂无已保存网络" : "附近暂无未保存网络");
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
    st.ui.scan_btn = Network_MakeBtn(footer, "刷新", kActionBtnW, kActionBtnH, Network_OnScanClicked);
}

int Network_NearbyPageCount() {
    if (Network_State().scan_results.empty()) {
        return 1;
    }
    const int ps = std::max(1, Network_State().nearby_page_size);
    return static_cast<int>((Network_State().scan_results.size() + ps - 1) / ps);
}

int Network_SavedPageCount() {
    const size_t n = SsidManager::GetInstance().GetSsidList().size();
    if (n == 0) {
        return 1;
    }
    const int ps = std::max(1, Network_State().saved_page_size);
    return static_cast<int>((n + ps - 1) / ps);
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
        lv_label_set_text(hint, Lang::Strings::NETWORK_SCANNING);
        lv_obj_set_style_text_font(hint, fontpack_lv_font_ui(), 0);
        // 页码保持占位高度，避免底栏变矮后列表区虚高、末行压住刷新
        if (Network_State().ui.page_lbl != nullptr) {
            lv_label_set_text(Network_State().ui.page_lbl, "1 / 1");
        }
        return;
    }

    if (Network_State().scan_results.empty()) {
        lv_obj_t* hint = lv_label_create(Network_State().ui.list);
        lv_label_set_text(hint, Lang::Strings::NETWORK_NEARBY_EMPTY);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(hint, fontpack_lv_font_ui(), 0);
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
        char foot[24];
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
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
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
        lv_obj_set_style_text_font(left, fontpack_lv_font_ui(), 0);
        lv_obj_clear_flag(left, LV_OBJ_FLAG_CLICKABLE);
        Network_StyleWifiNameRow(row, left);

        char meta[24];
        snprintf(meta, sizeof(meta), "%ddBm", ap.rssi);
        lv_obj_t* right = lv_label_create(row);
        lv_label_set_text(right, meta);
        lv_obj_set_style_text_font(right, fontpack_lv_font_ui(), 0);
        lv_obj_clear_flag(right, LV_OBJ_FLAG_CLICKABLE);
    }
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
        lv_label_set_text(hint, Lang::Strings::NETWORK_SAVED_EMPTY);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(hint, fontpack_lv_font_ui(), 0);
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
        char foot[24];
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
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 6, 0);

        lv_obj_t* name = lv_label_create(row);
        char ttext[96];
        if (i == 0) {
            snprintf(ttext, sizeof(ttext), Lang::Strings::NETWORK_DEFAULT_FMT, item.ssid.c_str());
        } else {
            snprintf(ttext, sizeof(ttext), "%s", item.ssid.c_str());
        }
        lv_label_set_text(name, ttext);
        lv_obj_set_style_text_font(name, fontpack_lv_font_ui(), 0);
        Network_StyleWifiNameRow(row, name);

        if (i != 0) {
            auto* def_ctx = new SavedActionCtx{item.ssid};
            lv_obj_t* def_btn = Network_MakeBtn(row, Lang::Strings::NETWORK_SET_DEFAULT, kSavedDefBtnW, kSavedActionBtnH,
                                        Network_OnSavedSetDefault, def_ctx);
            lv_obj_add_event_cb(def_btn, Network_OnSavedBtnDelete, LV_EVENT_DELETE, def_ctx);
        }

        auto* del_ctx = new SavedActionCtx{item.ssid};
        lv_obj_t* del_btn =
            Network_MakeBtn(row, Lang::Strings::NETWORK_DELETE, kSavedDelBtnW, kSavedActionBtnH, Network_OnSavedRemove,
                            del_ctx);
        lv_obj_add_event_cb(del_btn, Network_OnSavedBtnDelete, LV_EVENT_DELETE, del_ctx);
    }
}

void Network_RebuildListPage() {
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

void Network_OnNearbyTab(lv_event_t* /*e*/) { Network_SwitchTab(Tab::kNearby); }
void Network_OnSavedTab(lv_event_t* /*e*/) { Network_SwitchTab(Tab::kSaved); }
void Network_OnScanClicked(lv_event_t* /*e*/) { Network_ScheduleScan(); }

