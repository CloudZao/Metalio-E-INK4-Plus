#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct WifiDiagnosticAccessPoint {
    std::string ssid;
    int8_t rssi = -127;
};

struct WifiDiagnosticResult {
    std::atomic<bool> cancelled{false};
    std::atomic<bool> completed{false};
    uint32_t generation = 0;
    bool success = false;
    std::vector<WifiDiagnosticAccessPoint> access_points;
};
using WifiDiagnosticRequest = std::shared_ptr<WifiDiagnosticResult>;

/** @brief 提交独立扫网检测，保留现有连接；忙碌或资源不足时未接纳。 */
WifiDiagnosticRequest WifiDiagnosticStart();

/** @brief 查询旧扫描是否仍在执行或回收硬件资源。 */
bool WifiDiagnosticIsRunning();

/** @brief 请求取消；业务任务负责释放扫描、驱动和射频占用。 */
void WifiDiagnosticCancel(const WifiDiagnosticRequest& request);
