/**
 * @file metalio_audio.cc
 * @brief S31 蓝牙音频：UART AT 模式1 + PA_EN（I2S 在 Board::GetAudioCodec）
 */

#include "metalio_audio.h"

#include "audio_codec.h"
#include "board.h"
#include "config.h"
#include "epd_board_metalio_eink4_plus.h"
#include "simple_uart.hpp"

#include <atomic>
#include <cstring>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <vector>

#define TAG "MetalioAudio"

static std::atomic<bool> s_mode_ready{false};
static std::atomic<bool> s_bt_rx_seen{false};
static bool s_codec_started = false;

/** @brief 对齐 397 BluetoothScreen::ApplyDefaultMode：阻塞下发模式1 */
static void ApplyDefaultModeBlocking() {
    SimpleUart& uart = SimpleUart::getInstance();
    ESP_LOGI(TAG, "TX: AT+RX=2");
    uart.sendString("AT+RX=2\r\n");
    vTaskDelay(pdMS_TO_TICKS(700));
    ESP_LOGI(TAG, "TX: AT+MODE=1");
    uart.sendString("AT+MODE=1\r\n");
    vTaskDelay(pdMS_TO_TICKS(500));
    if (!s_bt_rx_seen.load()) {
        ESP_LOGW(TAG, "no BT RX after AT — check UART TX/RX swap (now TX=%d RX=%d)",
                 BT_AUDIO_TX_PIN, BT_AUDIO_RX_PIN);
    }
    s_mode_ready.store(true);
    ESP_LOGI(TAG, "BT mode1 applied (rx_seen=%d)", s_bt_rx_seen.load() ? 1 : 0);
}

AudioCodec* MetalioAudio_GetCodec() {
    return Board::GetInstance().GetAudioCodec();
}

void MetalioAudio_Init() {
    epd_board_metalio_eink4_plus_pa_set(false);

    SimpleUart& uart = SimpleUart::getInstance();
    if (!uart.begin(BT_AUDIO_TX_PIN, BT_AUDIO_RX_PIN, 115200, UART_NUM_1)) {
        ESP_LOGE(TAG, "BT UART fail TX=%d RX=%d", BT_AUDIO_TX_PIN, BT_AUDIO_RX_PIN);
        return;
    }
    ESP_LOGI(TAG, "BT UART ready TX=%d RX=%d", BT_AUDIO_TX_PIN, BT_AUDIO_RX_PIN);

    uart.registerCallback([](const std::vector<uint8_t>& data) {
        s_bt_rx_seen.store(true);
        std::string line;
        line.reserve(data.size());
        for (uint8_t b : data) {
            if (b == '\r' || b == '\n') {
                line.push_back(' ');
            } else if (b >= 0x20 && b < 0x7F) {
                line.push_back(static_cast<char>(b));
            } else {
                line.push_back('.');
            }
        }
        ESP_LOGI(TAG, "BT RX (%u): %s", static_cast<unsigned>(data.size()), line.c_str());
    });

    s_mode_ready.store(false);
    ApplyDefaultModeBlocking();
}

bool MetalioAudio_WaitModeReady(int timeout_ms) {
    if (s_mode_ready.load()) {
        return true;
    }
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }
    constexpr int kStepMs = 50;
    int waited = 0;
    while (!s_mode_ready.load()) {
        if (waited >= timeout_ms) {
            ESP_LOGW(TAG, "BT mode1 not ready after %dms", timeout_ms);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(kStepMs));
        waited += kStepMs;
    }
    return true;
}

bool MetalioAudio_EnsureStarted() {
    AudioCodec* codec = MetalioAudio_GetCodec();
    if (codec == nullptr) {
        return false;
    }
    if (!MetalioAudio_WaitModeReady(2000)) {
        return false;
    }
    if (!s_codec_started) {
        codec->SetOutputVolume(80);
        codec->Start();
        s_codec_started = true;
        ESP_LOGI(TAG, "codec started");
    }
    return true;
}

bool MetalioAudio_ModeReady() {
    return s_mode_ready.load();
}

bool MetalioAudio_BtRxSeen() {
    return s_bt_rx_seen.load();
}

void MetalioAudio_SetPa(bool on) {
    epd_board_metalio_eink4_plus_pa_set(on);
}

int MetalioAudio_InputRate() {
    AudioCodec* codec = MetalioAudio_GetCodec();
    return codec != nullptr ? codec->input_sample_rate() : AUDIO_INPUT_SAMPLE_RATE;
}

int MetalioAudio_ReadMic(int16_t* dest, int samples) {
    if (dest == nullptr || samples <= 0 || !MetalioAudio_EnsureStarted()) {
        return -1;
    }
    AudioCodec* codec = MetalioAudio_GetCodec();
    std::vector<int16_t> buf(static_cast<size_t>(samples));
    if (!codec->InputData(buf)) {
        return 0;
    }
    int got = static_cast<int>(buf.size());
    std::memcpy(dest, buf.data(), static_cast<size_t>(got) * sizeof(int16_t));
    return got;
}

int MetalioAudio_WriteSpk(const int16_t* data, int samples) {
    if (data == nullptr || samples <= 0 || !MetalioAudio_EnsureStarted()) {
        return -1;
    }
    epd_board_metalio_eink4_plus_pa_set(true);
    AudioCodec* codec = MetalioAudio_GetCodec();
    std::vector<int16_t> buf(data, data + samples);
    codec->OutputData(buf);
    return samples;
}
