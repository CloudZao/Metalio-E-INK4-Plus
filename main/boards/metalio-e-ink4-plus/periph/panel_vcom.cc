#include "panel_vcom.h"

#include "config.h"
#include "assets/lang_screen.h"
#include "screen_common.h"
#include "epd_refresh.h"
#include "metalio_audio.h"
#include "power_policy.h"
#include "simple_uart.hpp"
#include <cJSON.h>
#include <driver/usb_serial_jtag.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

static const char* TAG = "PanelVcom";
// 数据编号 470 由本工程保留；单条记录避免标识与电压分别更新。
static constexpr int kSlot = 470;
struct Request {
    bool write; // 写入或读取
    int mv; // 负毫伏值
    uint32_t id; // 工具请求编号，0 为设备界面操作
    char panel[80]; // 屏幕标签标识
};

static QueueHandle_t s_queue;
static std::atomic<bool> s_busy{false};
static std::mutex s_state_mutex, s_rx_mutex;
static PanelVcomState s_state{};
static char s_panel[80]{};
static char s_line[256]{};
static size_t s_length;
static bool s_overflow, s_waiting, s_received;
static char s_expected[48]{}, s_reply[256]{};
static bool s_usb;
static const char* s_stage = "read";

static void NotifyResult(bool write, const char* error) {
    const uintptr_t notice = error && !strcmp(error, "INVALID_VCOM") ? 5 :
        (error ? (write ? 4 : 3) : (write ? 2 : 1));
    if (!ScreenLvAsync([](void* data) {
        const auto kind = reinterpret_cast<uintptr_t>(data);
        const char* text = kind == 5 ? Lang::Strings::SETTINGS_VCOM_INVALID :
            kind == 4 ? Lang::Strings::SETTINGS_VCOM_WRITE_FAILED :
            kind == 3 ? Lang::Strings::SETTINGS_VCOM_READ_FAILED :
            kind == 2 ? Lang::Strings::SETTINGS_VCOM_WRITE_OK : Lang::Strings::SETTINGS_VCOM_READ_OK;
        ScreenShowToast(text);
    }, reinterpret_cast<void*>(notice))) {
        ESP_LOGW(TAG, "result notification enqueue failed");
    }
}

static bool ValidMv(int mv) {
    // 按板级校准范围校验负毫伏值，保留驱动所需的 10mV 步进。
    return mv <= -EPD_VCOM_MIN_MV && mv >= -EPD_VCOM_MAX_MV && mv % 10 == 0;
}

static bool ValidPanel(const char* text) {
    if (!text || !*text || strlen(text) >= sizeof(s_panel)) return false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return false;
    }
    return true;
}

void PanelVcomOnUartData(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(s_rx_mutex);
    for (uint8_t c : data) {
        if (c == '\r' || c == '\n') {
            s_line[s_length] = 0;
            if (s_waiting && !s_overflow &&
                (strncmp(s_line, s_expected, strlen(s_expected)) == 0 ||
                 strcmp(s_line, "ERROR") == 0 || strcmp(s_line, "FAIL") == 0)) {
                snprintf(s_reply, sizeof(s_reply), "%s", s_line);
                s_received = true;
                s_waiting = false;
            }
            s_length = 0;
            s_overflow = false;
        } else if (c >= 32 && c < 127) {
            if (s_length + 1 < sizeof(s_line)) s_line[s_length++] = static_cast<char>(c);
            else s_overflow = true;
        } else {
            s_overflow = true;
        }
    }
}

static const char* Exchange(const char* command, const char* prefix, int timeout_ms = 1800) {
    {
        std::lock_guard<std::mutex> lock(s_rx_mutex);
        snprintf(s_expected, sizeof(s_expected), "%s", prefix);
        s_received = false;
        s_waiting = true;
        s_length = 0;
        s_overflow = false;
    }
    if (!SimpleUart::getInstance().sendString(command)) {
        std::lock_guard<std::mutex> lock(s_rx_mutex);
        s_waiting = false;
        return "BH_TX_FAILED";
    }
    const int64_t deadline = esp_timer_get_time() + int64_t(timeout_ms) * 1000;
    while (esp_timer_get_time() < deadline) {
        {
            std::lock_guard<std::mutex> lock(s_rx_mutex);
            if (s_received) return nullptr;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    std::lock_guard<std::mutex> lock(s_rx_mutex);
    s_waiting = false;
    return "BH_TIMEOUT";
}

static const char* Execute(const Request& request) {
    s_stage = "input";
    if (request.write && !ValidMv(request.mv)) return "INVALID_VCOM";
    s_stage = "ready";
    PowerNeedHold hold(PowerNeed::StandbyInhibit);
    if (!MetalioAudio_WaitModeReady(12000)) return "BH_NOT_READY";
    auto& uart = SimpleUart::getInstance();
    std::unique_lock<std::recursive_timed_mutex> transaction(uart.commandMutex(), std::defer_lock);
    if (!transaction.try_lock_for(std::chrono::milliseconds(1000))) return "BH_BUSY";
    char command[240], record[200];
    if (request.write) {
        s_stage = "write";
        snprintf(record, sizeof(record), "V1|%d|%s", request.mv, request.panel);
        snprintf(command, sizeof(command), "AT+WDATA=%d,%s\r\n", kSlot, record);
        if (const char* error = Exchange(command, "AT+WDATA=")) return error;
        if (strcmp(s_reply, "AT+WDATA=OK")) return "BH_WRITE_REJECTED";
        s_stage = "flush";
        if (const char* error = Exchange("AT+FLUSHDATA=1\r\n", "AT+FLUSHDATA=", 3000)) return error;
        if (strcmp(s_reply, "AT+FLUSHDATA=OK")) return "BH_FLUSH_REJECTED";
    }
    s_stage = "readback";
    snprintf(command, sizeof(command), "AT+RDATA=%d\r\n", kSlot);
    if (const char* error = Exchange(command, "AT+RDATA=")) return error;
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "AT+RDATA=%d,", kSlot);
    if (strncmp(s_reply, prefix, strlen(prefix))) return "BH_READ_INVALID";
    const char* payload = s_reply + strlen(prefix);
    if (request.write && strcmp(payload, record)) return "BH_VERIFY_MISMATCH";
    if (strncmp(payload, "V1|", 3)) return "BH_RECORD_INVALID";
    const char* separator = strchr(payload + 3, '|');
    if (!separator) return "BH_RECORD_INVALID";
    int mv = 0;
    const auto parsed = std::from_chars(payload + 3, separator, mv);
    if (parsed.ec != std::errc() || parsed.ptr != separator ||
        !ValidPanel(separator + 1)) return "BH_RECORD_INVALID";
    if (!ValidMv(mv)) return "INVALID_VCOM";
    snprintf(record, sizeof(record), "V1|%d|%s", mv, separator + 1);
    if (strcmp(record, payload)) return "BH_RECORD_INVALID";
    s_stage = "apply";
    if (!EpdSetVcomMv(mv)) return "DISPLAY_UNAVAILABLE";
    {
        std::lock_guard<std::mutex> lock(s_state_mutex);
        s_state.vcom_mv = mv;
        snprintf(s_panel, sizeof(s_panel), "%s", separator + 1);
    }
    return nullptr;
}

PanelVcomState PanelVcomGetState() {
    std::lock_guard<std::mutex> lock(s_state_mutex);
    auto state = s_state;
    state.busy = s_busy.load();
    return state;
}

static bool Enqueue(const Request& request) {
    bool idle = false;
    if (!s_queue || !s_busy.compare_exchange_strong(idle, true)) return false;
    if (xQueueSend(s_queue, &request, 0) == pdTRUE) return true;
    s_busy.store(false);
    ESP_LOGE(TAG, "operation enqueue failed");
    return false;
}

bool PanelVcomRequestRead() {
    return Enqueue(Request{});
}

static void Reply(uint32_t id, const char* code, const char* stage) {
    const auto state = PanelVcomGetState();
    uint8_t mac[6]{};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char output[704];
    const int length = snprintf(output, sizeof(output),
        "\r\n@VCOM {\"id\":%lu,\"ok\":%s,\"code\":\"%s\",\"stage\":\"%s\","
        "\"vcom_mv\":%d,\"applied_mv\":%d,\"panel_id\":\"%s\",\"local_saved\":%s,\"board_model\":\"%s\","
        "\"device_mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\"}\r\n",
        static_cast<unsigned long>(id), strcmp(code, "OK") == 0 ? "true" : "false", code, stage,
        state.vcom_mv, EpdGetVcomMv(), s_panel, state.ok && state.vcom_mv ? "true" : "false", BOARD_TYPE, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (length > 0 && static_cast<size_t>(length) < sizeof(output) && s_usb) {
        // 与标准日志共用输出锁，等待整帧离开 FIFO 后再允许日志输出。
        flockfile(stdout);
        const int written = usb_serial_jtag_write_bytes(output, length, pdMS_TO_TICKS(1000));
        const esp_err_t sent = usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(1000));
        funlockfile(stdout);
        if (written != length || sent != ESP_OK)
            ESP_LOGW(TAG, "tool response incomplete id=%lu", static_cast<unsigned long>(id));
    }
}

static void HandleLine(const char* line) {
    if (strncmp(line, "@VCOM ", 6)) return;
    // 请求字段均为标量，拒绝嵌套，限制外部输入引起的解析栈占用。
    bool quoted = false, escaped = false;
    int depth = 0;
    for (const char* p = line + 6; *p; ++p) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (*p == '\\') escaped = true;
            else if (*p == '"') quoted = false;
        } else if (*p == '"') quoted = true;
        else if (*p == '[' || (*p == '{' && ++depth > 1)) {
            Reply(0, "INVALID_REQUEST", "input");
            return;
        } else if (*p == '}') --depth;
    }
    cJSON* root = cJSON_ParseWithOpts(line + 6, nullptr, true);
    const auto* id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const auto* cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsNumber(id) || id->valuedouble < 1 || id->valuedouble > 2147483647 ||
        floor(id->valuedouble) != id->valuedouble || !cJSON_IsString(cmd)) {
        cJSON_Delete(root);
        Reply(0, "INVALID_REQUEST", "input");
        return;
    }
    Request request{false, 0, static_cast<uint32_t>(id->valuedouble), {}};
    const char* error = nullptr;
    if (!strcmp(cmd->valuestring, "panel.set")) {
        request.write = true;
        const auto* mv = cJSON_GetObjectItemCaseSensitive(root, "vcom_mv");
        const auto* panel = cJSON_GetObjectItemCaseSensitive(root, "panel_id");
        if (!cJSON_IsNumber(mv) || mv->valuedouble < -EPD_VCOM_MAX_MV || mv->valuedouble > -EPD_VCOM_MIN_MV ||
            floor(mv->valuedouble) != mv->valuedouble || !ValidMv(mv->valueint)) error = "INVALID_VCOM";
        else if (!cJSON_IsString(panel) || !ValidPanel(panel->valuestring)) error = "INVALID_VALUE";
        else {
            request.mv = mv->valueint;
            snprintf(request.panel, sizeof(request.panel), "%s", panel->valuestring);
        }
    } else if (strcmp(cmd->valuestring, "panel.get")) error = "UNKNOWN_COMMAND";
    cJSON_Delete(root);
    if (error) {
        NotifyResult(request.write, error);
        Reply(request.id, error, "input");
    }
    else if (!Enqueue(request)) Reply(request.id, "BUSY", "device");
}

static void RecordResult(const char* error) {
    if (error && !PanelVcomGetState().vcom_mv && !EpdSetBlackDepth(EpdGetBlackDepth())) {
        ESP_LOGE(TAG, "black depth fallback unavailable");
    }
    std::lock_guard<std::mutex> lock(s_state_mutex);
    s_state.ok = !error;
    snprintf(s_state.code, sizeof(s_state.code), "%s", error ? error : "OK");
    ++s_state.revision;
}

static void Worker(void*) {
    // 等待启动复位与模式初始化完成后读取；此时不依赖任何页面。
    Request boot{};
    const char* error = Execute(boot);
    RecordResult(error);
    if (error) ESP_LOGW(TAG, "boot read unavailable code=%s; black depth fallback selected", error);
    s_busy.store(false);
    char input[384]{};
    size_t length = 0;
    bool overflow = false;
    for (;;) {
        Request request{};
        if (xQueueReceive(s_queue, &request, 0) == pdTRUE) {
            error = Execute(request);
            RecordResult(error);
            if (error) ESP_LOGE(TAG, "operation=%s stage=%s code=%s",
                request.write ? "write" : "read", s_stage, error);
            else ESP_LOGI(TAG, "operation=%s complete", request.write ? "write" : "read");
            s_busy.store(false);
            NotifyResult(request.write, error);
            if (request.id) Reply(request.id, error ? error : "OK", error ? s_stage : "done");
        }
        uint8_t bytes[64];
        const int count = s_usb ? usb_serial_jtag_read_bytes(bytes, sizeof(bytes), pdMS_TO_TICKS(50)) : 0;
        if (!s_usb) vTaskDelay(pdMS_TO_TICKS(50));
        for (int i = 0; i < count; ++i) {
            if (bytes[i] == '\r' || bytes[i] == '\n') {
                if (overflow) Reply(0, "LINE_TOO_LONG", "input");
                else if (length) { input[length] = 0; HandleLine(input); }
                length = 0; overflow = false;
            } else if (length + 1 < sizeof(input)) input[length++] = static_cast<char>(bytes[i]);
            else overflow = true;
        }
    }
}

void PanelVcomInit() {
    s_queue = xQueueCreate(1, sizeof(Request));
    if (!s_queue) {
        ESP_LOGE(TAG, "request queue allocation failed");
        RecordResult("SERVICE_UNAVAILABLE");
        return;
    }
    usb_serial_jtag_driver_config_t config = {.tx_buffer_size = 1024, .rx_buffer_size = 1024};
    const esp_err_t err = usb_serial_jtag_driver_install(&config);
    s_usb = err == ESP_OK;
    if (!s_usb) ESP_LOGE(TAG, "USB service unavailable err=%d", static_cast<int>(err));
    s_busy.store(true);
    if (xTaskCreatePinnedToCoreWithCaps(Worker, "panel_vcom", 6144, nullptr, 4, nullptr, 0,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_busy.store(false);
        vQueueDelete(s_queue); s_queue = nullptr;
        if (s_usb) usb_serial_jtag_driver_uninstall();
        s_usb = false;
        ESP_LOGE(TAG, "service task creation failed");
        RecordResult("SERVICE_UNAVAILABLE");
    }
}
