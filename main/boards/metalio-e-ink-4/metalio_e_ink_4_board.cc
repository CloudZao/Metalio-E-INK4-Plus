#include <memory>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_touch.h>
#include <esp_log.h>
#include "assets/lang_config.h"
#include "boards/common/audio/bluetooth/bt_audio_codec.h"
#include "boards/common/audio/bluetooth/bluetooth_audio.h"
#include "boards/common/board/dual_network_board.h"
#include "boards/common/bus/i2c/peripheral_i2c_bus.h"
#include "boards/common/camera/uvc/uvc_still_camera.h"
#include "boards/common/haptics/vibration.h"
#include "boards/common/input/buttons/gpio/gpio_button_input.h"
#include "boards/common/input/touch/cst816s/touch_input.h"
#include "boards/common/io/IOExpander.hpp"
#include "boards/common/io/tca95xx/io_expander_setup.h"
#include "boards/common/monitor/system_monitor.h"
#include "boards/common/power/bq27220/bq27220_gauge.h"
#include "boards/common/power/cx25601n/charger_control.h"
#include "boards/common/power/power_hw.h"
#include "boards/common/rtc/pcf8563/pcf8563.h"
#include "boards/common/sensor/sc7a20h/accelerometer.h"
#include "boards/common/storage/SdCardManager.hpp"
#include "boards/common/storage/usb_virtual_disk.h"
#include "config.h"
#include "ui/common/drawing/adapter/ssd1677/display_setup.h"
#include "epd_panel.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "infra/ota/resources_upgrade.h"
#include "infra/storage/sd_paths.h"
#include "infra/time/time_service.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"

#define TAG "MetalioEInk4Board"

static void InitializeBoardRtc(const power_i2c_t* bus) {
    auto& rtc = Pcf8563::GetInstance();
    if (!rtc.Begin(bus)) {
        ESP_LOGW("RtcBoot", "PCF8563 init skipped");
        return;
    }
    if (TimeServiceRestoreRtc()) {
        ESP_LOGD("RtcBoot", "boot time source: PCF8563 (offline / pre-network)");
    } else {
        ESP_LOGW("RtcBoot", "PCF8563 present but ApplyRtcToSystem failed");
    }
}

static void InitializeBoardStorage() {
    // microSD pin2 = CD/DAT3：1-bit 模式下不走数据线，但必须为高，否则卡易进 SPI。
    // ESP32-S3 GPIO46 仅输入，无法推挽拉高，用内部上拉（板级有外拉更好）。
    gpio_config_t dat3_cfg = {
        .pin_bit_mask = BIT64(SDMMC_DAT3_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t dat3_ret = gpio_config(&dat3_cfg);
    if (dat3_ret != ESP_OK) {
        ESP_LOGW("SdBoot", "SD DAT3/CD GPIO%d pull-up config failed: %s", static_cast<int>(SDMMC_DAT3_PIN),
                 esp_err_to_name(dat3_ret));
    } else {
        ESP_LOGD("SdBoot", "SD DAT3/CD GPIO%d input+pullup (idle high)", static_cast<int>(SDMMC_DAT3_PIN));
    }

    if (!SdCardManager::GetInstance().Mount()) {
        ESP_LOGW("SdBoot", "SD card not mounted at boot (card may be absent)");
    } else {
        SdEnsureUploadReadme();
    }
    // 默认保持 USB Serial/JTAG，设置页启用时再创建任务并切 MSC。
    UsbVirtualDisk::GetInstance().Init();
}

// 关机：POWER 长按满 kPowerLongPressMs / 策略超时 → PowerPolicy::RequestPowerOff()
class MetalioEInk4Board : public DualNetworkBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;     // I2C 总线
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;  // 墨水屏 panel IO
    esp_lcd_panel_handle_t panel_ = nullptr;        // 墨水屏 panel
    esp_lcd_touch_handle_t touch_ = nullptr;        // 触摸句柄（可空）
    Display* display_ = nullptr;                    // LVAdapterDisplay
    UvcStillCamera* camera_ = nullptr;              // UVC 拍照

    Button boot_button_;                          // BOOT
    Button power_button_;                         // 电源键
    std::unique_ptr<Button> volume_up_button_;    // 音量+
    std::unique_ptr<Button> volume_down_button_;  // 音量-
    PowerKeyGestureState power_key_;              // 开机残留按住：屏蔽/吞 Click

public:
    MetalioEInk4Board()
        : DualNetworkBoard(NT26_TX_PIN, NT26_RX_PIN, NT26_MRDY_PIN, NT26_SRDY_PIN, 0),
          boot_button_(BOOT_BUTTON_GPIO, false, kBootLongPressMs),
          power_button_(POWER_BUTTON_GPIO, false, kPowerLongPressMs) {
        PeripheralI2cBus_Create(&i2c_bus_);
        const esp_err_t mapping = IOExpander::getInstance().setPinMap({
            {IOExpander::Pin::SCREEN_SOCKET_PWR, IOX_PIN_SCREEN_SOCKET_PWR},
            {IOExpander::Pin::MAIN_PWR, IOX_PIN_MAIN_PWR},
            {IOExpander::Pin::PA, IOX_PIN_PA},
            {IOExpander::Pin::PA_SWITCH, IOX_PIN_PA_SWITCH},
            {IOExpander::Pin::VOLUME_DOWN, IOX_PIN_VOLUME_DOWN, IOExpander::Direction::kInput},
            {IOExpander::Pin::VOLUME_UP, IOX_PIN_VOLUME_UP, IOExpander::Direction::kInput},
            {IOExpander::Pin::TOUCH_RST, IOX_PIN_TOUCH_RST},
            {IOExpander::Pin::PWR_KEY_PULSE, IOX_PIN_PWR_KEY_PULSE},
            {IOExpander::Pin::ACCEL_INT, IOX_PIN_ACCEL_INT, IOExpander::Direction::kInput},
            {IOExpander::Pin::USB_MUX_SEL, IOX_PIN_USB_MUX_SEL},
        });
        const bool io_ok = mapping == ESP_OK && IoExpanderSetup_Init(&i2c_bus_);
        if (mapping != ESP_OK) {
            ESP_LOGE(TAG, "operation=init stage=io_mapping code=%d", static_cast<int>(mapping));
        }
        if (Vibration_Init({
                .drive = VibrationDrive::Gpio,
                .output = VibrationOutput::DirectPin,
                .gpio = VIBRATION_MOTOR_GPIO,
                .active_high = true,
                .write_expanded = nullptr,
                .feedback_pulse_ms = BOARD_VIBRATION_FEEDBACK_PULSE_MS,
                .pulse_ms = VIBRATION_MOTOR_PULSE_MS,
            }) != ESP_OK) {
            ESP_LOGW(TAG, "vibration unavailable");
        }
        // BQ27220 挂到 I2C；失败不崩溃，GetBatteryLevel 内会节流自愈重试。
        Bq27220Gauge::GetInstance().Begin(PeripheralI2cBus_PowerBus());
        // SC7A20H 加速度计；未焊接时 probe 失败即可，产测页显示「未检测到」。
        Accelerometer_Init(PeripheralI2cBus_PowerBus(), nullptr);
        InitializeBoardRtc(PeripheralI2cBus_PowerBus());
        ChargerControl_Init(PeripheralI2cBus_PowerBus(), "charge", "ichg_ma");
        BluetoothAudio_Init({UART_NUM_2, false, power_hw_pa_set}, nullptr);
        InitializeBoardStorage();
        EpdPanel_Init(&panel_io_, &panel_);
        touch_ = nullptr;
        const bool touch_ok = (touch_input_init(i2c_bus_, &touch_) == ESP_OK);
        // 显示创建和资源映射前，安装固件附带的资源镜像。
        ResourcesUpgradeApply();
        display_ = DisplaySetup_Create(panel_, panel_io_, touch_);
        if (!touch_ok) {
            ESP_LOGW(TAG, "Touch missing; TouchMissingScreen should be showing");
        }
        if (!io_ok) {
            if (auto* disp = LVAdapterDisplay::Instance()) {
                // 状态栏控件在 HomeScreen Create 时才绑定，先排队
                disp->QueueStatusNotification(LangString_SETTINGS_TEST_FAIL, 60000);
            }
        }
        GpioButtonInput_Init(boot_button_, power_button_, volume_up_button_, volume_down_button_, power_key_);
        camera_ = new UvcStillCamera();
        SystemMonitor_Start();
        PowerPolicy::GetInstance().Init(this);
        if (auto* display = LVAdapterDisplay::Instance()) {
            display->SetSystemReady();
        }
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BTAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                              AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_SPK_GPIO_DOUT,
                                              AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    // 转发到 Bq27220Gauge；通知栏 UpdateStatusBar 会按 level/充电态刷新电池图标。
    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        return Bq27220Gauge::GetInstance().GetCachedBatteryLevel(level, charging, discharging);
    }

    virtual void SetVibration(bool on) override {
        Vibration_Set(on);
    }

    virtual esp_err_t TouchEnterSleep() override {
        return touch_input_enter_sleep(touch_);
    }

    virtual esp_err_t TouchWakeByReset() override {
        auto& io = IOExpander::getInstance();
        if (!io.isInitialized() || io.handle() == nullptr) {
            return ESP_ERR_INVALID_STATE;
        }
        return touch_input_wake_by_reset(io.handle(), TouchInput_AfterWake);
    }

    // 联网且 OTA 写入 server_time 后：系统时间 → RTC → 再读回系统，以芯片为准。
    virtual void OnNetworkTimeSynced() override {
        auto& rtc = Pcf8563::GetInstance();
        if (!rtc.IsReady()) {
            ESP_LOGW(TAG, "OnNetworkTimeSynced: PCF8563 not ready");
            return;
        }
        if (!TimeServiceStoreRtc()) {
            ESP_LOGW(TAG, "OnNetworkTimeSynced: SyncSystemToRtc failed");
            return;
        }
        if (!TimeServiceRestoreRtc()) {
            ESP_LOGW(TAG, "OnNetworkTimeSynced: ApplyRtcToSystem failed");
            return;
        }
        ESP_LOGD(TAG, "network time synced to PCF8563; system uses RTC");
    }
};

DECLARE_BOARD(MetalioEInk4Board);
