#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <cJSON.h>
#include <driver/usb_serial_jtag.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "infra/tasks/task_create.h"
#include <esp_mac.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include "panel_vcom.h"
#include "boards/common/display/panel_calibration_storage.h"
#include "config.h"
#include "sysmgmt/power/policy/power_policy.h"

static const char* TAG = "PanelVcom";

struct Request {
    bool write;                                      // 写入或读取
    int mv;                                          // 负毫伏值
    uint32_t id;                                     // 工具请求编号，0 为设备界面操作
    char panel[kPanelCalibrationStorageIdCapacity];  // 屏幕标签标识
};

static QueueHandle_t s_queue;
static std::atomic<bool> s_busy{false};
static std::mutex s_state_mutex;
static PanelVcomBindings s_bindings{};
static PanelVcomState s_state{};
static char s_panel[kPanelCalibrationStorageIdCapacity]{};
static bool s_usb;
static const char* s_stage = "read";

static void NotifyResult(bool write, const char* error) {
    if (s_bindings.notify) {
        s_bindings.notify(write, error);
    }
}

static const char* Execute(const Request& request) {
    PowerNeedHold hold(PowerNeed::StandbyInhibit);
    const auto result =
        request.write ? PanelCalibrationStorageWrite(request.mv, request.panel) : PanelCalibrationStorageRead();
    s_stage = result.stage;
    if (result.error) {
        return result.error;
    }
    s_stage = "apply";
    if (!s_bindings.apply(result.vcom_mv)) {
        return "DISPLAY_UNAVAILABLE";
    }
    {
        std::lock_guard<std::mutex> lock(s_state_mutex);
        s_state.vcom_mv = result.vcom_mv;
        snprintf(s_panel, sizeof(s_panel), "%s", result.panel);
    }
    return nullptr;
}

static void RecordResult(const char* error) {
    bool use_saved_depth = false;
    {
        std::lock_guard<std::mutex> lock(s_state_mutex);
        use_saved_depth = error != nullptr && s_state.vcom_mv == 0;
    }
    if (use_saved_depth && !s_bindings.enable_saved_depth()) {
        ESP_LOGE(TAG, "black depth fallback unavailable");
    }
    std::lock_guard<std::mutex> lock(s_state_mutex);
    s_state.ok = error == nullptr;
    snprintf(s_state.code, sizeof(s_state.code), "%s", error ? error : "OK");
    ++s_state.revision;
}

static bool Enqueue(const Request& request) {
    bool idle = false;
    if (!s_queue || !s_busy.compare_exchange_strong(idle, true)) {
        return false;
    }
    if (xQueueSend(s_queue, &request, 0) == pdTRUE) {
        return true;
    }
    s_busy.store(false);
    ESP_LOGE(TAG, "operation enqueue failed");
    return false;
}

static void Reply(uint32_t id, const char* code, const char* stage) {
    const auto state = PanelVcomGetState();
    uint8_t mac[6]{};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char output[704];
    const int length =
        snprintf(output, sizeof(output),
                 "\r\n@VCOM {\"id\":%lu,\"ok\":%s,\"code\":\"%s\",\"stage\":\"%s\","
                 "\"vcom_mv\":%d,\"applied_mv\":%d,\"panel_id\":\"%s\",\"local_saved\":%s,\"board_model\":\"%s\","
                 "\"device_mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\"}\r\n",
                 static_cast<unsigned long>(id), strcmp(code, "OK") == 0 ? "true" : "false", code, stage, state.vcom_mv,
                 s_bindings.current(), s_panel, state.ok && state.vcom_mv ? "true" : "false", BOARD_TYPE, mac[0],
                 mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (length > 0 && static_cast<size_t>(length) < sizeof(output) && s_usb) {
        // 与标准日志共用输出锁，等待整帧离开 FIFO 后再允许日志输出。
        flockfile(stdout);
        const int written = usb_serial_jtag_write_bytes(output, length, pdMS_TO_TICKS(1000));
        const esp_err_t sent = usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(1000));
        funlockfile(stdout);
        if (written != length || sent != ESP_OK) {
            ESP_LOGW(TAG, "tool response incomplete id=%lu", static_cast<unsigned long>(id));
        }
    }
}

static void HandleLine(const char* line) {
    if (strncmp(line, "@VCOM ", 6)) {
        return;
    }
    // 请求字段均为标量，拒绝嵌套，限制外部输入引起的解析栈占用。
    bool quoted = false, escaped = false;
    int depth = 0;
    for (const char* p = line + 6; *p; ++p) {
        if (quoted) {
            if (escaped) {
                escaped = false;
            } else if (*p == '\\') {
                escaped = true;
            } else if (*p == '"') {
                quoted = false;
            }
        } else if (*p == '"') {
            quoted = true;
        } else if (*p == '[' || (*p == '{' && ++depth > 1)) {
            Reply(0, "INVALID_REQUEST", "input");
            return;
        } else if (*p == '}') {
            --depth;
        }
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
            floor(mv->valuedouble) != mv->valuedouble || !PanelCalibrationStorageValidMv(mv->valueint)) {
            error = "INVALID_VCOM";
        } else if (!cJSON_IsString(panel) || !PanelCalibrationStorageValidId(panel->valuestring)) {
            error = "INVALID_VALUE";
        } else {
            request.mv = mv->valueint;
            snprintf(request.panel, sizeof(request.panel), "%s", panel->valuestring);
        }
    } else if (strcmp(cmd->valuestring, "panel.get")) {
        error = "UNKNOWN_COMMAND";
    }
    cJSON_Delete(root);
    if (error) {
        NotifyResult(request.write, error);
        Reply(request.id, error, "input");
    } else if (!Enqueue(request)) {
        Reply(request.id, "BUSY", "device");
    }
}

static void Worker(void*) {
    // 等待启动复位与模式初始化完成后读取；此时不依赖任何页面。
    Request boot{};
    const char* error = Execute(boot);
    RecordResult(error);
    if (error) {
        ESP_LOGW(TAG, "boot read unavailable code=%s; black depth fallback selected", error);
    }
    s_busy.store(false);
    char input[384]{};
    size_t length = 0;
    bool overflow = false;
    for (;;) {
        Request request{};
        if (xQueueReceive(s_queue, &request, 0) == pdTRUE) {
            error = Execute(request);
            RecordResult(error);
            if (error) {
                ESP_LOGE(TAG, "operation=%s stage=%s code=%s", request.write ? "write" : "read", s_stage, error);
            } else {
                ESP_LOGI(TAG, "operation=%s complete", request.write ? "write" : "read");
            }
            s_busy.store(false);
            NotifyResult(request.write, error);
            if (request.id) {
                Reply(request.id, error ? error : "OK", error ? s_stage : "done");
            }
        }
        uint8_t bytes[64];
        const int count = s_usb ? usb_serial_jtag_read_bytes(bytes, sizeof(bytes), pdMS_TO_TICKS(50)) : 0;
        if (!s_usb) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        for (int i = 0; i < count; ++i) {
            if (bytes[i] == '\r' || bytes[i] == '\n') {
                if (overflow) {
                    Reply(0, "LINE_TOO_LONG", "input");
                } else if (length) {
                    input[length] = 0;
                    HandleLine(input);
                }
                length = 0;
                overflow = false;
            } else if (length + 1 < sizeof(input)) {
                input[length++] = static_cast<char>(bytes[i]);
            } else {
                overflow = true;
            }
        }
    }
}

void PanelVcomInit(PanelVcomBindings bindings) {
    if (!bindings.apply || !bindings.current || !bindings.enable_saved_depth) {
        ESP_LOGE(TAG, "operation=init stage=display_binding result=invalid");
        return;
    }
    s_bindings = bindings;
    if (!BOARD_HAS_VCOM) {
        return;
    }
    s_queue = xQueueCreate(1, sizeof(Request));
    if (!s_queue) {
        ESP_LOGE(TAG, "request queue allocation failed");
        RecordResult("SERVICE_UNAVAILABLE");
        return;
    }
    usb_serial_jtag_driver_config_t config = {.tx_buffer_size = 1024, .rx_buffer_size = 1024};
    const esp_err_t err = usb_serial_jtag_driver_install(&config);
    s_usb = err == ESP_OK;
    if (!s_usb) {
        ESP_LOGE(TAG, "USB service unavailable err=%d", static_cast<int>(err));
    }
    s_busy.store(true);
    if (FirmwareCreateTask(Worker, "panel_vcom", 6144, nullptr, 4, nullptr, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_busy.store(false);
        vQueueDelete(s_queue);
        s_queue = nullptr;
        if (s_usb) {
            usb_serial_jtag_driver_uninstall();
        }
        s_usb = false;
        ESP_LOGE(TAG, "service task creation failed");
        RecordResult("SERVICE_UNAVAILABLE");
    }
}

PanelVcomState PanelVcomGetState() {
    std::lock_guard<std::mutex> lock(s_state_mutex);
    auto state = s_state;
    state.busy = s_busy.load();
    return state;
}

bool PanelVcomRequestRead() {
    return Enqueue(Request{});
}
