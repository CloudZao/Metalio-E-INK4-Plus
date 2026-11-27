#ifndef WIFI_BOARD_H
#define WIFI_BOARD_H

#include "board.h"

class WifiBoard : public Board {
protected:
    bool wifi_config_mode_ = false;
    void EnterWifiConfigMode();
    virtual std::string GetBoardJson() override;

public:
    /** @brief 读取 Wi-Fi 启动配置，连接由联网启动接口发起 */
    WifiBoard();
    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual bool EnsureNetworkReady(int timeout_ms = 30000) override;

    /** @brief Wi-Fi 关闭时返回开启提示，否则使用默认文案 */
    const char* GetNetworkErrorText(const char* fallback) override;
    virtual void RefreshNetworkWaitDeadline(int timeout_ms = 30000) override;
    virtual void PauseNetworkIfNotReady() override;
    virtual bool IsNetworkReady() override;
    virtual bool IsWifiConfigMode() override;
    virtual NetworkInterface* GetNetwork() override;

    /** @brief 返回当前联网状态与原始信号强度。 */
    NetworkStatus GetNetworkStatus() override;
    virtual void SetPowerSaveMode(bool enabled) override;

    /** @brief 提示用户通过设备设置页管理 Wi-Fi 配置 */
    virtual void ResetWifiConfiguration();
    virtual AudioCodec* GetAudioCodec() override {
        return nullptr;
    }
    virtual std::string GetDeviceStatusJson() override;
};

#endif  // WIFI_BOARD_H
