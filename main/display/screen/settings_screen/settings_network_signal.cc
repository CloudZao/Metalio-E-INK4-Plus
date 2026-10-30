#include "settings_network_signal.h"
#include "assets/lang_cellular.h"
#include "assets/lang_config.h"
#include "board.h"
#include "dual_network_board.h"
#include "nt26_board.h"
#include "wifi_board.h"
#include "wifi_station.h"
#include "fontpack_lvgl.h"
#include "screen_common.h"
#include "ui_scale.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

constexpr const char* TAG = "SettingsSignal";
constexpr uint32_t kSignalPollMs = 2000; // 采样周期；4G 查询需占用 AT 通道
constexpr lv_coord_t kSignalPanelH = UiSy(48);
constexpr lv_coord_t kSignalTitleW = UiSx(120);
constexpr size_t kSignalTextMax = 48;

// 采样在后台执行；页面删除时递增代数，使迟到结果失效。
struct SignalUiMsg {
    uint32_t generation = 0; // 信号栏会话代数
    char text[kSignalTextMax] = {}; // 格式化后的信号状态
};

lv_obj_t* s_signal_value = nullptr;
lv_timer_t* s_signal_timer = nullptr;
std::atomic<bool> s_signal_alive{false};
std::atomic<uint32_t> s_signal_generation{0};
std::atomic<bool> s_signal_busy{false};
char s_last_signal_text[kSignalTextMax] = {};

void StopSignalTimer() {
    if (s_signal_timer != nullptr) {
        lv_timer_delete(s_signal_timer);
        s_signal_timer = nullptr;
    }
}

void InvalidateSignalSession() {
    s_signal_alive.store(false, std::memory_order_release);
    s_signal_generation.fetch_add(1, std::memory_order_acq_rel);
    StopSignalTimer();
    s_signal_value = nullptr;
    s_last_signal_text[0] = '\0';
}

void FormatWifiSignal(char* ui, size_t ui_len) {
    auto& wifi = WifiStation::GetInstance();
    if (!wifi.IsConnected()) {
        std::snprintf(ui, ui_len, "%s", Lang::Strings::SETTINGS_TEST_SIGNAL_WIFI_DISCONNECTED);
        return;
    }

    // 读取失败时明确显示状态，避免将无效 RSSI 当作有效信号。
    wifi_ap_record_t ap = {};
    if (!wifi.TryGetApInfo(ap)) {
        std::snprintf(ui, ui_len, "%s", Lang::Strings::SETTINGS_TEST_SIGNAL_WIFI_READ_FAIL);
        return;
    }

    const int rssi = static_cast<int>(ap.rssi);
    std::snprintf(ui, ui_len, Lang::Strings::SETTINGS_TEST_SIGNAL_WIFI_RSSI_FMT, rssi);
}

void FormatCellSignal(char* ui, size_t ui_len) {
    auto& board = Board::GetInstance();
    auto* dual = dynamic_cast<DualNetworkBoard*>(&board);
    auto* nt26 = dynamic_cast<Nt26Board*>(dual != nullptr ? &dual->GetCurrentBoard() : &board);
    if (nt26 == nullptr) {
        std::snprintf(ui, ui_len, "%s", Lang::Strings::SETTINGS_TEST_SIGNAL_CELL_UNAVAIL);
        return;
    }

    // 可能阻塞 AT；仅允许在采样任务线程调用。
    const int csq = nt26->GetSignalStrength();
    if (csq == 99 || csq < 0) {
        std::snprintf(ui, ui_len, "%s", Lang::Strings::SETTINGS_TEST_SIGNAL_CELL_CSQ_UNKNOWN);
        return;
    }

    std::snprintf(ui, ui_len, Lang::Strings::SETTINGS_TEST_SIGNAL_CELL_CSQ_FMT, csq);
}

void SampleSignalText(char* ui, size_t ui_len) {
    auto* dual = dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
    if (dual == nullptr && dynamic_cast<WifiBoard*>(&Board::GetInstance()) == nullptr) {
        std::snprintf(ui, ui_len, "%s", Lang::Strings::SETTINGS_TEST_SIGNAL_BOARD_UNSUP);
        return;
    }

    if (dual == nullptr || dual->GetNetworkType() == NetworkType::WIFI) {
        FormatWifiSignal(ui, ui_len);
    } else {
        FormatCellSignal(ui, ui_len);
    }
}

void ApplySignalUi(void* p) {
    auto* msg = static_cast<SignalUiMsg*>(p);
    if (msg == nullptr) {
        return;
    }

    const bool live = s_signal_alive.load(std::memory_order_acquire) &&
                      msg->generation == s_signal_generation.load(std::memory_order_acquire) &&
                      s_signal_value != nullptr && lv_obj_is_valid(s_signal_value);
    if (live) {
        if (std::strncmp(s_last_signal_text, msg->text, kSignalTextMax) != 0) {
            lv_label_set_text(s_signal_value, msg->text);
            std::strncpy(s_last_signal_text, msg->text, kSignalTextMax - 1);
            s_last_signal_text[kSignalTextMax - 1] = '\0';
        }
    }
    delete msg;
}

void SignalSampleTask(void* /*arg*/) {
    const uint32_t gen = s_signal_generation.load(std::memory_order_acquire);

    char ui[kSignalTextMax] = {};
    SampleSignalText(ui, sizeof(ui));

    if (!s_signal_alive.load(std::memory_order_acquire) ||
        gen != s_signal_generation.load(std::memory_order_acquire)) {
        s_signal_busy.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }

    auto* msg = new (std::nothrow) SignalUiMsg{};
    if (msg == nullptr) {
        ESP_LOGW(TAG, "signal ui msg alloc failed");
        s_signal_busy.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }
    msg->generation = gen;
    std::strncpy(msg->text, ui, sizeof(msg->text) - 1);
    msg->text[sizeof(msg->text) - 1] = '\0';

    // busy 在投递成功后仍由本任务清掉：下一拍可再采样；迟到 Apply 靠 generation 丢弃。
    if (!ScreenLvAsyncUrgent(ApplySignalUi, msg)) {
        ESP_LOGW(TAG, "PostLvAsync signal ui failed");
        delete msg;
    }
    s_signal_busy.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

void KickSignalSample() {
    if (!s_signal_alive.load(std::memory_order_acquire)) {
        return;
    }
    bool expected = false;
    if (!s_signal_busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    const BaseType_t ok =
        xTaskCreate(SignalSampleTask, "stest_sig", 4096, nullptr, 5, nullptr);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "signal sample task create failed");
        s_signal_busy.store(false, std::memory_order_release);
    }
}

void OnSignalPollTimer(lv_timer_t* /*t*/) {
    KickSignalSample();
}

lv_obj_t* CreateSignalPanel(lv_obj_t* page) {
    lv_obj_t* row = lv_obj_create(page);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, kSignalPanelH, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_border_color(row, lv_color_black(), 0);
    lv_obj_set_style_pad_hor(row, 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title = lv_label_create(row);
    lv_label_set_text(title, Lang::Strings::SETTINGS_TEST_SIGNAL_TITLE);
    lv_obj_set_width(title, kSignalTitleW);
    lv_obj_set_height(title, LV_SIZE_CONTENT);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* value = lv_label_create(row);
    lv_label_set_text(value, Lang::Strings::SETTINGS_TEST_SIGNAL_READING);
    lv_obj_set_width(value, 0);
    lv_obj_set_flex_grow(value, 1);
    lv_obj_set_height(value, LV_SIZE_CONTENT);
    lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(value, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(value, lv_color_black(), 0);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_clear_flag(value, LV_OBJ_FLAG_CLICKABLE);
    return value;
}

}  // namespace

void SettingsNetworkSignal_Build(lv_obj_t* parent) {
    InvalidateSignalSession();
    s_signal_value = CreateSignalPanel(parent);
    lv_obj_add_event_cb(s_signal_value, [](lv_event_t* e) {
        if (lv_event_get_target(e) == s_signal_value) {
            InvalidateSignalSession();
        }
    }, LV_EVENT_DELETE, nullptr);
    s_signal_alive.store(true, std::memory_order_release);
    s_signal_timer = lv_timer_create(OnSignalPollTimer, kSignalPollMs, nullptr);
    KickSignalSample();
}
