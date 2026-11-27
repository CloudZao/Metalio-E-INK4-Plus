#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <esp_timer.h>
#include <wifi_station.h>
#include "ui/apps/settings/network/network_wifi.h"
#include "ui/apps/settings/network/network_keyboard.h"
#include "assets/lang_config.h"
#include "assets/lang_network.h"
#include "assets/lang_screen.h"
#include "business/network/wifi_setup.h"
#include "ui/apps/book/book_text_util.h"
#include "ui/apps/settings/network/network_list.h"
#include "ui/apps/settings/network/network_screen.h"
#include "ui/apps/settings/network/network_screen_priv.h"
#include "ui/common/runtime/page_lifecycle.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/widgets/ui_qrcode.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"

static UiPageLifecycle s_page;
static WifiSetupSessionPtr s_session;
static WifiSetupRequest s_request;
static int64_t s_next_scan_us = 0;
static bool s_scan_pending = false;
static WifiSetupStage s_shown_stage = WifiSetupStage::Queued;
static int s_shown_countdown = -1;
static bool s_suspended = false;

static void ShowConnectionFailure(const char* title, const char* detail) {
    if (Network_ScreenAlive()) {
        Network_State().retry_ssid = Network_State().pending_ssid;
        char buf[192];
        snprintf(buf, sizeof(buf), "%s\n%s", title, detail);
        Network_OpenStatusOverlay(buf);
        Network_State().fail_close_timer = lv_timer_create(
            [](lv_timer_t* timer) {
                Network_State().fail_close_timer = nullptr;
                if (Network_ScreenAlive()) {
                    Network_CloseStatusOverlay();
                }
                lv_timer_delete(timer);
            },
            2500, nullptr);
        lv_timer_set_repeat_count(Network_State().fail_close_timer, 1);
    }
}

static void ShowConnectionSuccess(const std::string& ssid) {
    if (Network_ScreenAlive()) {
        Network_State().retry_ssid.clear();
        Network_ClosePasswordPage();
        if (Network_State().transfer_mode) {
            Network_OpenStatusOverlay(LangString_NETWORK_TRANSFER_READY);
            auto* card = lv_obj_get_parent(Network_State().ui.status_msg);
            lv_obj_set_style_pad_row(card, UiSy(8), 0);
            if (const auto* font = UiFont(UiFontRole::Emphasis)) {
                lv_obj_set_style_text_font(Network_State().ui.status_msg, font, 0);
            }
            auto add_text = [card](const char* text) {
                auto* label = lv_label_create(card);
                lv_obj_set_width(label, LV_PCT(100));
                lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
                lv_obj_set_style_text_font(label, Network_UiFont(), 0);
                lv_obj_set_style_text_color(label, UiColorForeground(), 0);
                lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
                lv_label_set_text(label, text);
                return label;
            };
            auto* network = add_text("");
            lv_obj_set_height(network, Network_UiLineH());
            add_text(LangString_NETWORK_TRANSFER_OPEN_HINT);
            auto* qr = ApQrCreate(card);
            add_text(Network_State().transfer_info.url);
            add_text(LangString_NETWORK_TRANSFER_STOP_HINT);
            Network_MakeBtn(card, LangString_NETWORK_BACK_TO_LIST, kPwdBtnW, kPwdBtnH,
                            [](lv_event_t*) { Network_CloseStatusOverlay(); });
            char network_name[80];
            snprintf(network_name, sizeof(network_name), LangString_NETWORK_CURRENT_SSID_FMT, ssid.c_str());
            Book_SetFilenameMiddle(network, network_name);
            lv_obj_update_layout(card);
            int fixed = 0;
            const auto* frame = lv_obj_get_parent(qr);
            const int count = lv_obj_get_child_count(card);
            for (int i = 0; i < count; ++i) {
                auto* child = lv_obj_get_child(card, i);
                if (child != frame) {
                    fixed += lv_obj_get_height(child);
                }
            }
            fixed += std::max(0, count - 1) * lv_obj_get_style_pad_row(card, LV_PART_MAIN);
            const int side = std::max<int>(
                112, std::min<int>({240, lv_obj_get_content_width(card) - 2 * kUiQrFrameInset,
                                    LV_VER_RES - UiSy(32) - 2 * kOverlayCardPad - fixed - 2 * kUiQrFrameInset}));
            UiResizeQrCode(qr, side);
            ApQrPaint(qr, Network_State().transfer_info.url);
        }
    }
}

static bool EnsureSession() {
    if (WifiSetupIsOpen(s_session)) {
        return true;
    }
    s_session.reset();
    if (!WifiSetupIsClosed()) {
        return false;
    }
    s_session = WifiSetupOpen(Network_State().transfer_mode);
    if (!s_session) {
        s_next_scan_us = esp_timer_get_time() + 15 * 1000 * 1000;
        s_scan_pending = false;
        Network_State().scan_in_progress = false;
        Network_PostScanResult(LangString_NETWORK_SCAN_TASK_FAIL);
        Network_StyleScanBtn(true);
    }
    return s_session != nullptr;
}

static void ApplySetupResult(WifiSetupResult& result) {
    auto& state = Network_State();
    state.scan_in_progress = false;
    state.connect_in_progress = false;
    Network_StyleScanBtn(true);
    if (result.operation == WifiSetupOperation::Scan) {
        if (result.ok) {
            state.scan_results = std::move(result.access_points);
            state.nearby_page = 0;
            char text[64];
            std::snprintf(text, sizeof(text), LangString_NETWORK_SCAN_DONE_FMT, int(state.scan_results.size()));
            Network_PostScanResult(text);
        } else {
            Network_PostScanResult(result.error[0] ? result.error : result.timeout ? LangString_NETWORK_SCAN_TIMEOUT
                                                                                 : LangString_NETWORK_SCAN_FAIL);
        }
        s_next_scan_us = esp_timer_get_time() + 15 * 1000 * 1000;
        Network_RebuildListPage();
        return;
    }
    if (result.operation == WifiSetupOperation::Connect) {
        Network_RebuildListPage();
        if (result.ok) {
            state.transfer_info = result.transfer;
            char text[96];
            std::snprintf(text, sizeof(text), LangString_NETWORK_CONNECTED_FMT, result.ssid.c_str());
            Network_SetStatus(text);
            if (state.transfer_mode) {
                ShowConnectionSuccess(result.ssid);
            }
        } else {
            const char* reason = Network_DisconnectReasonText(result.disconnect_reason);
            char detail[96];
            if (result.error[0]) {
                std::snprintf(detail, sizeof(detail), "%s", result.error);
            } else if (reason) {
                std::snprintf(detail, sizeof(detail), "%s", reason);
            } else if (result.disconnect_reason) {
                std::snprintf(detail, sizeof(detail), LangString_NETWORK_ERR_REASON_FMT, result.disconnect_reason);
            } else {
                std::snprintf(detail, sizeof(detail), "%s", result.timeout ? LangString_NETWORK_CONNECT_TIMEOUT_HINT
                                                                         : LangString_NETWORK_CONNECT_FAIL_REFRESH);
            }
            ShowConnectionFailure(result.timeout ? LangString_NETWORK_CONNECT_TIMEOUT : LangString_NETWORK_CONNECT_FAIL, detail);
        }
        return;
    }
    state.saved_page = 0;
    Network_RebuildListPage();
    Network_SetStatus(!result.ok ? (result.error[0] ? result.error : LangString_SETTINGS_SCREEN_MARGIN_SAVE_FAILED)
                      : result.operation == WifiSetupOperation::SetDefault ? LangString_NETWORK_SET_DEFAULT_OK
                      : result.operation == WifiSetupOperation::Remove ? LangString_NETWORK_DELETED_OK
                                                                      : LangString_NETWORK_CLEARED_OK);
}

static void PollSetup() {
    if (s_suspended) {
        return;
    }
    if (Network_State().settings_list_dirty && !TouchUiFingerIsDown()) {
        Network_RebuildListPage();
    }
    if (s_request && s_request->completed.load(std::memory_order_acquire)) {
        auto result = std::move(s_request);
        ApplySetupResult(*result);
    }
    if (s_request) {
        const auto stage = s_request->stage.load(std::memory_order_acquire);
        if (stage == WifiSetupStage::Restart) {
            Network_State().restart_pending.store(true);
            const int remaining = int(std::max<int64_t>(0, s_request->reboot_at_us.load() - esp_timer_get_time()) / 1000000);
            if (remaining != s_shown_countdown) {
                s_shown_countdown = remaining;
                char headline[96],text[160];
                std::snprintf(headline, sizeof(headline), LangString_NETWORK_CONNECTED_FMT, s_request->ssid.c_str());
                std::snprintf(text, sizeof(text), LangString_SETTINGS_NET_REBOOT_COUNT_FMT, headline, std::max(1, remaining));
                Network_SetStatusOverlayText(remaining > 0 ? text : LangString_SETTINGS_NET_REBOOTING);
            }
        } else if (stage != s_shown_stage) {
            Network_SetStatus(stage == WifiSetupStage::Initializing ? LangString_NETWORK_WIFI_INIT
                              : stage == WifiSetupStage::Scanning ? LangString_NETWORK_SCANNING : "");
        }
        s_shown_stage = stage;
        return;
    }
    if (Network_State().settings_mode && !Network_State().settings_detail &&
        !s_request && !s_scan_pending && esp_timer_get_time() >= s_next_scan_us &&
        WifiStation::GetInstance().IsUserEnabled()) {
        s_scan_pending = true;
    }
    if (s_scan_pending && esp_timer_get_time() >= s_next_scan_us && EnsureSession()) {
        s_scan_pending = false;
        s_request = WifiSetupSubmit(s_session, WifiSetupOperation::Scan);
        if (!s_request) {
            s_next_scan_us = esp_timer_get_time() + 15 * 1000 * 1000;
            Network_State().scan_in_progress = false;
            Network_PostScanResult(LangString_NETWORK_SCAN_TASK_FAIL);
            Network_StyleScanBtn(true);
        }
    }
}

static void CloseSetup(bool resume_station) {
    WifiSetupClose(s_session, resume_station);
    s_session.reset();
    s_request.reset();
    Network_State().scan_in_progress = false;
    Network_State().connect_in_progress = false;
}

void Network_BindWifiSetup(lv_obj_t* root) {
    Network_StopForScreen();
    s_suspended = false;
    s_shown_stage = WifiSetupStage::Queued;
    s_shown_countdown = -1;
    s_scan_pending = false;
    s_next_scan_us = 0;
    s_page.Bind(root, {PollSetup, [](UiPagePowerEvent event) {
        if (event == UiPagePowerEvent::StopTransfers) {
            s_suspended = true;
            CloseSetup(false);
            Network_CloseStatusOverlay();
            s_scan_pending = true;
        } else if (event == UiPagePowerEvent::FinishResume) {
            s_suspended = false;
            s_scan_pending = true;
            s_next_scan_us = 0;
            Network_State().scan_in_progress = true;
        }
    }, [] { Network_OnScreenUnload(nullptr); }, 100}, true);
}

void Network_StopForScreen() {
    s_page.Unbind();
    s_scan_pending = false;
    CloseSetup(true);
    Network_State().restart_pending.store(false);
}

void Network_ScheduleScan() {
    if (!WifiStation::GetInstance().IsUserEnabled()) {
        Network_PostScanResult(LangString_WIFI_DISABLED_RETRY);
        ScreenShowToast(LangString_WIFI_DISABLED_RETRY);
        return;
    }
    if (s_request || s_scan_pending) {
        return;
    }
    auto& state = Network_State();
    if (state.transfer_scan_timer) {
        lv_timer_delete(state.transfer_scan_timer);
        state.transfer_scan_timer = nullptr;
    }
    state.transfer_scan_pending = state.transfer_mode;
    state.transfer_scan_message.clear();
    state.scan_in_progress = true;
    s_scan_pending = true;
    s_next_scan_us = 0;
    Network_StyleScanBtn(false);
    Network_SetStatus(LangString_NETWORK_SCANNING);
    Network_RebuildListPage();
    PollSetup();
}

void Network_ScheduleConnect(const std::string& ssid, const std::string& password) {
    if (!WifiStation::GetInstance().IsUserEnabled()) {
        Network_SetStatus(LangString_WIFI_DISABLED_RETRY);
        ScreenShowToast(LangString_WIFI_DISABLED_RETRY);
        return;
    }
    if (s_request || s_scan_pending) {
        return;
    }
    if (ssid.empty() || ssid.size() > kMaxSsidLen) {
        Network_SetStatus(LangString_NETWORK_SSID_INVALID);
        return;
    }
    if (password.size() > kMaxPasswordLen) {
        Network_SetStatus(LangString_NETWORK_PWD_TOO_LONG);
        return;
    }
    if (!EnsureSession()) {
        Network_SetStatus(LangString_NETWORK_CONNECT_TASK_FAIL);
        return;
    }
    s_request = WifiSetupSubmit(s_session, WifiSetupOperation::Connect, ssid, password);
    if (!s_request) {
        Network_SetStatus(LangString_NETWORK_CONNECT_TASK_FAIL);
        return;
    }
    s_shown_countdown = -1;
    Network_State().pending_ssid = ssid;
    Network_State().connect_in_progress = true;
    char text[96];
    std::snprintf(text, sizeof(text), LangString_NETWORK_CONNECTING_FMT, ssid.c_str());
    Network_OpenStatusOverlay(text);
}

void Network_ScheduleSavedChange(WifiSetupOperation operation, const std::string& ssid) {
    if (s_request || s_scan_pending || !EnsureSession()) {
        return;
    }
    s_request = WifiSetupSubmit(s_session, operation, ssid);
    if (!s_request) {
        Network_SetStatus(LangString_SETTINGS_SCREEN_MARGIN_SAVE_FAILED);
    }
}
