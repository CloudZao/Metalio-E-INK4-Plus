#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <esp_wifi_types_generic.h>
#include "business/transfer/ap_transfer.h"

struct WifiSetupSession;
using WifiSetupSessionPtr = std::shared_ptr<WifiSetupSession>;

enum class WifiSetupOperation { Scan, Connect, SetDefault, Remove, Clear };
enum class WifiSetupStage { Queued, Initializing, Scanning, Connecting, Restart };

struct WifiSetupAccessPoint {
    std::string ssid;
    int8_t rssi = -127;
    wifi_auth_mode_t authmode = WIFI_AUTH_OPEN;
};

struct WifiSetupResult {
    WifiSetupOperation operation = WifiSetupOperation::Scan;
    std::atomic<WifiSetupStage> stage{WifiSetupStage::Queued};
    std::atomic<bool> completed{false};
    std::atomic<int64_t> reboot_at_us{0};
    bool ok = false;
    bool timeout = false;
    uint8_t disconnect_reason = 0;
    std::string ssid; // 仅供界面展示，不记录日志
    std::vector<WifiSetupAccessPoint> access_points;
    ApTransferInfo transfer{};
    char error[96] = {};
};

using WifiSetupRequest = std::shared_ptr<WifiSetupResult>;

/** @brief 创建配网会话；旧会话尚未收尾或内存不足时返回空，不访问控件。 */
WifiSetupSessionPtr WifiSetupOpen(bool transfer);

/** @brief 提交扫描、连接或配置修改；忙碌时未接纳，密码仅由工作上下文持有。 */
WifiSetupRequest WifiSetupSubmit(const WifiSetupSessionPtr& session, WifiSetupOperation operation,
                                const std::string& ssid = {}, const std::string& password = {});

/** @brief 请求结束会话，唤醒正在等待的扫描和连接；UI不等待硬件收尾。 */
void WifiSetupClose(const WifiSetupSessionPtr& session, bool resume_station = true);

/** @brief 请求关闭当前配网会话，供射频切换与整机待机协作。 */
void WifiSetupCloseCurrent(bool resume_station = true);

/** @brief 查询配网驱动是否已完成关闭，硬件切换前须等待此条件。 */
bool WifiSetupIsClosed();

/** @brief 判断指定会话是否仍可提交操作。 */
bool WifiSetupIsOpen(const WifiSetupSessionPtr& session);
