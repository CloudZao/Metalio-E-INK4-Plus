#ifndef DUAL_NETWORK_BOARD_H
#define DUAL_NETWORK_BOARD_H

#include <memory>
#include <driver/gpio.h>
#include "board.h"

enum class NetworkType { WIFI = 0, CELLULAR = 1 };

// 双网络板卡类，可以在WiFi和4G之间切换
class DualNetworkBoard : public Board {
private:
    // 使用基类指针存储当前活动的板卡
    std::unique_ptr<Board> current_board_;
    NetworkType network_type_ = NetworkType::CELLULAR;

    // Cellular(NT26) pin configuration
    gpio_num_t cellular_tx_pin_;
    gpio_num_t cellular_rx_pin_;
    gpio_num_t cellular_dtr_pin_;
    gpio_num_t cellular_ri_pin_;

    bool SaveNetworkTypeToSettings(NetworkType type);

    // 初始化当前网络类型对应的板卡
    void InitializeCurrentBoard();

public:
    /** @brief 读取联网偏好并创建活动板卡，蜂窝握手引脚由板级传入 */
    DualNetworkBoard(gpio_num_t cellular_tx_pin, gpio_num_t cellular_rx_pin, gpio_num_t cellular_dtr_pin,
                     gpio_num_t cellular_ri_pin, int32_t default_net_type);

    virtual ~DualNetworkBoard() = default;

    /** @brief 读取联网偏好，无记录时按默认值选择；全球产品固定使用 Wi-Fi */
    static NetworkType LoadNetworkTypeFromSettings(int32_t default_net_type);

    /** @brief 保存目标网络并请求重启，失败时保持当前网络；已是目标类型时返回 true */
    bool SwitchToNetworkType(NetworkType type);

    /** @brief 获取当前活动的联网类型 */
    NetworkType GetNetworkType() const {
        return network_type_;
    }

    /** @brief 获取当前网络类型对应的不可用提示 */
    const char* GetNetworkErrorText(const char* fallback) override {
        return current_board_->GetNetworkErrorText(fallback);
    }

    /** @brief 获取当前活动板卡的借用引用，由双网络板卡持有其生命周期 */
    Board& GetCurrentBoard() const {
        return *current_board_;
    }

    AudioCodec* GetAudioCodec() override {
        return nullptr;
    }
    void SetNetworkEventCallback(NetworkEventCallback callback) override {
        current_board_->SetNetworkEventCallback(std::move(callback));
    }
    // 重写Board接口

    /** @brief 返回当前构建的产品身份，不随WiFi或蜂窝网络切换改变。 */
    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual bool EnsureNetworkReady(int timeout_ms = 30000) override;
    virtual void RefreshNetworkWaitDeadline(int timeout_ms = 30000) override;
    virtual void PauseNetworkIfNotReady() override;
    virtual bool IsNetworkReady() override;
    virtual bool IsWifiConfigMode() override;
    virtual NetworkInterface* GetNetwork() override;

    /** @brief 返回当前联网状态与原始信号强度。 */
    NetworkStatus GetNetworkStatus() override;
    virtual void SetPowerSaveMode(bool enabled) override;
    virtual std::string GetBoardJson() override;
    virtual std::string GetDeviceStatusJson() override;
};

#endif  // DUAL_NETWORK_BOARD_H
