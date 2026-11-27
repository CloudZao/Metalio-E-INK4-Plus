#include <chrono>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "panel_calibration_storage.h"
#include "boards/common/audio/bluetooth/bluetooth_audio.h"
#include "boards/common/audio/bluetooth/simple_uart.hpp"
#include "config.h"

static constexpr int kSlot = 470;  // 保留既有蓝牙芯片校准记录槽，不随板型名称改变
static std::mutex s_rx_mutex;
static char s_line[256]{};
static char s_expected[48]{};
static char s_reply[256]{};
static size_t s_length = 0;
static bool s_overflow = false;
static bool s_waiting = false;
static bool s_received = false;

static PanelCalibrationStorageResult Failure(PanelCalibrationStorageResult result, const char* error) {
    result.error = error;
    return result;
}

bool PanelCalibrationStorageValidMv(int mv) {
    // 按板级校准范围校验负毫伏值，保留驱动所需的 10mV 步进。
    return mv <= -EPD_VCOM_MIN_MV && mv >= -EPD_VCOM_MAX_MV && mv % 10 == 0;
}

bool PanelCalibrationStorageValidId(const char* text) {
    if (!text || !*text || strlen(text) >= kPanelCalibrationStorageIdCapacity) {
        return false;
    }
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' ||
              *p == '_')) {
            return false;
        }
    }
    return true;
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
            if (s_received) {
                return nullptr;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    std::lock_guard<std::mutex> lock(s_rx_mutex);
    s_waiting = false;
    return "BH_TIMEOUT";
}

static PanelCalibrationStorageResult Execute(bool write, int mv, const char* panel) {
    PanelCalibrationStorageResult result;
    result.stage = "input";
    if (write && !PanelCalibrationStorageValidMv(mv)) {
        return Failure(result, "INVALID_VCOM");
    }
    result.stage = "ready";
    if (!BluetoothAudio_WaitModeReady(12000)) {
        return Failure(result, "BH_NOT_READY");
    }
    auto& uart = SimpleUart::getInstance();
    std::unique_lock<std::recursive_timed_mutex> transaction(uart.commandMutex(), std::defer_lock);
    if (!transaction.try_lock_for(std::chrono::milliseconds(1000))) {
        return Failure(result, "BH_BUSY");
    }
    char command[240], record[200];
    if (write) {
        result.stage = "write";
        snprintf(record, sizeof(record), "V1|%d|%s", mv, panel);
        snprintf(command, sizeof(command), "AT+WDATA=%d,%s\r\n", kSlot, record);
        if (const char* error = Exchange(command, "AT+WDATA=")) {
            return Failure(result, error);
        }
        if (strcmp(s_reply, "AT+WDATA=OK")) {
            return Failure(result, "BH_WRITE_REJECTED");
        }
        result.stage = "flush";
        if (const char* error = Exchange("AT+FLUSHDATA=1\r\n", "AT+FLUSHDATA=", 3000)) {
            return Failure(result, error);
        }
        if (strcmp(s_reply, "AT+FLUSHDATA=OK")) {
            return Failure(result, "BH_FLUSH_REJECTED");
        }
    }
    result.stage = "readback";
    snprintf(command, sizeof(command), "AT+RDATA=%d\r\n", kSlot);
    if (const char* error = Exchange(command, "AT+RDATA=")) {
        return Failure(result, error);
    }
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "AT+RDATA=%d,", kSlot);
    if (strncmp(s_reply, prefix, strlen(prefix))) {
        return Failure(result, "BH_READ_INVALID");
    }
    const char* payload = s_reply + strlen(prefix);
    if (write && strcmp(payload, record)) {
        return Failure(result, "BH_VERIFY_MISMATCH");
    }
    if (strncmp(payload, "V1|", 3)) {
        return Failure(result, "BH_RECORD_INVALID");
    }
    const char* separator = strchr(payload + 3, '|');
    if (!separator) {
        return Failure(result, "BH_RECORD_INVALID");
    }
    int read_mv = 0;
    const auto parsed = std::from_chars(payload + 3, separator, read_mv);
    if (parsed.ec != std::errc() || parsed.ptr != separator || !PanelCalibrationStorageValidId(separator + 1)) {
        return Failure(result, "BH_RECORD_INVALID");
    }
    if (!PanelCalibrationStorageValidMv(read_mv)) {
        return Failure(result, "INVALID_VCOM");
    }
    snprintf(record, sizeof(record), "V1|%d|%s", read_mv, separator + 1);
    if (strcmp(record, payload)) {
        return Failure(result, "BH_RECORD_INVALID");
    }
    result.vcom_mv = read_mv;
    snprintf(result.panel, sizeof(result.panel), "%s", separator + 1);
    return result;
}

PanelCalibrationStorageResult PanelCalibrationStorageRead() {
    return Execute(false, 0, "");
}

PanelCalibrationStorageResult PanelCalibrationStorageWrite(int mv, const char* panel) {
    if (!PanelCalibrationStorageValidId(panel)) {
        PanelCalibrationStorageResult result;
        result.stage = "input";
        return Failure(result, "INVALID_VALUE");
    }
    return Execute(true, mv, panel);
}

void PanelCalibrationStorageOnUartData(const std::vector<uint8_t>& data) {
    if (!BOARD_HAS_VCOM) {
        return;
    }
    std::lock_guard<std::mutex> lock(s_rx_mutex);
    for (uint8_t c : data) {
        if (c == '\r' || c == '\n') {
            s_line[s_length] = 0;
            if (s_waiting && !s_overflow &&
                (strncmp(s_line, s_expected, strlen(s_expected)) == 0 || strcmp(s_line, "ERROR") == 0 ||
                 strcmp(s_line, "FAIL") == 0)) {
                snprintf(s_reply, sizeof(s_reply), "%s", s_line);
                s_received = true;
                s_waiting = false;
            }
            s_length = 0;
            s_overflow = false;
        } else if (c >= 32 && c < 127) {
            if (s_length + 1 < sizeof(s_line)) {
                s_line[s_length++] = static_cast<char>(c);
            } else {
                s_overflow = true;
            }
        } else {
            s_overflow = true;
        }
    }
}
