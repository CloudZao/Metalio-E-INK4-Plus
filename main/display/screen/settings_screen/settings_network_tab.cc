#include "assets/lang_cellular.h"
#include "settings_network_tab.h"
#include "settings_network_signal.h"

#include "application.h"
#include "board.h"
#include "dual_network_board.h"
#include "nt26_board.h"
#include "settings.h"
#include "settings_common.h"
#include "screen_common.h"
#include "wifi_board.h"
#include "network_screen/network_screen.h"
#include "sdkconfig.h"


#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "fontpack_lvgl.h"
#include "assets/lang_config.h"

namespace {

constexpr const char* TAG = "SettingsNetwork";
constexpr int kSimSlotExternal = 0;
constexpr int kSimSlotInternal = 1;

struct NetworkUi {
    lv_obj_t* root_scr = nullptr;
    lv_obj_t* wifi_btn = nullptr;
    lv_obj_t* cell_btn = nullptr;
    lv_obj_t* current_lbl = nullptr;
    lv_obj_t* wifi_cfg_box = nullptr;
    lv_obj_t* wifi_cfg_btn = nullptr;
    lv_obj_t* sim_box = nullptr;
    lv_obj_t* sim_ext_btn = nullptr;
    lv_obj_t* sim_int_btn = nullptr;
    lv_obj_t* sim_cur_lbl = nullptr;
    lv_obj_t* overlay = nullptr;
    lv_obj_t* overlay_msg = nullptr;
    lv_timer_t* reboot_timer = nullptr;
    int reboot_remaining = 0;
    std::string reboot_headline;
};

NetworkUi s_ui;
bool s_sim_switch_pending = false;
// LVGL worker 栈在 PSRAM：UI 路径只碰 RAM 缓存；NVS 在 Schedule / FreeRTOS 任务里做。
std::atomic<int> s_cached_sim_slot{kSimSlotExternal}; // UI、存储和 SIM 切换任务共享的槽位快照
bool s_tab_alive = false;

DualNetworkBoard* GetDualBoard() {
    return dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
}

Nt26Board* GetNt26Board() {
    auto& board = Board::GetInstance();
    if (auto* dual = dynamic_cast<DualNetworkBoard*>(&board)) {
        return dynamic_cast<Nt26Board*>(&dual->GetCurrentBoard());
    }
    return dynamic_cast<Nt26Board*>(&board);
}

WifiBoard* GetWifiBoard() {
    auto& board = Board::GetInstance();
    if (auto* dual = dynamic_cast<DualNetworkBoard*>(&board)) {
        return dynamic_cast<WifiBoard*>(&dual->GetCurrentBoard());
    }
    return dynamic_cast<WifiBoard*>(&board);
}

NetworkType CurrentNetworkType() {
    if (auto* dual = GetDualBoard()) {
        return dual->GetNetworkType();
    }
    return DualNetworkBoard::LoadNetworkTypeFromSettings(1);
}

bool IsCellNetwork() { return CurrentNetworkType() == NetworkType::CELLULAR; }

int GetCachedSimSlot() {
    return (s_cached_sim_slot == kSimSlotInternal) ? kSimSlotInternal : kSimSlotExternal;
}

void SetCachedSimSlot(int slot) {
    s_cached_sim_slot = (slot == kSimSlotInternal) ? kSimSlotInternal : kSimSlotExternal;
}

void PersistSimSlotToNvs(int slot) {
    const int normalized = (slot == kSimSlotInternal) ? kSimSlotInternal : kSimSlotExternal;
    Settings settings("network", true);
    settings.SetInt("sim_slot", normalized);
}

void SchedulePersistSimSlot(int slot) {
    Application::GetInstance().Schedule([slot]() { PersistSimSlotToNvs(slot); }, Application::TaskDomain::Storage);
}

const char* SimSlotName(int slot) {
    return (slot == kSimSlotInternal) ? Lang::Strings::SETTINGS_NET_SIM_INT : Lang::Strings::SETTINGS_NET_SIM_EXT;
}

void CloseOverlay() {
    if (s_ui.reboot_timer != nullptr) {
        lv_timer_delete(s_ui.reboot_timer);
        s_ui.reboot_timer = nullptr;
    }
    if (s_ui.overlay != nullptr) {
        lv_obj_delete(s_ui.overlay);
    }
    s_ui.overlay = nullptr;
    s_ui.overlay_msg = nullptr;
    s_ui.reboot_remaining = 0;
    s_ui.reboot_headline.clear();
}

void OpenOverlay(const char* msg) {
    if (s_ui.root_scr == nullptr) {
        return;
    }
    CloseOverlay();

    lv_obj_t* mask = lv_obj_create(s_ui.root_scr);
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(mask, 0, 0);
    ScreenApplyDotBackdrop(mask);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    s_ui.overlay = mask;

    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_HOR_RES - UiSx(80), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(card, UiSy(96), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, UiSx(20), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl = lv_label_create(card);
    s_ui.overlay_msg = lbl;
    lv_label_set_text(lbl, msg != nullptr ? msg : "");
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
}

void SetOverlayText(const char* msg) {
    if (s_ui.overlay_msg != nullptr) {
        lv_label_set_text(s_ui.overlay_msg, msg != nullptr ? msg : "");
    }
}

void RebootTask(void* /*arg*/) {
    vTaskDelay(pdMS_TO_TICKS(200));
    Application::GetInstance().Reboot();
    vTaskDelete(nullptr);
}

void RebootTimerCb(lv_timer_t* /*timer*/) {
    s_ui.reboot_remaining--;
    if (s_ui.reboot_remaining > 0) {
        if (s_ui.overlay_msg != nullptr) {
            char buf[128];
            snprintf(buf, sizeof(buf), Lang::Strings::SETTINGS_NET_REBOOT_COUNT_FMT, s_ui.reboot_headline.c_str(),
                     s_ui.reboot_remaining);
            lv_label_set_text(s_ui.overlay_msg, buf);
        }
        return;
    }
    if (s_ui.reboot_timer != nullptr) {
        lv_timer_delete(s_ui.reboot_timer);
        s_ui.reboot_timer = nullptr;
    }
    SetOverlayText(Lang::Strings::SETTINGS_NET_REBOOTING);
    xTaskCreate(RebootTask, "sim_reboot", 2048, nullptr, 5, nullptr);
}

void OpenRestartCountdown(const char* headline) {
    s_ui.reboot_headline = headline != nullptr ? headline : "";
    s_ui.reboot_remaining = 3;
    char buf[128];
    snprintf(buf, sizeof(buf), Lang::Strings::SETTINGS_NET_REBOOT_COUNT_FMT, s_ui.reboot_headline.c_str(),
             s_ui.reboot_remaining);
    OpenOverlay(buf);
    if (s_ui.reboot_timer != nullptr) {
        lv_timer_delete(s_ui.reboot_timer);
    }
    s_ui.reboot_timer = lv_timer_create(RebootTimerCb, 1000, nullptr);
}

void RefreshSimUi() {
    if (s_ui.sim_ext_btn == nullptr || s_ui.sim_int_btn == nullptr) {
        return;
    }
    const int slot = GetCachedSimSlot();
    const bool ext = (slot == kSimSlotExternal);
    SettingsStyleSelectable(s_ui.sim_ext_btn, lv_obj_get_child(s_ui.sim_ext_btn, 0), ext);
    SettingsStyleSelectable(s_ui.sim_int_btn, lv_obj_get_child(s_ui.sim_int_btn, 0), !ext);
    if (s_ui.sim_cur_lbl != nullptr) {
        char buf[48];
        snprintf(buf, sizeof(buf), Lang::Strings::SETTINGS_NET_CURRENT_FMT, SimSlotName(slot));
        lv_label_set_text(s_ui.sim_cur_lbl, buf);
    }
}

void SetSimSectionVisible(bool visible) {
    if (s_ui.sim_box == nullptr) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(s_ui.sim_box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.sim_box, LV_OBJ_FLAG_HIDDEN);
    }
}

void SetWifiConfigSectionVisible(bool visible) {
    if (s_ui.wifi_cfg_box == nullptr) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(s_ui.wifi_cfg_box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.wifi_cfg_box, LV_OBJ_FLAG_HIDDEN);
    }
}

void RefreshNetworkOptions() {
    const bool is_cell = IsCellNetwork();
    if (s_ui.wifi_btn != nullptr) {
        SettingsStyleSelectable(s_ui.wifi_btn, lv_obj_get_child(s_ui.wifi_btn, 0), !is_cell);
    }
    if (s_ui.cell_btn != nullptr) {
        SettingsStyleSelectable(s_ui.cell_btn, lv_obj_get_child(s_ui.cell_btn, 0), is_cell);
    }
    if (s_ui.current_lbl != nullptr) {
        lv_label_set_text(s_ui.current_lbl, is_cell ? Lang::Strings::SETTINGS_NET_CUR_4G : Lang::Strings::SETTINGS_NET_CUR_WIFI);
    }
    // 仅当前为 WiFi 时展示进入配网；仅 4G 时展示内外置卡切换
    SetWifiConfigSectionVisible(!is_cell);
    SetSimSectionVisible(is_cell);
    if (is_cell) {
        RefreshSimUi();
    }
}

struct SwitchNetworkJob {
    NetworkType target;
};

void SwitchNetworkTask(void* arg) {
    auto* job = static_cast<SwitchNetworkJob*>(arg);
    auto* dual = GetDualBoard();
    if (dual == nullptr) {
        ESP_LOGW(TAG, "not a DualNetworkBoard, ignore network switch");
    } else {
        ESP_LOGI(TAG, "switch network to %s", job->target == NetworkType::WIFI ? "WiFi" : "4G");
        dual->SwitchToNetworkType(job->target);
    }
    delete job;
    vTaskDelete(nullptr);
}

[[maybe_unused]] void OnNetworkOptionClicked(lv_event_t* e) {
    const auto target = static_cast<NetworkType>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (CurrentNetworkType() == target) {
        // 点已选中的 4G：确保 SIM 区可见；点已选中的 WiFi：确保配网区可见
        if (target == NetworkType::CELLULAR) {
            SetSimSectionVisible(true);
            RefreshSimUi();
        } else {
            SetWifiConfigSectionVisible(true);
        }
        return;
    }
    if (target == NetworkType::WIFI) {
        SettingsStyleSelectable(s_ui.wifi_btn, lv_obj_get_child(s_ui.wifi_btn, 0), true);
        SettingsStyleSelectable(s_ui.cell_btn, lv_obj_get_child(s_ui.cell_btn, 0), false);
        SetSimSectionVisible(false);
        SetWifiConfigSectionVisible(true);
        if (s_ui.current_lbl != nullptr) {
            lv_label_set_text(s_ui.current_lbl, Lang::Strings::SETTINGS_NET_SWITCH_WIFI);
        }
    } else {
        SettingsStyleSelectable(s_ui.wifi_btn, lv_obj_get_child(s_ui.wifi_btn, 0), false);
        SettingsStyleSelectable(s_ui.cell_btn, lv_obj_get_child(s_ui.cell_btn, 0), true);
        SetWifiConfigSectionVisible(false);
        SetSimSectionVisible(true);
        RefreshSimUi();
        if (s_ui.current_lbl != nullptr) {
            lv_label_set_text(s_ui.current_lbl, Lang::Strings::SETTINGS_NET_SWITCH_4G);
        }
    }
    auto* job = new SwitchNetworkJob{target};
    if (xTaskCreate(SwitchNetworkTask, "net_switch", 4096, job, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(net_switch) failed");
        delete job;
        RefreshNetworkOptions();
    }
}

// ---- SIM：查询 / 切换（AT+CFUN=0 / AT+ECSIMCFG=SimSlot,X / AT+CFUN=1）----

int ParseSimSlotFromEcsimcfg(const std::string& resp) {
    constexpr const char* kKey = "\"SimSlot\"";
    size_t pos = 0;
    while ((pos = resp.find(kKey, pos)) != std::string::npos) {
        size_t comma = resp.find(',', pos);
        if (comma == std::string::npos) {
            return -1;
        }
        size_t i = comma + 1;
        while (i < resp.size() && (resp[i] == ' ' || resp[i] == '\t')) {
            ++i;
        }
        if (i >= resp.size() || !std::isdigit(static_cast<unsigned char>(resp[i]))) {
            pos = comma + 1;
            continue;
        }
        int slot = 0;
        while (i < resp.size() && std::isdigit(static_cast<unsigned char>(resp[i]))) {
            slot = slot * 10 + (resp[i] - '0');
            ++i;
        }
        return slot;
    }
    return -1;
}

struct SimSlotQueryMsg {
    int slot;
};

void AsyncSimSlotQueried(void* user_data) {
    auto* msg = static_cast<SimSlotQueryMsg*>(user_data);
    if (s_tab_alive && msg->slot >= 0) {
        if (GetCachedSimSlot() != msg->slot) {
            SetCachedSimSlot(msg->slot);
            SchedulePersistSimSlot(msg->slot);
            ESP_LOGI(TAG, "sim_slot synced from modem: %d", msg->slot);
        }
        RefreshSimUi();
    }
    delete msg;
}

void SimSlotQueryTask(void* /*arg*/) {
    auto* msg = new SimSlotQueryMsg{-1};
    Nt26Board* nt26 = GetNt26Board();
    if (nt26 != nullptr) {
        std::string resp;
        esp_err_t err = nt26->SendAtCommand("AT+ECSIMCFG?", resp, 5000, true);
        ESP_LOGI(TAG, "AT 'AT+ECSIMCFG?' -> err=%d", (int)err);
        if (err == ESP_OK && resp.find("OK") != std::string::npos) {
            int slot = ParseSimSlotFromEcsimcfg(resp);
            if (slot == kSimSlotExternal || slot == kSimSlotInternal) {
                msg->slot = slot;
            }
        }
    }
    if (!ScreenLvAsync(AsyncSimSlotQueried, msg)) {
        delete msg;
    }
    vTaskDelete(nullptr);
}

void ScheduleSimSlotQuery() {
    if (GetNt26Board() == nullptr) {
        return;
    }
    if (xTaskCreate(SimSlotQueryTask, "sim_query", 4096, nullptr, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(sim_query) failed");
    }
}

struct AsyncStringMsg {
    std::string text;
};

void AsyncSimProgress(void* user_data) {
    auto* msg = static_cast<AsyncStringMsg*>(user_data);
    if (s_tab_alive) {
        SetOverlayText(msg->text.c_str());
    }
    delete msg;
}

void PostSimProgress(const std::string& text) {
    if (!s_tab_alive) {
        return;
    }
    auto* msg = new AsyncStringMsg{text};
    if (!ScreenLvAsync(AsyncSimProgress, msg)) {
        delete msg;
    }
}

struct SimSwitchResultMsg {
    bool success;
    int target_slot;
    std::string detail;
};

void AsyncSimSwitchDone(void* user_data) {
    auto* msg = static_cast<SimSwitchResultMsg*>(user_data);
    s_sim_switch_pending = false;
    if (s_tab_alive) {
        if (msg->success) {
            char buf[64];
            snprintf(buf, sizeof(buf), Lang::Strings::SETTINGS_NET_SWITCHED_FMT, SimSlotName(msg->target_slot));
            OpenRestartCountdown(buf);
            RefreshSimUi();
        } else {
            CloseOverlay();
            ScheduleSimSlotQuery();
            RefreshSimUi();
            if (s_ui.sim_cur_lbl != nullptr) {
                lv_label_set_text(s_ui.sim_cur_lbl, msg->detail.c_str());
            }
        }
    }
    delete msg;
}

void PostSimSwitchDone(SimSwitchResultMsg* result) {
    if (result == nullptr) {
        return;
    }
    if (!ScreenLvAsync(AsyncSimSwitchDone, result)) {
        s_sim_switch_pending = false;
        delete result;
    }
}

struct SimSwitchCtx {
    int target_slot;
};

void SimSwitchTask(void* arg) {
    {
        std::unique_ptr<SimSwitchCtx> ctx(static_cast<SimSwitchCtx*>(arg));
        auto* result = new SimSwitchResultMsg{};
        result->target_slot = ctx->target_slot;
        result->success = false;
        auto* nt26 = GetNt26Board();
        std::string resp;
        const auto run_at = [nt26](const std::string& cmd, std::string& response, uint32_t timeout_ms) {
            response.clear();
            return nt26->SendAtCommand(cmd, response, timeout_ms, true);
        };
        do {
            if (nt26 == nullptr) {
                result->detail = Lang::Strings::SETTINGS_NET_NO_4G;
                break;
            }
            PostSimProgress(Lang::Strings::SETTINGS_NET_CFUN0);
            if (run_at("AT+CFUN=0", resp, 8000) != ESP_OK || resp.find("OK") == std::string::npos) {
                result->detail = Lang::Strings::SETTINGS_NET_CFUN0_FAIL;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));

            char cmd[64];
            snprintf(cmd, sizeof(cmd), "AT+ECSIMCFG=SimSlot,%d", ctx->target_slot);
            char prog[96];
            snprintf(prog, sizeof(prog), Lang::Strings::SETTINGS_NET_SWITCH_SIM_FMT, SimSlotName(ctx->target_slot), cmd);
            PostSimProgress(prog);
            if (run_at(cmd, resp, 5000) != ESP_OK || resp.find("OK") == std::string::npos) {
                std::string recovery;
                run_at("AT+CFUN=1", recovery, 10000);
                result->detail = Lang::Strings::SETTINGS_NET_ECSIMCFG_FAIL;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));

            PostSimProgress(Lang::Strings::SETTINGS_NET_CFUN1);
            if (run_at("AT+CFUN=1", resp, 15000) != ESP_OK || resp.find("OK") == std::string::npos) {
                result->detail = Lang::Strings::SETTINGS_NET_CFUN1_FAIL;
                break;
            }
            SetCachedSimSlot(ctx->target_slot);
            PersistSimSlotToNvs(ctx->target_slot);
            result->success = true;
        } while (false);
        PostSimSwitchDone(result);
    }
    // FreeRTOS 删除任务不展开 C++ 栈，先归还参数及命令字符串。
    vTaskDelete(nullptr);
}

void ScheduleSimSwitch(int target_slot) {
    if (s_sim_switch_pending) {
        return;
    }
    if (target_slot != kSimSlotExternal && target_slot != kSimSlotInternal) {
        return;
    }
    if (GetCachedSimSlot() == target_slot) {
        return;
    }
    if (!IsCellNetwork() || GetNt26Board() == nullptr) {
        if (s_ui.sim_cur_lbl != nullptr) {
            lv_label_set_text(s_ui.sim_cur_lbl, Lang::Strings::SETTINGS_NET_NOT_4G);
        }
        return;
    }

    s_sim_switch_pending = true;
    char buf[80];
    snprintf(buf, sizeof(buf), Lang::Strings::SETTINGS_NET_SWITCH_SIM_CFUN0_FMT, SimSlotName(target_slot));
    OpenOverlay(buf);
    auto* ctx = new SimSwitchCtx{target_slot};
    if (xTaskCreate(SimSwitchTask, "sim_switch", 4096, ctx, 5, nullptr) != pdPASS) {
        delete ctx;
        s_sim_switch_pending = false;
        CloseOverlay();
        if (s_ui.sim_cur_lbl != nullptr) {
            lv_label_set_text(s_ui.sim_cur_lbl, Lang::Strings::SETTINGS_NET_SIM_TASK_FAIL);
        }
    }
}

void OnSimExtClicked(lv_event_t* /*e*/) { ScheduleSimSwitch(kSimSlotExternal); }
void OnSimIntClicked(lv_event_t* /*e*/) { ScheduleSimSwitch(kSimSlotInternal); }

void OnEnterWifiConfigClicked(lv_event_t* /*e*/) {
    if (IsCellNetwork()) {
        return;
    }
    if (GetWifiBoard() == nullptr) {
        if (s_ui.current_lbl != nullptr) {
            lv_label_set_text(s_ui.current_lbl, Lang::Strings::SETTINGS_NET_BOARD_NO_AP);
        }
        return;
    }
    ScreenNavigateTo(NetworkScreen::Create);
}

void AsyncRefreshSimUi(void* /*user_data*/) {
    if (s_tab_alive) {
        RefreshSimUi();
    }
}

[[maybe_unused]] void ScheduleLoadSimSlotFromNvs() {
    Application::GetInstance().Schedule([]() {
        Settings settings("network", false);
        SetCachedSimSlot(settings.GetInt("sim_slot", kSimSlotExternal));
        ESP_LOGI(TAG, "sim_slot loaded from NVS: %d", GetCachedSimSlot());
        // Application 任务非 LVGL：须 ScreenLvAsync，否则与删屏竞态会搞坏 async 链表
        ScreenLvAsync(AsyncRefreshSimUi);
    }, Application::TaskDomain::Storage);
}

void BuildWifiConfigSection(lv_obj_t* page) {
    s_ui.wifi_cfg_box = lv_obj_create(page);
    lv_obj_remove_style_all(s_ui.wifi_cfg_box);
    lv_obj_set_width(s_ui.wifi_cfg_box, lv_pct(100));
    lv_obj_set_height(s_ui.wifi_cfg_box, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_ui.wifi_cfg_box, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_ui.wifi_cfg_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.wifi_cfg_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_ui.wifi_cfg_box, kSettingsOptionGap, 0);
    lv_obj_clear_flag(s_ui.wifi_cfg_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_ui.wifi_cfg_box, LV_OBJ_FLAG_CLICKABLE);

    #if !CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS_GLOBAL
    lv_obj_t* split = lv_obj_create(s_ui.wifi_cfg_box);
    lv_obj_remove_style_all(split);
    lv_obj_set_size(split, lv_pct(100), 2);
    lv_obj_set_style_bg_color(split, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(split, LV_OPA_COVER, 0);
    lv_obj_clear_flag(split, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(split, LV_OBJ_FLAG_SCROLLABLE);
    #endif

    lv_obj_t* title = lv_label_create(s_ui.wifi_cfg_box);
    lv_label_set_text(title, Lang::Strings::SETTINGS_NET_WIFI_CFG_TITLE);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* hint = lv_label_create(s_ui.wifi_cfg_box);
    lv_label_set_text(hint, Lang::Strings::SETTINGS_NET_WIFI_CFG_HINT);
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(hint, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);

    s_ui.wifi_cfg_btn =
        SettingsCreateSelectableOption(s_ui.wifi_cfg_box, Lang::Strings::SETTINGS_NET_ENTER_CFG, OnEnterWifiConfigClicked, 0);
    SettingsStyleSelectable(s_ui.wifi_cfg_btn, lv_obj_get_child(s_ui.wifi_cfg_btn, 0), false);

    SetWifiConfigSectionVisible(false);
}

[[maybe_unused]] void BuildSimSection(lv_obj_t* page) {
    s_ui.sim_box = lv_obj_create(page);
    lv_obj_remove_style_all(s_ui.sim_box);
    lv_obj_set_width(s_ui.sim_box, lv_pct(100));
    lv_obj_set_height(s_ui.sim_box, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_ui.sim_box, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_ui.sim_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.sim_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_ui.sim_box, kSettingsOptionGap, 0);
    lv_obj_clear_flag(s_ui.sim_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_ui.sim_box, LV_OBJ_FLAG_CLICKABLE);

    // SIM 区与上方上网方式之间的分割横线
    lv_obj_t* split = lv_obj_create(s_ui.sim_box);
    lv_obj_remove_style_all(split);
    lv_obj_set_size(split, lv_pct(100), 2);
    lv_obj_set_style_bg_color(split, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(split, LV_OPA_COVER, 0);
    lv_obj_clear_flag(split, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title = lv_label_create(s_ui.sim_box);
    lv_label_set_text(title, Lang::Strings::SETTINGS_NET_SIM_TITLE);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* hint = lv_label_create(s_ui.sim_box);
    lv_label_set_text(hint, Lang::Strings::SETTINGS_NET_REBOOT_HINT);
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(hint, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);

    s_ui.sim_ext_btn =
        SettingsCreateSelectableOption(s_ui.sim_box, Lang::Strings::SETTINGS_NET_SIM_EXT, OnSimExtClicked, 0);
    s_ui.sim_int_btn =
        SettingsCreateSelectableOption(s_ui.sim_box, Lang::Strings::SETTINGS_NET_SIM_INT, OnSimIntClicked, 0);

    s_ui.sim_cur_lbl = lv_label_create(s_ui.sim_box);
    lv_label_set_text(s_ui.sim_cur_lbl, Lang::Strings::SETTINGS_NET_CUR_DASH);
    lv_obj_set_style_text_font(s_ui.sim_cur_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_ui.sim_cur_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(s_ui.sim_cur_lbl, LV_OBJ_FLAG_CLICKABLE);

    SetSimSectionVisible(false);
}

}  // namespace

void SettingsNetworkTab_Reset() {
    CloseOverlay();
    s_tab_alive = false;
    s_sim_switch_pending = false;
    s_ui = {};
}

const char* SettingsNetworkTab_CurrentName() {
    return IsCellNetwork() ? "4G" : "WiFi";
}

bool SettingsNetworkTab_IsWifi() {
    return !IsCellNetwork();
}

void SettingsNetworkTab_Build(lv_obj_t* page) {
    SettingsNetworkSignal_Build(page);
    s_ui = {};
    s_tab_alive = true;
    s_sim_switch_pending = false;
    s_ui.root_scr = lv_obj_get_screen(page);

    #if CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS_GLOBAL
    BuildWifiConfigSection(page);
    SetWifiConfigSectionVisible(true);
    #else
    lv_obj_t* title = lv_label_create(page);
    lv_label_set_text(title, Lang::Strings::SETTINGS_NET_MODE_TITLE);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* hint = lv_label_create(page);
    lv_label_set_text(hint, Lang::Strings::SETTINGS_NET_REBOOT_HINT);
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(hint, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);

    // 4G 在前：与出厂默认一致
    s_ui.cell_btn = SettingsCreateSelectableOption(page, Lang::Strings::SETTINGS_NET_MODE_4G, OnNetworkOptionClicked,
                                                   static_cast<intptr_t>(NetworkType::CELLULAR));
    s_ui.wifi_btn = SettingsCreateSelectableOption(page, Lang::Strings::SETTINGS_NET_MODE_WIFI, OnNetworkOptionClicked,
                                                   static_cast<intptr_t>(NetworkType::WIFI));

    s_ui.current_lbl = lv_label_create(page);
    lv_label_set_text(s_ui.current_lbl, "");
    lv_obj_set_style_text_font(s_ui.current_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_ui.current_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(s_ui.current_lbl, LV_OBJ_FLAG_CLICKABLE);

    BuildWifiConfigSection(page);
    BuildSimSection(page);
    RefreshNetworkOptions();
    ScheduleLoadSimSlotFromNvs();
    if (IsCellNetwork()) {
        ScheduleSimSlotQuery();
    }
    #endif
}
