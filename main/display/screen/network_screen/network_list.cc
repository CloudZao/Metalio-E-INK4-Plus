// network_list.cc — split from parent .cc
#include "network_screen/network_screen.h"
#include "network_screen_priv.h"
#include "network_list.h"
#include "network_screen/network_screen_priv.h"

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
    if (Network_State().ui.status_lbl != nullptr) {
        lv_label_set_text(Network_State().ui.status_lbl, text != nullptr ? text : "");
    }
}

void Network_AsyncSetStatus(void* p) {
    auto* msg = static_cast<AsyncStatusMsg*>(p);
    if (Network_ScreenAlive()) {
        Network_SetStatus(msg->text);
    }
    delete msg;
}

void Network_PostStatus(const char* text) {
    if (!Network_State().screen_active) {
        return;
    }
    auto* msg = new AsyncStatusMsg{};
    snprintf(msg->text, sizeof(msg->text), "%s", text != nullptr ? text : "");
    if (lv_async_call(Network_AsyncSetStatus, msg) != LV_RESULT_OK) {
        delete msg;
    }
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
    // 扫网任务常在点击后立刻跑，此时 LVGL 仍持锁（墨水局刷可远超 50ms）。
    // 勿走 ScreenLvAsyncUrgent(lock 50ms)：会打 esp_lv_adapter 的 ERROR，且触控后几乎必失败。
    // ScreenLvAsync 经 MainEventLoop 再无限等锁排队，与翻页/扫完刷新路径一致。
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
    lv_async_call(Network_AsyncStyleScan, reinterpret_cast<void*>(static_cast<intptr_t>(enabled ? 1 : 0)));
}

void Network_OnNearbyItemClicked(lv_event_t* e) {
    auto* ctx = static_cast<NearbyClickCtx*>(lv_event_get_user_data(e));
    if (ctx == nullptr) {
        return;
    }
    const auto auth = static_cast<wifi_auth_mode_t>(ctx->authmode);
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
    SsidManager::GetInstance().SetDefaultSsid(ctx->index);
    Network_State().saved_page = 0;
    Network_RebuildListPage();
    Network_SetStatus(Lang::Strings::NETWORK_SET_DEFAULT_OK);
}

void Network_OnSavedRemove(lv_event_t* e) {
    auto* ctx = static_cast<SavedActionCtx*>(lv_event_get_user_data(e));
    if (ctx == nullptr) {
        return;
    }
    SsidManager::GetInstance().RemoveSsid(ctx->index);
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

    const auto& list = SsidManager::GetInstance().GetSsidList();
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

    const int pages = Network_SavedPageCount();
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

    const int ps = std::max(1, Network_State().saved_page_size);
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
            auto* def_ctx = new SavedActionCtx{i};
            lv_obj_t* def_btn = Network_MakeBtn(row, Lang::Strings::NETWORK_SET_DEFAULT, kSavedDefBtnW, kSavedActionBtnH,
                                        Network_OnSavedSetDefault, def_ctx);
            lv_obj_add_event_cb(def_btn, Network_OnSavedBtnDelete, LV_EVENT_DELETE, def_ctx);
        }

        auto* del_ctx = new SavedActionCtx{i};
        lv_obj_t* del_btn =
            Network_MakeBtn(row, Lang::Strings::NETWORK_DELETE, kSavedDelBtnW, kSavedActionBtnH, Network_OnSavedRemove,
                            del_ctx);
        lv_obj_add_event_cb(del_btn, Network_OnSavedBtnDelete, LV_EVENT_DELETE, del_ctx);
    }
}

void Network_RebuildListPage() {
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

