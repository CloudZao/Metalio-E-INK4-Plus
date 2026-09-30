#ifndef BOARD_H
#define BOARD_H

#include <http.h>
#include <web_socket.h>
#include <mqtt.h>
#include <udp.h>
#include <string>
#include <network_interface.h>
#include <esp_err.h>

#include "backlight.h"
#include "camera.h"
#include "assets.h"

// 网络事件枚举：统一给业务层回调。
enum class NetworkEvent {
    Scanning,
    Connecting,
    Connected,
    Disconnected,
    WifiConfigModeEnter,
    WifiConfigModeExit,
    ModemDetecting,
    ModemErrorNoSim,
    ModemErrorRegDenied,
    ModemErrorInitFailed,
    ModemErrorTimeout
};

// 省电级别。
enum class PowerSaveLevel {
    LOW_POWER,
    BALANCED,
    PERFORMANCE,
};

// 网络事件回调：event / data，data 可包含 SSID 等附加信息。
using NetworkEventCallback = std::function<void(NetworkEvent event, const std::string& data)>;


void* create_board();
class AudioCodec;
class Display;
class Board {
private:
    Board(const Board&) = delete; // 禁用拷贝构造函数
    Board& operator=(const Board&) = delete; // 禁用赋值操作

protected:
    Board();
    std::string GenerateUuid();

    // 软件生成的设备唯一标识
    std::string uuid_;

public:
    static Board& GetInstance() {
        static Board* instance = static_cast<Board*>(create_board());
        return *instance;
    }

    virtual ~Board() = default;
    virtual std::string GetBoardType() = 0;
    virtual std::string GetUuid() { return uuid_; }
    virtual Backlight* GetBacklight() { return nullptr; }
    virtual AudioCodec* GetAudioCodec() = 0;
    virtual bool GetTemperature(float& esp32temp);
    virtual Display* GetDisplay();
    virtual Camera* GetCamera();
    virtual NetworkInterface* GetNetwork() = 0;
    virtual void StartNetwork() = 0;
    /**
     * @brief 确保业务可用的网络已就绪（WiFi：已关联且拿到 IP）
     * @param timeout_ms 等待上限；默认 30s
     * @return true 已就绪
     * @note 电源策略关 WiFi 后进百问/OTA 等须先调本接口再发 HTTP/WS
     * @note WiFi：timeout_ms>=30s 仍未连上时 Pause STA（停扫）；短切片失败不停
     */
    virtual bool EnsureNetworkReady(int timeout_ms = 30000) {
        (void)timeout_ms;
        return true;
    }
    /**
     * @brief 刷新等网截止时间（不阻塞）；已有 Ensure 在等则延长共享 deadline
     * @param timeout_ms 自此刻起再等的毫秒数
     */
    virtual void RefreshNetworkWaitDeadline(int timeout_ms = 30000) { (void)timeout_ms; }
    /**
     * @brief 等网预算用尽仍未就绪时停网（WiFi：Pause STA）；短切片循环结束时调用
     */
    virtual void PauseNetworkIfNotReady() {}
    /**
     * @brief 网络是否已就绪（WiFi：已关联且拿到 IP），不触发重连
     */
    virtual bool IsNetworkReady() { return true; }
    /**
     * @brief SoftAP 配网中（无互联网）；业务应立即失败，勿 EnsureNetworkReady 空等
     */
    virtual bool IsWifiConfigMode() { return false; }
    virtual const char* GetNetworkStateIcon() = 0;
    /**
     * @brief 扫网/关联中时顶栏状态文案；空闲返回 nullptr（继续时钟等）
     */
    virtual const char* GetNetworkBusyStatusText() { return nullptr; }
    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging);
    virtual std::string GetSystemInfoJson();
    virtual void SetPowerSaveMode(bool enabled) = 0;
    virtual std::string GetBoardJson() = 0;
    virtual std::string GetDeviceStatusJson() = 0;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) { (void)callback; }
    // OTA / 联网对时成功后调用：板级可把系统时间写入 RTC 等外设。
    virtual void OnNetworkTimeSynced() {}
    // 短震触觉反馈（虚拟键/点击等）；无马达的板为空实现。
    // 业务侧请走 HapticPulseIfEnabled()，以便尊重设置中的按键震动开关。
    virtual void PulseVibration() {}
    // 常开/关闭马达（产测老化等）；绕过按键震动开关。无马达板为空实现。
    virtual void SetVibration(bool on) { (void)on; }

    // 触摸芯片休眠：写寄存器 0xA5=0x03；唤醒仅硬件 RST
    virtual esp_err_t TouchEnterSleep() { return ESP_ERR_NOT_SUPPORTED; }
    // 退出休眠：硬件脉冲 RST
    virtual esp_err_t TouchWakeByReset() { return ESP_ERR_NOT_SUPPORTED; }
};

// 注册板类：定义 create_board() 工厂供 Board::GetInstance
#define DECLARE_BOARD(BOARD_CLASS_NAME) \
void* create_board() { \
    return new BOARD_CLASS_NAME(); \
}

#endif // BOARD_H
