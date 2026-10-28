#include "assets/lang_network.h"
#include "cloud_screen/cloud_chrome.h"
#include "cloud_screen/cloud_screen_priv.h"
#include "cloud_screen/cloud_fetch.h"
#include "cloud_screen/cloud_list.h"
#include "cloud_screen/cloud_ui_helpers.h"
#include "cloud_screen/preview/cloud_wallpaper_preview.h"
#include "cloud_screen/ap/ap_qr.h"
#include "cloud_screen/ap/ap_transfer.h"

#include <lvgl.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <esp_heap_caps.h>

#include "wifi_station.h"
#include "network_screen/network_screen.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "dual_network_board.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "settings_storage_tab.h"
#include "screen_common.h"

void ScheduleUserCloudSync() {
    auto& st = Cloud_State();
    if (st.preview.open) {
        return;
    }
    if (st.xfer.download_busy || SyncBusy() || s_net_prep_task != nullptr) {
        return;
    }
    if (Board::GetInstance().IsWifiConfigMode()) {
        st.data.error = Lang::Strings::CLOUD_NEED_WIFI_CFG;
        RenderListPage();
        return;
    }
    st.data.fetch_done = false;
    ScheduleNetPrepAndFetch();
}

void ShowObj(lv_obj_t* obj, bool on) {
    if (obj == nullptr) {
        return;
    }
    if (on) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void ShowAppBody(bool on) {
    auto& st = Cloud_State();
    ShowObj(st.chrome.header_row, on);
    ShowObj(st.chrome.filter_row, on);
    ShowObj(st.chrome.list_body, on);
    ShowObj(st.chrome.footer, on);
    ShowObj(st.chrome.page_lbl, on);
}

void PaintApPanel(const ApTransferInfo& info) {
    auto& st = Cloud_State();
    if (st.chrome.ap_ssid_lbl != nullptr) {
        char line[64];
        std::snprintf(line, sizeof(line), Lang::Strings::CLOUD_AP_SSID_FMT, info.ssid);
        lv_label_set_text(st.chrome.ap_ssid_lbl, line);
    }
    if (st.chrome.ap_url_lbl != nullptr) {
        lv_label_set_text(st.chrome.ap_url_lbl, info.url);
    }
    if (st.chrome.ap_status_lbl != nullptr) {
        ShowObj(st.chrome.ap_status_lbl, false);
    }
    char wifi_qr[64];
    std::snprintf(wifi_qr, sizeof(wifi_qr), "WIFI:T:nopass;S:%s;;", info.ssid);
    ApQrPaint(st.chrome.ap_qr_wifi, wifi_qr);
    ApQrPaint(st.chrome.ap_qr_url, info.url);
}

void LeaveUsbMode() {
    SettingsStorageTab_Reset();
    auto& st = Cloud_State();
    if (st.chrome.usb_panel != nullptr) {
        lv_obj_clean(st.chrome.usb_panel);
    }
}

namespace {

constexpr uint32_t kRadioStack = 8192;
constexpr int kRadioClosed = -1;
constexpr int kRadioStandby = -2;
#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
constexpr uint32_t kRadioMemoryCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
constexpr uint32_t kRadioMemoryCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif

std::atomic<uint32_t> s_radio_gen{0};
std::atomic<int> s_radio_mode{kRadioClosed};
bool s_radio_stop_pending = false; // 受工作锁保护，快速重进也必须先完成旧服务清理
TaskHandle_t s_radio_task = nullptr;
std::mutex s_radio_work_mu;
StaticSemaphore_t s_radio_ui_storage;
SemaphoreHandle_t s_radio_ui_mu = nullptr;
bool s_transfer_power_held = false;
bool s_activation_held = false;

void HoldTransferPower() {
    if (s_transfer_power_held) {
        return;
    }
    PowerPolicy::GetInstance().Acquire(PowerNeed::StandbyInhibit);
    PowerPolicy::GetInstance().Acquire(PowerNeed::UiKeepNet);
    s_transfer_power_held = true;
}

void ReleaseTransferPower() {
    if (!s_transfer_power_held) {
        return;
    }
    PowerPolicy::GetInstance().Release(PowerNeed::UiKeepNet);
    PowerPolicy::GetInstance().Release(PowerNeed::StandbyInhibit);
    s_transfer_power_held = false;
}

void HoldActivationPause() {
    if (s_activation_held) {
        return;
    }
    Application::GetInstance().PauseActivationPolling();
    s_activation_held = true;
}

void ReleaseActivationPause() {
    if (!s_activation_held) {
        return;
    }
    Application::GetInstance().ResumeActivationPolling();
    s_activation_held = false;
}

struct RadioResult {
    uint32_t gen = 0;
    int mode = -1;
    bool ok = false;
    const char* fail = nullptr;
    ApTransferInfo info{};
};

RadioResult s_radio_result;

void SetRadioStatus(const char* text) {
    if (text == Lang::Strings::WIFI_DISABLED_RETRY) {
        ScreenShowToast(text);
    }
    auto& st = Cloud_State();
    if (st.chrome.radio_status_lbl == nullptr) {
        return;
    }
    const bool on = text != nullptr && text[0] != '\0';
    if (on) {
        lv_label_set_text(st.chrome.radio_status_lbl, text);
        lv_obj_align(st.chrome.radio_status_lbl, LV_ALIGN_CENTER, 0, 0);
        lv_obj_move_foreground(st.chrome.radio_status_lbl);
    }
    ShowObj(st.chrome.radio_status_lbl, on);
}

void ApplyRadioResult(void* /*user*/) {
    if (s_radio_ui_mu == nullptr) {
        return;
    }
    RadioResult local;
    xSemaphoreTake(s_radio_ui_mu, portMAX_DELAY);
    local = s_radio_result;
    xSemaphoreGive(s_radio_ui_mu);
    if (local.gen != s_radio_gen.load() || !ScreenAlive()) {
        return;
    }
    auto& st = Cloud_State();
    if (st.data.mode != local.mode) {
        return;
    }
    if (local.mode == kCloudModeAp) {
        if (!local.ok) {
            ShowObj(st.chrome.ap_panel, false);
            const char* fail = local.fail != nullptr ? local.fail : Lang::Strings::CLOUD_AP_FAIL;
            SetRadioStatus(fail);
            return;
        }
        PaintApPanel(local.info);
        ShowObj(st.chrome.ap_panel, true);
        SetRadioStatus(nullptr);
        return;
    }
    if (local.mode == kCloudModeWifi) {
        SetRadioStatus(nullptr);
        ShowObj(st.chrome.wifi_panel, true);
        NetworkScreen::BuildTransfer(st.chrome.wifi_panel);
        return;
    }
    if (local.mode != kCloudModeApp) {
        return;
    }
    if (!local.ok) {
        ShowAppBody(false);
        const char* fail = local.fail != nullptr ? local.fail : Lang::Strings::CLOUD_NET_NOT_READY;
        SetRadioStatus(fail);
        return;
    }
    SetRadioStatus(nullptr);
    ShowAppBody(true);
    if (!st.data.fetch_done && !st.data.loading && !st.data.waiting_net) {
        ScheduleUserCloudSync();
    } else {
        RenderListPage();
    }
}

void PostRadioResult(uint32_t gen, int mode, bool ok, const char* fail, const ApTransferInfo* info) {
    if (gen != s_radio_gen.load() || s_radio_ui_mu == nullptr) {
        return;
    }
    xSemaphoreTake(s_radio_ui_mu, portMAX_DELAY);
    s_radio_result.gen = gen;
    s_radio_result.mode = mode;
    s_radio_result.ok = ok;
    s_radio_result.fail = fail;
    s_radio_result.info = info != nullptr ? *info : ApTransferInfo{};
    xSemaphoreGive(s_radio_ui_mu);
    if (!ScreenLvAsync(ApplyRadioResult, nullptr)) {
        ESP_LOGW(TAG, "radio ui post failed");
    }
}

void RadioTask(void* /*arg*/) {
    for (;;) {
        uint32_t gen;
        int mode;
        bool stop_pending;
        {
            std::lock_guard<std::mutex> lock(s_radio_work_mu);
            gen = s_radio_gen.load();
            mode = s_radio_mode.load();
            stop_pending = s_radio_stop_pending;
            s_radio_stop_pending = false;
        }
        if (stop_pending) {
            ApTransferStop(mode != kCloudModeUsb && mode != kRadioStandby);
        }
        if (s_radio_gen.load() != gen) continue;
        if (mode == kCloudModeAp) {
            ApTransferInfo info{};
            const bool ok = ApTransferStart(&info);
            PostRadioResult(gen, mode, ok, info.fail, &info);
        } else if (mode == kCloudModeWifi) {
            if (!stop_pending) ApTransferStop(true);
            PostRadioResult(gen, mode, true, nullptr, nullptr);
        } else if (mode == kCloudModeApp) {
            if (!stop_pending) ApTransferStop(true);
            // 云同步复用 NetPrepTask 的通用联网流程，不依赖 STA 状态。
            PostRadioResult(gen, mode, true, nullptr, nullptr);
        } else if (mode == kCloudModeUsb && !stop_pending) {
            ApTransferStop(false);
        }
        bool retire;
        {
            std::lock_guard<std::mutex> lock(s_radio_work_mu);
            retire = s_radio_gen.load() == gen && mode < 0;
            if (retire) s_radio_task = nullptr;
        }
        if (retire) {
            vTaskDeleteWithCaps(nullptr);
            return;
        }
        // 页面拥有任务期间保留通知等待，退出无需再分配清理任务。
        while (s_radio_gen.load() == gen) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
    }
}

void KickRadio(int mode) {
    if ((mode == kCloudModeAp || mode == kCloudModeWifi) && !WifiStation::GetInstance().IsUserEnabled()) {
        SetRadioStatus(Lang::Strings::WIFI_DISABLED_RETRY);
        return;
    }
    std::lock_guard<std::mutex> lock(s_radio_work_mu);
    if (s_radio_ui_mu == nullptr) {
        s_radio_ui_mu = xSemaphoreCreateMutexStatic(&s_radio_ui_storage);
    }
    s_radio_mode.store(mode);
    s_radio_gen.fetch_add(1);
    if (mode < 0) {
        ApTransferRequestStop(mode == kRadioStandby);
        s_radio_stop_pending = s_radio_task != nullptr || s_radio_stop_pending;
    }
    if (s_radio_task != nullptr) {
        xTaskNotifyGive(s_radio_task);
    } else if (mode >= 0) {
        if (xTaskCreatePinnedToCoreWithCaps(RadioTask, "cloud_radio", kRadioStack, nullptr,
                                           tskIDLE_PRIORITY + 2, &s_radio_task, 0,
                                           kRadioMemoryCaps) != pdPASS) {
            s_radio_task = nullptr;
            ESP_LOGW(TAG, "cloud_radio create failed");
            SetRadioStatus(Lang::Strings::CLOUD_TASK_FAIL);
        }
    }
}

}  // namespace

void CloudRadioAbandon() {
    KickRadio(kRadioClosed);
    ReleaseTransferPower();
    ReleaseActivationPause();
}

void ParkApForStandby() {
    auto& st = Cloud_State();
    if (st.chrome.screen == nullptr || st.data.mode != kCloudModeAp) {
        return;
    }
    // 待机先阻止旧启动继续接收请求，关闭和任务回收仍由射频任务完成。
    KickRadio(kRadioStandby);
}

void ReloadApAfterStandby() {
    auto& st = Cloud_State();
    if (st.chrome.screen == nullptr || st.data.mode != kCloudModeAp) {
        ApTransferAllowStart();
        return;
    }
    ApTransferAllowStart();
    EnterCloudMode(kCloudModeAp);
}

void EnterCloudMode(int mode) {
    auto& st = Cloud_State();
    const int prev = st.data.mode;
    if (prev == kCloudModeWifi) NetworkScreen::CloseTransfer();
    if (prev == kCloudModeUsb && mode != kCloudModeUsb) {
        LeaveUsbMode();
    }
    st.data.mode = mode;
    ShowAppBody(false);
    ShowObj(st.chrome.ap_panel, false);
    ShowObj(st.chrome.wifi_panel, false);
    ShowObj(st.chrome.usb_panel, mode == kCloudModeUsb);
    RefreshTabUi();
    auto* dual = dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
    if ((mode == kCloudModeAp || mode == kCloudModeWifi) &&
        dual != nullptr && dual->GetNetworkType() == NetworkType::CELLULAR) {
        CloudRadioAbandon();
        SetRadioStatus(Lang::Strings::CLOUD_CELLULAR_NEED_WIFI);
        return;
    }
    if (mode == kCloudModeApp) {
        ReleaseActivationPause();
    } else {
        HoldActivationPause();
    }
    if (mode == kCloudModeAp || mode == kCloudModeWifi) {
        HoldTransferPower();
        SetRadioStatus(mode == kCloudModeAp ? Lang::Strings::CLOUD_AP_OPENING : "正在准备WIFI传输…");
        KickRadio(mode);
        return;
    }
    ReleaseTransferPower();
    if (mode == kCloudModeUsb) {
        SetRadioStatus(nullptr);
        if (st.chrome.usb_panel != nullptr && prev != kCloudModeUsb) {
            lv_obj_clean(st.chrome.usb_panel);
            SettingsStorageTab_Build(st.chrome.usb_panel);
        }
        KickRadio(mode);
        return;
    }
    SetRadioStatus(Lang::Strings::CLOUD_CONNECTING_NET);
    KickRadio(mode);
}

void OnTabClicked(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto& st = Cloud_State();
    // Tab 仅本地筛选；下载进度页打开时不可切
    if (st.xfer.dl_page_open || st.preview.open || st.dialogs.dialog_mask != nullptr ||
        st.dialogs.action_mask != nullptr) {
        return;
    }
    const int tab = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (tab < 0 || tab >= kTabCount || tab == st.data.mode) {
        return;
    }
    EnterCloudMode(tab);
}

void OnFilterClicked(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto& st = Cloud_State();
    if (st.data.mode != kCloudModeApp || st.xfer.dl_page_open || st.preview.open ||
        st.dialogs.dialog_mask != nullptr || st.dialogs.action_mask != nullptr) {
        return;
    }
    const int tab = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (tab < 0 || tab >= kFilterTabCount || tab == st.data.filter_tab) {
        return;
    }
    st.data.filter_tab = tab;
    st.data.page = 0;
    RefreshFilterUi();
    RefreshSectionTitle();
    RenderListPage();
}

lv_obj_t* MakeTabBtn(lv_obj_t* parent, const char* text, int tab) {
    auto& st = Cloud_State();
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, lv_pct(100));
    lv_obj_set_style_pad_all(btn, 0, 0);
    Cloud_DisableScroll(btn);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, OnTabClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(tab)));
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, Cloud_ItemFont(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    st.chrome.tab_btns[tab] = btn;
    st.chrome.tab_lbls[tab] = lbl;
    return btn;
}

void OnSyncClicked(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto& st = Cloud_State();
    if (st.preview.open || st.data.multi) {
        return;
    }
    if (st.xfer.download_busy || st.data.batch_busy || st.data.delete_busy || SyncBusy() ||
        s_net_prep_task != nullptr) {
        SetStatusTip(Lang::Strings::CLOUD_PLEASE_WAIT, true);
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (s_last_user_sync_us != 0 && (now - s_last_user_sync_us) < kSyncThrottleUs) {
        SetStatusTip(Lang::Strings::CLOUD_PLEASE_WAIT, true);
        return;
    }
    s_last_user_sync_us = now;
    ScheduleUserCloudSync();
}

void BuildCloudMainChrome(lv_obj_t* scr, lv_coord_t status_height) {
    auto& st = Cloud_State();
    // 为贴底页码留出 kFooterOutsideH，列表区可多挤一行
    const lv_coord_t body_h = LV_VER_RES - status_height - kFooterOutsideH;

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, body_h);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status_height);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(body, kPad, 0);
    lv_obj_set_style_pad_bottom(body, UiSy(4), 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(body, kRowGap, 0);
    Cloud_DisableScroll(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);
    st.chrome.main_body = body;

    lv_obj_t* tab_row = lv_obj_create(body);
    lv_obj_remove_style_all(tab_row);
    lv_obj_set_width(tab_row, Cloud_ContentWidth());
    lv_obj_set_height(tab_row, kTabH);
    lv_obj_set_style_bg_color(tab_row, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(tab_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(tab_row, lv_color_black(), 0);
    lv_obj_set_style_border_width(tab_row, kTabBorderW, 0);
    lv_obj_set_style_radius(tab_row, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(tab_row, true, 0);
    lv_obj_set_flex_flow(tab_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tab_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(tab_row, kTabInset, 0);
    lv_obj_set_style_pad_column(tab_row, 0, 0);
    Cloud_DisableScroll(tab_row);
    lv_obj_clear_flag(tab_row, LV_OBJ_FLAG_CLICKABLE);
    MakeTabBtn(tab_row, Lang::Strings::CLOUD_TAB_APP, kCloudModeApp);
    MakeTabBtn(tab_row, Lang::Strings::CLOUD_TAB_AP, kCloudModeAp);
    MakeTabBtn(tab_row, "WIFI传输", kCloudModeWifi);
    MakeTabBtn(tab_row, Lang::Strings::CLOUD_TAB_USB, kCloudModeUsb);
    RefreshTabUi();

    lv_obj_t* header_row = lv_obj_create(body);
    st.chrome.header_row = header_row;
    lv_obj_remove_style_all(header_row);
    lv_obj_set_width(header_row, Cloud_ContentWidth());
    lv_obj_set_height(header_row, kHeaderH);
    lv_obj_set_style_bg_opa(header_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(header_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header_row, kHeaderGap, 0);
    Cloud_DisableScroll(header_row);
    lv_obj_clear_flag(header_row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* star_wrap = lv_obj_create(header_row);
    lv_obj_remove_style_all(star_wrap);
    lv_obj_set_size(star_wrap, kHeaderIcon, kHeaderIcon);
    lv_obj_set_style_bg_color(star_wrap, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(star_wrap, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(star_wrap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(star_wrap, 0, 0);
    Cloud_DisableScroll(star_wrap);
    lv_obj_clear_flag(star_wrap, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* star = lv_label_create(star_wrap);
    lv_label_set_text(star, "\xE2\x98\x85");  // ★
    lv_obj_set_style_text_color(star, lv_color_white(), 0);
    lv_obj_set_style_text_font(star, Cloud_ItemFont(), 0);
    lv_obj_center(star);
    lv_obj_clear_flag(star, LV_OBJ_FLAG_CLICKABLE);

    st.chrome.section_title = lv_label_create(header_row);
    lv_label_set_text(st.chrome.section_title, Lang::Strings::CLOUD_PENDING);
    lv_obj_set_style_text_font(st.chrome.section_title, Cloud_UiFont(), 0);
    lv_obj_set_style_text_color(st.chrome.section_title, lv_color_black(), 0);
    lv_obj_clear_flag(st.chrome.section_title, LV_OBJ_FLAG_CLICKABLE);

    st.chrome.status_lbl = lv_label_create(header_row);
    lv_label_set_text(st.chrome.status_lbl, "");
    lv_obj_set_style_pad_left(st.chrome.status_lbl, kStatusGap, 0);
    lv_obj_set_style_text_font(st.chrome.status_lbl, Cloud_ItemFont(), 0);
    lv_obj_set_style_text_color(st.chrome.status_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(st.chrome.status_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(st.chrome.status_lbl, LV_OBJ_FLAG_HIDDEN);

    // 传输内容共用细外框；App 列表的页码贴底放在框外
    lv_obj_t* list_frame = lv_obj_create(body);
    st.chrome.list_frame = list_frame;
    lv_obj_remove_style_all(list_frame);
    lv_obj_set_width(list_frame, Cloud_ContentWidth());
    lv_obj_set_flex_grow(list_frame, 1);
    lv_obj_set_style_bg_color(list_frame, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(list_frame, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(list_frame, kListFrameBorderW, 0);
    lv_obj_set_style_border_color(list_frame, lv_color_black(), 0);
    lv_obj_set_style_radius(list_frame, kListFrameRadius, 0);
    lv_obj_set_style_pad_all(list_frame, kListFramePad, 0);
    lv_obj_set_flex_flow(list_frame, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_frame, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list_frame, 0, 0);
    Cloud_DisableScroll(list_frame);
    lv_obj_clear_flag(list_frame, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* filter_row = lv_obj_create(list_frame);
    st.chrome.filter_row = filter_row;
    lv_obj_remove_style_all(filter_row);
    lv_obj_set_width(filter_row, lv_pct(100));
    lv_obj_set_height(filter_row, kFilterH);
    lv_obj_set_style_border_side(filter_row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(filter_row, kSyncDividerW, 0);
    lv_obj_set_style_border_color(filter_row, lv_color_black(), 0);
    lv_obj_set_flex_flow(filter_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(filter_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    Cloud_DisableScroll(filter_row);
    lv_obj_clear_flag(filter_row, LV_OBJ_FLAG_CLICKABLE);
    const char* filter_names[kFilterTabCount] = {
        Lang::Strings::CLOUD_TAB_ALL,
        Lang::Strings::CLOUD_TAB_WALLPAPER,
        Lang::Strings::CLOUD_TAB_BOOK,
        Lang::Strings::CLOUD_TAB_FONT,
    };
    for (int i = 0; i < kFilterTabCount; ++i) {
        lv_obj_t* btn = lv_obj_create(filter_row);
        lv_obj_remove_style_all(btn);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, lv_pct(100));
        Cloud_DisableScroll(btn);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(btn);
        lv_obj_add_event_cb(btn, OnFilterClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, filter_names[i]);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_center(lbl);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        st.chrome.filter_btns[i] = btn;
        st.chrome.filter_lbls[i] = lbl;
    }
    RefreshFilterUi();

    lv_obj_t* list_body = lv_obj_create(list_frame);
    lv_obj_remove_style_all(list_body);
    lv_obj_set_width(list_body, lv_pct(100));
    lv_obj_set_style_pad_top(list_body, 2, 0);
    lv_obj_set_flex_grow(list_body, 1);
    Cloud_DisableScroll(list_body);
    st.chrome.list_body = list_body;

    lv_obj_t* footer = lv_obj_create(list_frame);
    lv_obj_remove_style_all(footer);
    lv_obj_set_width(footer, lv_pct(100));
    lv_obj_set_height(footer, kFooterBlockH);
    lv_obj_set_style_bg_opa(footer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(footer, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(footer, kSyncDividerW, 0);
    lv_obj_set_style_border_color(footer, lv_color_black(), 0);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    Cloud_DisableScroll(footer);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_CLICKABLE);
    st.chrome.footer = footer;

    // 分割线 + 居中「刷新」下划线（多选时隐藏，换 multi_bar）
    st.chrome.sync_btn = lv_obj_create(footer);
    lv_obj_remove_style_all(st.chrome.sync_btn);
    lv_obj_set_width(st.chrome.sync_btn, lv_pct(100));
    lv_obj_set_height(st.chrome.sync_btn, kSyncBtnH);
    lv_obj_set_style_bg_opa(st.chrome.sync_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(st.chrome.sync_btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(st.chrome.sync_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    Cloud_DisableScroll(st.chrome.sync_btn);
    lv_obj_add_flag(st.chrome.sync_btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(st.chrome.sync_btn);
    lv_obj_add_event_cb(st.chrome.sync_btn, OnSyncClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* sync_text = lv_obj_create(st.chrome.sync_btn);
    lv_obj_remove_style_all(sync_text);
    lv_obj_set_size(sync_text, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_border_side(sync_text, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(sync_text, kSyncUnderlineH, 0);
    lv_obj_set_style_border_color(sync_text, lv_color_black(), 0);
    lv_obj_set_style_pad_bottom(sync_text, 2, 0);
    lv_obj_set_style_pad_hor(sync_text, kSyncUnderlinePadHor, 0);
    lv_obj_set_style_bg_opa(sync_text, LV_OPA_TRANSP, 0);
    Cloud_DisableScroll(sync_text);
    lv_obj_clear_flag(sync_text, LV_OBJ_FLAG_CLICKABLE);

    st.chrome.sync_lbl = lv_label_create(sync_text);
    lv_label_set_text(st.chrome.sync_lbl, Lang::Strings::CLOUD_REFRESH);
    lv_obj_set_style_text_font(st.chrome.sync_lbl, Cloud_ItemFont(), 0);
    lv_obj_set_style_text_color(st.chrome.sync_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(st.chrome.sync_lbl, LV_OBJ_FLAG_CLICKABLE);

    st.chrome.multi_bar = lv_obj_create(footer);
    lv_obj_remove_style_all(st.chrome.multi_bar);
    lv_obj_set_width(st.chrome.multi_bar, lv_pct(100));
    lv_obj_set_height(st.chrome.multi_bar, kSyncBtnH);
    lv_obj_set_style_bg_opa(st.chrome.multi_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(st.chrome.multi_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st.chrome.multi_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(st.chrome.multi_bar, kMultiBarGap, 0);
    Cloud_DisableScroll(st.chrome.multi_bar);
    lv_obj_clear_flag(st.chrome.multi_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(st.chrome.multi_bar, LV_OBJ_FLAG_HIDDEN);

    // 与「刷新」同款：下划线文字；多项间居中「·」
    auto make_multi_action = [](lv_obj_t* parent, const char* text, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_obj_create(parent);
        lv_obj_remove_style_all(btn);
        lv_obj_set_height(btn, kSyncBtnH);
        lv_obj_set_width(btn, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_hor(btn, kMultiActionPadHor, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        Cloud_DisableScroll(btn);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(btn);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* text_wrap = lv_obj_create(btn);
        lv_obj_remove_style_all(text_wrap);
        lv_obj_set_size(text_wrap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_border_side(text_wrap, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(text_wrap, kSyncUnderlineH, 0);
        lv_obj_set_style_border_color(text_wrap, lv_color_black(), 0);
        lv_obj_set_style_pad_bottom(text_wrap, kSyncUnderlinePadBottom, 0);
        lv_obj_set_style_pad_hor(text_wrap, kSyncUnderlinePadHor, 0);
        lv_obj_set_style_bg_opa(text_wrap, LV_OPA_TRANSP, 0);
        Cloud_DisableScroll(text_wrap);
        lv_obj_clear_flag(text_wrap, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* lbl = lv_label_create(text_wrap);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, Cloud_ItemFont(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        return btn;
    };
    auto make_multi_dot = [](lv_obj_t* parent) {
        lv_obj_t* dot = lv_obj_create(parent);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, kMultiDotSize, kMultiDotSize);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        Cloud_DisableScroll(dot);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        return dot;
    };
    make_multi_action(st.chrome.multi_bar, Lang::Strings::COMMON_CANCEL, OnMultiCancel);
    make_multi_dot(st.chrome.multi_bar);
    make_multi_action(st.chrome.multi_bar, Lang::Strings::COMMON_SELECT_ALL, OnMultiSelectAll);
    make_multi_dot(st.chrome.multi_bar);
    make_multi_action(st.chrome.multi_bar, Lang::Strings::COMMON_DELETE, OnMultiDelete);
    make_multi_dot(st.chrome.multi_bar);
    make_multi_action(st.chrome.multi_bar, Lang::Strings::CLOUD_SAVE, OnMultiSave);

    // 页码贴底，对齐壁纸页
    st.chrome.page_lbl = lv_label_create(scr);
    lv_label_set_text(st.chrome.page_lbl, "");
    lv_obj_set_width(st.chrome.page_lbl, LV_HOR_RES - kPageLblMargin);
    lv_obj_set_style_text_align(st.chrome.page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(st.chrome.page_lbl, Cloud_ItemFont(), 0);
    lv_obj_set_style_text_color(st.chrome.page_lbl, lv_color_black(), 0);
    lv_obj_align(st.chrome.page_lbl, LV_ALIGN_BOTTOM_MID, 0, -kPageLblBottom);
    lv_obj_clear_flag(st.chrome.page_lbl, LV_OBJ_FLAG_CLICKABLE);
    Cloud_DisableScroll(st.chrome.page_lbl);

    st.chrome.radio_status_lbl = lv_label_create(list_frame);
    lv_label_set_text(st.chrome.radio_status_lbl, "");
    lv_label_set_long_mode(st.chrome.radio_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(st.chrome.radio_status_lbl, Cloud_ContentWidth() - kListFramePad * 2);
    lv_obj_set_style_text_align(st.chrome.radio_status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(st.chrome.radio_status_lbl, Cloud_UiFont(), 0);
    lv_obj_set_style_text_color(st.chrome.radio_status_lbl, lv_color_black(), 0);
    lv_obj_add_flag(st.chrome.radio_status_lbl, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(st.chrome.radio_status_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(st.chrome.radio_status_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(st.chrome.radio_status_lbl, LV_OBJ_FLAG_HIDDEN);
    Cloud_DisableScroll(st.chrome.radio_status_lbl);

    // 本地传输面板共用列表外框
    lv_obj_t* ap_panel = lv_obj_create(list_frame);
    st.chrome.ap_panel = ap_panel;
    lv_obj_remove_style_all(ap_panel);
    lv_obj_set_width(ap_panel, lv_pct(100));
    lv_obj_set_flex_grow(ap_panel, 1);
    lv_obj_set_flex_flow(ap_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ap_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ap_panel, 0, 0);
    Cloud_DisableScroll(ap_panel);
    lv_obj_add_flag(ap_panel, LV_OBJ_FLAG_HIDDEN);

    auto make_step = [](lv_obj_t* parent) {
        lv_obj_t* half = lv_obj_create(parent);
        lv_obj_remove_style_all(half);
        lv_obj_set_width(half, lv_pct(100));
        lv_obj_set_flex_grow(half, 1);
        lv_obj_set_flex_flow(half, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(half, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(half, kRowGap, 0);
        Cloud_DisableScroll(half);
        lv_obj_clear_flag(half, LV_OBJ_FLAG_CLICKABLE);
        return half;
    };
    auto add_center = [](lv_obj_t* parent, const char* text, bool bold) {
        lv_obj_t* lbl = lv_label_create(parent);
        lv_label_set_text(lbl, text);
        lv_obj_set_width(lbl, lv_pct(100));
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        const lv_font_t* font = Cloud_UiFont();
        if (bold) {
            const lv_font_t* heavy = fontpack_lv_font_get(30, 4);
            if (heavy != nullptr) {
                font = heavy;
            }
        }
        lv_obj_set_style_text_font(lbl, font, 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        return lbl;
    };
    lv_obj_t* ap_top = make_step(ap_panel);
    lv_obj_set_style_pad_top(ap_top, kListFramePad, 0);
    add_center(ap_top, Lang::Strings::CLOUD_AP_STEP1, true);
    add_center(ap_top, Lang::Strings::CLOUD_AP_JOIN, false);
    st.chrome.ap_qr_wifi = ApQrCreate(ap_top);
    st.chrome.ap_ssid_lbl = add_center(ap_top, "", false);
    st.chrome.ap_status_lbl = add_center(ap_top, "", false);
    lv_obj_add_flag(st.chrome.ap_status_lbl, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* ap_bot = make_step(ap_panel);
    lv_obj_set_style_border_side(ap_bot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(ap_bot, kSyncDividerW, 0);
    lv_obj_set_style_border_color(ap_bot, lv_color_black(), 0);
    lv_obj_set_style_pad_top(ap_bot, kListFramePad * 2, 0);
    add_center(ap_bot, Lang::Strings::CLOUD_AP_STEP2, true);
    add_center(ap_bot, Lang::Strings::CLOUD_AP_OPEN, false);
    st.chrome.ap_url_lbl = add_center(ap_bot, "", false);
    add_center(ap_bot, Lang::Strings::CLOUD_AP_HINT, false);
    st.chrome.ap_qr_url = ApQrCreate(ap_bot);

    lv_obj_t* wifi_panel = lv_obj_create(list_frame);
    st.chrome.wifi_panel = wifi_panel;
    lv_obj_remove_style_all(wifi_panel);
    lv_obj_set_width(wifi_panel, lv_pct(100));
    lv_obj_set_height(wifi_panel, 0);
    lv_obj_set_flex_grow(wifi_panel, 1);
    Cloud_DisableScroll(wifi_panel);
    lv_obj_clear_flag(wifi_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(wifi_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* usb_panel = lv_obj_create(list_frame);
    st.chrome.usb_panel = usb_panel;
    lv_obj_remove_style_all(usb_panel);
    lv_obj_set_width(usb_panel, lv_pct(100));
    lv_obj_set_flex_grow(usb_panel, 1);
    lv_obj_set_flex_flow(usb_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(usb_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(usb_panel, kPreviewGap, 0);
    lv_obj_set_style_pad_hor(usb_panel, UiSx(24), 0);
    Cloud_DisableScroll(usb_panel);
    lv_obj_add_flag(usb_panel, LV_OBJ_FLAG_HIDDEN);

    // 固定 6 条/页（Tab+标题后列表区刚好可排下）
    st.data.page_size = 6;
}

void BuildWallpaperPreviewChrome(lv_obj_t* scr, lv_coord_t status_height) {
    auto& st = Cloud_State();
    // 预览：图框 → 双行文件名 → 类型/大小胶囊 → 保存（对齐壁纸设置页）
    lv_obj_t* preview_body = lv_obj_create(scr);
    lv_obj_remove_style_all(preview_body);
    lv_obj_set_size(preview_body, LV_HOR_RES, LV_VER_RES - status_height);
    lv_obj_align(preview_body, LV_ALIGN_TOP_MID, 0, status_height);
    lv_obj_set_style_bg_opa(preview_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_hor(preview_body, kPad, 0);
    lv_obj_set_style_pad_top(preview_body, kPreviewBodyPadTop, 0);
    lv_obj_set_style_pad_bottom(preview_body, kPreviewBodyPadBot, 0);
    lv_obj_set_flex_flow(preview_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(preview_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(preview_body, kPreviewGap, 0);
    Cloud_DisableScroll(preview_body);
    lv_obj_add_flag(preview_body, LV_OBJ_FLAG_HIDDEN);
    st.preview.body = preview_body;

    lv_obj_t* img_host = lv_obj_create(preview_body);
    lv_obj_remove_style_all(img_host);
    lv_obj_set_width(img_host, Cloud_ContentWidth());
    lv_obj_set_flex_grow(img_host, 1);
    lv_obj_set_style_max_height(img_host, kPreviewImgMaxH, 0);
    lv_obj_set_style_bg_color(img_host, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(img_host, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(img_host, kRowBorderW, 0);
    lv_obj_set_style_border_color(img_host, lv_color_black(), 0);
    lv_obj_set_style_radius(img_host, kPreviewImgHostRadius, 0);
    lv_obj_set_style_clip_corner(img_host, true, 0);
    Cloud_DisableScroll(img_host);
    st.preview.img_host = img_host;

    lv_obj_t* img = lv_image_create(img_host);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    st.preview.img = img;

    lv_obj_t* preview_status = lv_label_create(img_host);
    lv_obj_set_style_text_font(preview_status, Cloud_UiFont(), 0);
    lv_obj_set_style_text_color(preview_status, lv_color_black(), 0);
    lv_obj_set_style_text_align(preview_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(preview_status, "");
    lv_obj_align(preview_status, LV_ALIGN_CENTER, 0, 0);
    st.preview.status = preview_status;

    lv_obj_t* preview_title = lv_label_create(preview_body);
    lv_obj_set_width(preview_title, Cloud_ContentWidth());
    {
        const lv_font_t* f = Cloud_UiFont();
        const lv_coord_t lh = (f != nullptr && f->line_height > 0) ? f->line_height : 29;
        lv_obj_set_height(preview_title, lh * 2);
    }
    lv_obj_set_style_text_font(preview_title, Cloud_UiFont(), 0);
    lv_obj_set_style_text_color(preview_title, lv_color_black(), 0);
    lv_obj_set_style_text_align(preview_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(preview_title, 0, 0);
    // 文案已按像素拆成最多两行；CLIP 避免再走 LVGL 整词换行
    lv_label_set_long_mode(preview_title, LV_LABEL_LONG_CLIP);
    lv_label_set_text(preview_title, "");
    st.preview.title = preview_title;

    lv_obj_t* preview_meta = lv_obj_create(preview_body);
    lv_obj_remove_style_all(preview_meta);
    lv_obj_set_width(preview_meta, Cloud_ContentWidth());
    lv_obj_set_height(preview_meta, kPreviewMetaH);
    lv_obj_set_flex_flow(preview_meta, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(preview_meta, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(preview_meta, kPreviewMetaGap, 0);
    lv_obj_clear_flag(preview_meta, LV_OBJ_FLAG_CLICKABLE);
    Cloud_DisableScroll(preview_meta);
    st.preview.meta = preview_meta;

    lv_obj_t* download_btn = lv_obj_create(preview_body);
    lv_obj_remove_style_all(download_btn);
    lv_obj_set_size(download_btn, Cloud_ContentWidth(), kActionH);
    lv_obj_set_style_pad_all(download_btn, 0, 0);
    lv_obj_set_flex_flow(download_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(download_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    Cloud_DisableScroll(download_btn);
    HapticAttachClick(download_btn);
    lv_obj_add_event_cb(download_btn, OnWallpaperDownloadClicked, LV_EVENT_CLICKED, nullptr);
    st.preview.download_btn = download_btn;

    lv_obj_t* download_lbl = lv_label_create(download_btn);
    lv_obj_set_style_text_font(download_lbl, Cloud_UiFont(), 0);
    lv_label_set_text(download_lbl, Lang::Strings::CLOUD_SAVE_LOCAL);
    st.preview.download_lbl = download_lbl;
    StyleDownloadBtn(true);
}

