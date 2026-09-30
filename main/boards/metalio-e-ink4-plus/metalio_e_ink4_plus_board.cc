/**
 * @file metalio_e_ink4_plus_board.cc
 * @brief Metalio E-Ink4-Plus：WifiBoard + BH1098 音频 + EPDiy
 */

#include "wifi_board.h"

#include "bt_audio_codec.h"
#include "bq27220_gauge.h"
#include "config.h"
#include "frontlight.h"
#include "metalio_accel.h"
#include "metalio_audio.h"
#include "metalio_display_setup.h"
#include "metalio_epd.h"
#include "metalio_keys.h"
#include "metalio_power.h"
#include "metalio_rtc.h"
#include "metalio_sd.h"
#include "metalio_sys_mon.h"
#include "metalio_touch.h"
#include "metalio_vibe.h"
#include "power_policy.h"
#include "settings.h"

#include <esp_log.h>

#define TAG "MetalioEInk4Plus"

namespace {

FrontlightCct NormalizeFrontlightCct(int value) {
    if (value < static_cast<int>(FrontlightCct::kWarm) ||
        value > static_cast<int>(FrontlightCct::kOff)) {
        return FrontlightCct::kOff;
    }
    return static_cast<FrontlightCct>(value);
}

void ApplySavedFrontlight() {
    auto& fl = Frontlight::GetInstance();
    Settings settings("frontlight", false);
    const FrontlightCct cct =
        NormalizeFrontlightCct(static_cast<int>(settings.GetInt("cct", static_cast<int>(FrontlightCct::kOff))));
    fl.SetCct(cct);
    fl.RestoreBrightness();
    ESP_LOGI(TAG, "frontlight cct=%d brightness=%u", static_cast<int>(cct), fl.target_brightness());
}

} // namespace

class MetalioEInk4PlusBoard : public WifiBoard {
public:
    MetalioEInk4PlusBoard() {
        if (!MetalioEpd_Init()) {
            ESP_LOGE(TAG, "epd init fail");
            return;
        }
        MetalioPower_Init();
        // 前光尽早：勿等 SD/音频；否则首帧已出仍一片暗
        auto& fl = Frontlight::GetInstance();
        fl.Init(FL_COOL_PIN, FL_WARM_PIN);
        ApplySavedFrontlight();

        MetalioRtc_Init();
        MetalioAccel_Init();
        metalio_touch_init(); // 先于 Display：INT+feed 就绪后再挂 LVGL 回调
        display_ = MetalioDisplay_Create();

        MetalioVibe_Init();
        MetalioSd_Init();
        MetalioKeys_Init();
        MetalioAudio_Init(); // UART + AT 模式1（I2S 在 GetAudioCodec）

        MetalioPeriph_StartSystemMonitor();
        PowerPolicy::GetInstance().Init(this);

        if (display_ != nullptr) {
            MetalioDisplay_SetSystemReady(display_);
        }
        ESP_LOGI(TAG, "board ready display=%s sd=%s gauge=%s",
                 display_ ? "ok" : "FAIL", MetalioSd_Ok() ? "ok" : "FAIL",
                 Bq27220Gauge::GetInstance().IsReady() ? "ok" : "N/A");
    }

    std::string GetBoardType() override {
        return "metalio-e-ink4-plus";
    }

    Display* GetDisplay() override {
        return display_;
    }

    /** @brief 首次调用时创建 duplex I2S */
    AudioCodec* GetAudioCodec() override {
        static BTAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                              AUDIO_I2S_BCLK, AUDIO_I2S_WS, AUDIO_I2S_DOUT,
                                              AUDIO_I2S_DIN);
        return &audio_codec;
    }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        return Bq27220Gauge::GetInstance().GetBatteryLevel(level, charging, discharging);
    }

    void PulseVibration() override {
        MetalioVibe_Pulse();
    }

    void SetVibration(bool on) override {
        MetalioVibe_Set(on);
    }

    esp_err_t TouchEnterSleep() override {
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t TouchWakeByReset() override {
        return ESP_ERR_NOT_SUPPORTED;
    }

private:
    Display* display_ = nullptr;
};

DECLARE_BOARD(MetalioEInk4PlusBoard);
