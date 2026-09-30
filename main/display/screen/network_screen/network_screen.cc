// network_screen.cc — Create / VK / lifecycle / overlays
#include "network_screen/network_screen.h"
#include "network_screen_priv.h"
#include "network_screen_ui.h"
#include "network_keyboard.h"
#include "network_wifi.h"
#include "network_list.h"
#include "network_screen/network_screen_priv.h"

#include "application.h"
#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "vk_key_handler.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include <esp_log.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

NetworkUiState& Network_State() {
    static NetworkUiState s;
    return s;
}

void Network_CloseStatusOverlay() {
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
    lv_obj_t* mask = lv_obj_create(Network_State().ui.screen);
    Network_Strip(mask);
    lv_obj_set_size(mask, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(mask, 0, 0);
    ScreenApplyDotBackdrop(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    Network_State().ui.status_overlay = mask;

    lv_obj_t* card = lv_obj_create(mask);
    Network_Strip(card);
    lv_obj_set_size(card, LV_HOR_RES - kOverlayCardMargin, kOverlayCardH);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, kOverlayBorderW, 0);
    lv_obj_set_style_radius(card, kOverlayCardRadius, 0);
    lv_obj_set_style_pad_all(card, kOverlayCardPad, 0);

    lv_obj_t* lbl = lv_label_create(card);
    Network_State().ui.status_msg = lbl;
    lv_label_set_text(lbl, msg != nullptr ? msg : "");
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);
}

void Network_SetStatusOverlayText(const char* msg) {
    if (Network_State().ui.status_msg != nullptr) {
        lv_label_set_text(Network_State().ui.status_msg, msg != nullptr ? msg : "");
    }
}

void Network_RebootTask(void* /*arg*/) {
    vTaskDelay(pdMS_TO_TICKS(200));
    Application::GetInstance().Reboot();
    vTaskDelete(nullptr);
}

void Network_RestartTimerCb(lv_timer_t* /*timer*/) {
    Network_State().restart_remaining--;
    if (Network_State().restart_remaining > 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), Lang::Strings::SETTINGS_NET_REBOOT_COUNT_FMT, Network_State().restart_headline.c_str(),
                 Network_State().restart_remaining);
        Network_SetStatusOverlayText(buf);
        return;
    }
    if (Network_State().restart_timer != nullptr) {
        lv_timer_delete(Network_State().restart_timer);
        Network_State().restart_timer = nullptr;
    }
    Network_SetStatusOverlayText(Lang::Strings::SETTINGS_NET_REBOOTING);
    xTaskCreate(Network_RebootTask, "net_reboot", 2048, nullptr, 5, nullptr);
}

void Network_OpenRestartCountdown(const std::string& headline) {
    Network_State().restart_headline = headline;
    Network_State().restart_remaining = kRestartCountdownSec;
    char buf[128];
    snprintf(buf, sizeof(buf), Lang::Strings::SETTINGS_NET_REBOOT_COUNT_FMT, Network_State().restart_headline.c_str(),
             Network_State().restart_remaining);
    Network_OpenStatusOverlay(buf);
    if (Network_State().restart_timer != nullptr) {
        lv_timer_delete(Network_State().restart_timer);
    }
    Network_State().restart_timer = lv_timer_create(Network_RestartTimerCb, 1000, nullptr);
}

void Network_OnScreenUnload(lv_event_t* /*e*/) {
    ESP_LOGI(TAG, "unload");
    Network_State().screen_active = false;
    if (Network_State().restart_timer != nullptr) {
        lv_timer_delete(Network_State().restart_timer);
        Network_State().restart_timer = nullptr;
    }
    if (Network_State().fail_close_timer != nullptr) {
        lv_timer_delete(Network_State().fail_close_timer);
        Network_State().fail_close_timer = nullptr;
    }
    Network_ClosePasswordPage();
    Network_CloseStatusOverlay();
    Network_WifiTeardownForScreen();
    Network_ReleaseUiKeepNet();
    for (int i = 0; i < kKbRows * kKbCols; ++i) {
        Network_State().kb_keys[i] = nullptr;
    }
    Network_State().ui = {};
    Network_State().scan_results.clear();
    Network_State().scan_in_progress = false;
    Network_State().connect_in_progress = false;
}

bool Network_ScreenAlive() {
    return Network_State().screen_active && Network_State().ui.screen != nullptr && lv_obj_is_valid(Network_State().ui.screen);
}

const lv_font_t* Network_UiFont() {
    const lv_font_t* f = fontpack_lv_font_ui();
    return f;
}

// 键盘用系统字库 30@2，失败回退 UI 默认字
const lv_font_t* Network_KbFont() {
    const lv_font_t* f = fontpack_lv_font_get(30, 2);
    return f != nullptr ? f : Network_UiFont();
}

bool Network_IsPasswordPageOpen() {
    return Network_State().ui.pwd_page != nullptr && !lv_obj_has_flag(Network_State().ui.pwd_page, LV_OBJ_FLAG_HIDDEN);
}

const char* Network_AuthLabel(wifi_auth_mode_t mode) {
    return (mode == WIFI_AUTH_OPEN) ? Lang::Strings::NETWORK_AUTH_OPEN : Lang::Strings::NETWORK_AUTH_SECURE;
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
            return Lang::Strings::NETWORK_ERR_BAD_PASSWORD;
        case WIFI_REASON_NO_AP_FOUND:
            return Lang::Strings::NETWORK_ERR_AP_GONE;
        case WIFI_REASON_ASSOC_LEAVE:
        case WIFI_REASON_ASSOC_TOOMANY:
        case WIFI_REASON_ASSOC_FAIL:
        case WIFI_REASON_ASSOC_NOT_AUTHED:
            return Lang::Strings::NETWORK_ERR_ASSOC;
        case WIFI_REASON_BEACON_TIMEOUT:
            return Lang::Strings::NETWORK_ERR_WEAK;
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
    const lv_font_t* font = fontpack_lv_font_ui();
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
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, kBtnBorderW, 0);
    lv_obj_set_style_radius(btn, kBtnRadius, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
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
        lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        if (lbl != nullptr) {
            lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        }
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        if (lbl != nullptr) {
            lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
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
    // 挡 AppIdle（不跑保网断网）；待机计时仍走（Evaluate 先判待机再判 SoftKeepNet）
    Network_AcquireUiKeepNet();

    lv_obj_t* scr = lv_obj_create(nullptr);
    Network_State().ui.screen = scr;
    Network_Strip(scr);
    lv_obj_set_size(scr, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, Network_OnScreenUnload, LV_EVENT_SCREEN_UNLOADED, nullptr);

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
    lv_label_set_text(title, Lang::Strings::NETWORK_TITLE);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);

    Network_State().ui.status_lbl = lv_label_create(body);
    lv_label_set_text(Network_State().ui.status_lbl, "");
    lv_obj_set_width(Network_State().ui.status_lbl, LV_PCT(100));
    lv_label_set_long_mode(Network_State().ui.status_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(Network_State().ui.status_lbl, fontpack_lv_font_ui(), 0);

    lv_obj_t* tab_row = lv_obj_create(body);
    Network_Strip(tab_row);
    lv_obj_set_width(tab_row, LV_PCT(100));
    lv_obj_set_height(tab_row, kTabRowH);
    lv_obj_set_flex_flow(tab_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tab_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tab_row, kTabGap, 0);

    Network_State().ui.nearby_tab_btn =
        Network_MakeBtn(tab_row, Lang::Strings::NETWORK_TAB_NEARBY, kTabBtnW, kTabBtnH, Network_OnNearbyTab);
    Network_State().ui.saved_tab_btn =
        Network_MakeBtn(tab_row, Lang::Strings::NETWORK_TAB_SAVED, kTabBtnW, kTabBtnH, Network_OnSavedTab);
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
        Network_MakeBtn(action_row, Lang::Strings::NETWORK_SCAN, kActionBtnW, kActionBtnH, Network_OnScanClicked);
    Network_State().ui.clear_btn =
        Network_MakeBtn(action_row, Lang::Strings::NETWORK_CLEAR_ALL, kActionBtnW, kActionBtnH, Network_OnClearSaved);
    lv_obj_add_flag(Network_State().ui.clear_btn, LV_OBJ_FLAG_HIDDEN);

    Network_State().ui.page_lbl = lv_label_create(foot);
    // 先占位再量列表高：空字符串会让底栏偏矮，多算出一条压住刷新按钮
    lv_label_set_text(Network_State().ui.page_lbl, "1 / 1");
    lv_obj_set_width(Network_State().ui.page_lbl, LV_PCT(100));
    lv_obj_set_style_text_align(Network_State().ui.page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(Network_State().ui.page_lbl, fontpack_lv_font_ui(), 0);

    Network_BuildPasswordPage(scr, status.height);

    Network_RefreshPageSizes();
    Network_RebuildListPage();

    ScreenSetIsHome(false);
    VkKey_AttachScreen(scr, kScreenId, VkKeyScreenDesc{NetworkScreen::Create, NetworkScreen::OnVkKey});

    // 进入后自动扫一次
    Network_ScheduleScan();
    return scr;
}

bool NetworkScreen::OnVkKey(const char* key_name) {
    if (key_name == nullptr || !Network_ScreenAlive()) {
        return false;
    }
    // 盖板键在 touch_feed 任务回调，禁止同步 lv_obj_clean / delete（会与 LVGL 线程抢 event 链崩溃）
    if (Network_State().ui.status_overlay != nullptr) {
        if (std::strcmp(key_name, "vk_prev") == 0 || std::strcmp(key_name, "vk_home") == 0) {
            if (Network_State().restart_timer != nullptr) {
                return true;  // 倒计时重启中禁止退出
            }
            ScreenLvAsync([](void*) {
                if (Network_ScreenAlive()) {
                    Network_CloseStatusOverlay();
                }
            });
            return true;
        }
        return true;
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
