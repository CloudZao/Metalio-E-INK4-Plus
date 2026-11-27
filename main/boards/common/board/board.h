#ifndef BOARD_H
#define BOARD_H

#include <string>
#include <functional>
#include <http.h>
#include <web_socket.h>
#include <mqtt.h>
#include <udp.h>
#include <network_interface.h>
#include <esp_err.h>
#include "boards/common/display/backlight.h"
class Camera;

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

enum class NetworkKind {
    Wifi,      // WiFi
    Cellular,  // 蜂窝网络
};
enum class NetworkPhase {
    Idle,          // 未连接且无在途连接
    Scanning,      // 正在扫描
    Connecting,    // 正在连接或注册
    Ready,         // 网络已就绪
    Provisioning,  // 配网模式
};
struct NetworkStatus {
    NetworkKind kind = NetworkKind::Wifi;     // 联网介质
    NetworkPhase phase = NetworkPhase::Idle;  // 当前阶段
    int signal = -1;                          // WiFi为RSSI，蜂窝为CSQ；未知为-1
};

// 网络事件回调：event / data，data 可包含 SSID 等附加信息。
using NetworkEventCallback = std::function<void(NetworkEvent event, const std::string& data)>;

/** @brief 创建所选板型实例，所有权交给板级单例。 */
void* create_board();
class AudioCodec;
class Display;
class Board {
private:
    Board(const Board&) = delete;             // 禁用拷贝构造函数
    Board& operator=(const Board&) = delete;  // 禁用赋值操作

protected:
    Board();
    std::string GenerateUuid();

    // 软件生成的设备唯一标识
    std::string uuid_;

public:
    /** @brief 获取当前板型的常驻实例。 */
    static Board& GetInstance() {
        static Board* instance = static_cast<Board*>(create_board());
        return *instance;
    }

    virtual ~Board() = default;

    /** @brief 获取板型标识。 */
    virtual std::string GetBoardType() = 0;

    /** @brief 获取软件设备标识。 */
    virtual std::string GetUuid() {
        return uuid_;
    }

    /** @brief 获取可选前光控制器，不支持时返回空指针。 */
    virtual Backlight* GetBacklight() {
        return nullptr;
    }

    /** @brief 获取音频硬件接口。 */
    virtual AudioCodec* GetAudioCodec() = 0;

    /** @brief 读取芯片温度，失败返回false。 */
    virtual bool GetTemperature(float& esp32temp);

    /** @brief 获取显示设备入口。 */
    virtual Display* GetDisplay();

    /** @brief 获取可选摄像头入口，不可用时返回空指针。 */
    virtual Camera* GetCamera();

    /** @brief 获取当前联网接口。 */
    virtual NetworkInterface* GetNetwork() = 0;

    /** @brief 启动当前联网方式。 */
    virtual void StartNetwork() = 0;

    /**
     * @brief 确保业务可用的网络已就绪（WiFi：已关联且拿到 IP）
     * @param timeout_ms 等待上限；默认 30s
     * @return true 已就绪
     * @note 电源策略关 WiFi 后进百问/OTA 等须先调本接口再发 HTTP/WS
     * @note WiFi：timeout_ms>=30s 仍未连上时 Pause STA（停扫）；短切片失败不停
     */
    virtual bool EnsureNetworkReady(int timeout_ms = 30000) = 0;

    /**
     * @brief 刷新等网截止时间（不阻塞）；已有 Ensure 在等则延长共享 deadline
     * @param timeout_ms 自此刻起再等的毫秒数
     */
    virtual void RefreshNetworkWaitDeadline(int timeout_ms = 30000) {
        (void)timeout_ms;
    }

    /**
     * @brief 等网预算用尽仍未就绪时停网（WiFi：Pause STA）；短切片循环结束时调用
     */
    virtual void PauseNetworkIfNotReady() {}

    /**
     * @brief 网络是否已就绪（WiFi：已关联且拿到 IP），不触发重连
     */
    virtual bool IsNetworkReady() = 0;

    /** @brief 返回当前网络不可用的操作提示，无特定原因时返回默认文案 */
    virtual const char* GetNetworkErrorText(const char* fallback) {
        return fallback;
    }

    /**
     * @brief SoftAP 配网中（无互联网）；业务应立即失败，勿 EnsureNetworkReady 空等
     */
    virtual bool IsWifiConfigMode() {
        return false;
    }

    /** @brief 查询原始联网阶段和信号，不包含界面文案或图标。 */
    virtual NetworkStatus GetNetworkStatus() = 0;

    /** @brief 读取电量与充放电状态，失败返回false。 */
    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging);

    /** @brief 生成系统信息快照，板级信息生成失败时返回空字符串。 */
    virtual std::string GetSystemInfoJson();

    /** @brief 调整当前联网方式的节能设置。 */
    virtual void SetPowerSaveMode(bool enabled) = 0;

    /** @brief 生成板级信息快照，分配或编码失败时返回空字符串。 */
    virtual std::string GetBoardJson() = 0;

    /** @brief 生成设备状态快照，分配或编码失败时返回空字符串。 */
    virtual std::string GetDeviceStatusJson() = 0;

    /** @brief 设置当前联网事件回调。 */
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) {
        (void)callback;
    }

    /** @brief 联网对时成功后同步板级时钟外设，不支持时不执行操作 */
    virtual void OnNetworkTimeSynced() {}

    /** @brief 直接切换持续振动，绕过按键反馈开关；无马达板型不执行操作 */
    virtual void SetVibration(bool on) {
        (void)on;
    }

    /** @brief 按当前触摸芯片约定进入休眠，不支持时返回错误。 */
    virtual esp_err_t TouchEnterSleep() {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /** @brief 恢复当前触摸芯片，不支持时返回错误。 */
    virtual esp_err_t TouchWakeByReset() {
        return ESP_ERR_NOT_SUPPORTED;
    }
};

// 注册板类：定义 create_board() 工厂供 Board::GetInstance
#define DECLARE_BOARD(BOARD_CLASS_NAME) \
    void* create_board() {              \
        return new BOARD_CLASS_NAME();  \
    }

#endif  // BOARD_H
