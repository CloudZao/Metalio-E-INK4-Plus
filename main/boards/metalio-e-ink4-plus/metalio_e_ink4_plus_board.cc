/**
 * @file metalio_e_ink4_plus_board.cc
 * @brief Metalio E-Ink4-Plus：DualNetworkBoard + 蓝牙芯片音频 + EPDiy
 */

#include <esp_log.h>
#include "boards/common/audio/bluetooth/bt_audio_codec.h"
#include "boards/common/audio/bluetooth/bluetooth_audio.h"
#include "boards/common/board/dual_network_board.h"
#include "boards/common/bus/i2c/shared_i2c_bus.h"
#include "boards/common/camera/dvp/dvp_camera.h"
#include "boards/common/camera/dvp/dvp_camera_adapter.h"
#include "boards/common/display/epdiy/panel/epd_board_adapter.h"
#include "boards/common/display/frontlight.h"
#include "boards/common/haptics/vibration.h"
#include "boards/common/input/buttons/io_expander/expander_button_input.h"
#include "boards/common/input/touch/ft6336u/touch_input.h"
#include "boards/common/input/touch/ft6336u/touch_firmware.h"
#include "boards/common/monitor/system_monitor.h"
#include "boards/common/power/bq27220/bq27220_gauge.h"
#include "boards/common/power/cx25601n/charger_control.h"
#include "boards/common/sensor/sc7a20h/accelerometer.h"
#include "boards/common/storage/sdmmc/sdmmc_storage.h"
#include "boards/common/storage/wifi_config_storage.h"
#include "business/wallpaper/wallpaper_active.h"
#include "config.h"
#include "infra/storage/settings.h"
#include "infra/time/time_service.h"
#include "ui/common/drawing/adapter/epdiy/display_setup.h"
#include "epd_panel.h"
#include "business/diagnostics/panel_vcom.h"
#include "boards/common/display/panel_calibration_storage.h"
#include "ui/common/drawing/adapter/epd_refresh.h"
#include "ui/common/drawing/rendering/panel_vcom_notice.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "boards/common/rtc/pcf8563/rtc_service.h"

#define TAG "MetalioEInk4Plus"

static void SubscribeAccelIrq() {
    epd_board_adapter_gpio2_irq_subscribe(xTaskGetCurrentTaskHandle());
}

// 档位不变时仍打开备用电压，供校准记录缺失后使用。
static bool EnableSavedBlackDepth() {
    return EpdSetBlackDepth(EpdGetBlackDepth());
}

static FrontlightCct NormalizeFrontlightCct(int value) {
    if (value < static_cast<int>(FrontlightCct::kWarm) || value > static_cast<int>(FrontlightCct::kOff)) {
        return FrontlightCct::kOff;
    }
    return static_cast<FrontlightCct>(value);
}

static void ApplySavedFrontlight() {
    auto& fl = Frontlight::GetInstance();
    Settings settings("frontlight", false);
    const FrontlightCct cct =
        NormalizeFrontlightCct(static_cast<int>(settings.GetInt("cct", static_cast<int>(FrontlightCct::kOff))));
    fl.SetCct(cct);
    fl.RestoreBrightness();
    ESP_LOGI(TAG, "frontlight cct=%d brightness=%u", static_cast<int>(cct), fl.target_brightness());
}

class MetalioEInk4PlusBoard : public DualNetworkBoard {
public:
    MetalioEInk4PlusBoard() : DualNetworkBoard(NT26_TX_PIN, NT26_RX_PIN, NT26_MRDY_PIN, NT26_SRDY_PIN, 0) {
        if (!EpdPanel_Init()) {
            ESP_LOGE(TAG, "epd init fail");
            return;
        }
        if (!TouchFirmware_UpgradeOnBoot()) {
            ESP_LOGE(TAG, "touch upgrade failed; continuing with touch probe");
        }
        const power_i2c_t power_bus = {I2C_NUM_1, shared_i2c_bus_lock, shared_i2c_bus_unlock,
                                       shared_i2c_bus_device_locked};
        Bq27220Gauge::GetInstance().Begin(&power_bus);
        ChargerControl_Init(&power_bus, "charger", "current_ma");
        // 先恢复逻辑设置，首次物理输出等待音频 UART 初始化完成。
        auto& fl = Frontlight::GetInstance();
        fl.Init(FL_COOL_PIN, FL_WARM_PIN);
        fl.SetOutputDeferred(true);
        ApplySavedFrontlight();

        RtcService_Init();
        Accelerometer_Init(&power_bus, SubscribeAccelIrq);
        touch_input_init();  // 先于 Display：INT+feed 就绪后再挂 LVGL 回调
        display_ = DisplaySetup_Create();

        if (epd_board_adapter_ioexp_ok()) {
            if (Vibration_Init({
                    .drive = VibrationDrive::Pwm,
                    .output = VibrationOutput::ExpandedPin,
                    .gpio = GPIO_NUM_NC,
                    .active_high = true,
                    .write_expanded = epd_board_adapter_motor_set,
                    .feedback_pulse_ms = BOARD_VIBRATION_FEEDBACK_PULSE_MS,
                    .pulse_ms = VIBRATION_MOTOR_PULSE_MS,
                }) != ESP_OK) {
                ESP_LOGW(TAG, "vibration unavailable");
            }
        } else {
            ESP_LOGW(TAG, "vibration unavailable: IO expander missing");
        }
        SdmmcStorage_Init();
        WifiConfigStorage_Init();
        // SD 挂载后补齐本地壁纸缓存，必须早于电源键开放待机入口。
        Wallpaper_HydrateFromNvsNow();
        ExpanderButtonInput_Init();

        SystemMonitor_Start();
        PowerPolicy::GetInstance().Init(this);

        if (display_ != nullptr) {
            DisplaySetup_SetSystemReady(display_);
        }
        // 开机保留共享电源状态；先初始化 UART，再由后台配置模式。
        BluetoothAudio_Init({UART_NUM_1, false, epd_board_adapter_pa_set, PanelCalibrationStorageOnUartData}, [] {
            Frontlight::GetInstance().SetOutputDeferred(false);
            ESP_LOGI(TAG, "frontlight startup output released after audio UART initialization");
        });
        PanelVcomInit({EpdSetVcomMv, EpdGetVcomMv, UiNotifyPanelVcom, EnableSavedBlackDepth});
        ESP_LOGI(TAG, "board ready display=%s sd=%s gauge=%s", display_ ? "ok" : "FAIL",
                 SdmmcStorage_Ok() ? "ok" : "FAIL", Bq27220Gauge::GetInstance().IsReady() ? "ok" : "N/A");
    }

    void OnNetworkTimeSynced() override {
        if (!TimeServiceStoreRtc()) {
            ESP_LOGW(TAG, "network time applied but RTC writeback failed");
        }
    }

    Display* GetDisplay() override {
        return display_;
    }

    /** @brief 首次检测成功后返回 DVP 摄像头实例，检测结果保留至重启 */
    Camera* GetCamera() override {
        static Camera* camera = []() -> Camera* {
            DvpCameraDetectInfo info;
            const esp_err_t err = DvpCameraDetect(info, {});
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Camera unavailable; MCP camera tool disabled: %s", esp_err_to_name(err));
                return nullptr;
            }
            static DvpCamera detected_camera;
            return &detected_camera;
        }();
        return camera;
    }

    /** @brief 返回板级音频实例，硬件资源在进入音频功能时按需申请 */
    AudioCodec* GetAudioCodec() override {
        static BTAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_BCLK,
                                              AUDIO_I2S_WS, AUDIO_I2S_DOUT, AUDIO_I2S_DIN);
        return &audio_codec;
    }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        return Bq27220Gauge::GetInstance().GetCachedBatteryLevel(level, charging, discharging);
    }

    void SetVibration(bool on) override {
        Vibration_Set(on);
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
