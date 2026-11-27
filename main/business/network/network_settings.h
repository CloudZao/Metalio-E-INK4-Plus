#pragma once
#include <atomic>
#include <cstdint>
#include <memory>

enum class NetworkType;

class NetworkModemSession {
public:
    /** @brief 尝试独占跨多条AT指令的操作，避免产测恢复与设置切卡交叉。 */
    NetworkModemSession();

    /** @brief 归还本实例取得的调制解调器操作占用。 */
    ~NetworkModemSession();

    explicit operator bool() const { return acquired_; }
    NetworkModemSession(const NetworkModemSession&) = delete;
    NetworkModemSession& operator=(const NetworkModemSession&) = delete;

private:
    bool acquired_ = false;
};
enum class NetworkWifiSwitchState { kIdle, kSwitching, kFailed };
enum class NetworkSettingsOperation { QuerySim, SwitchSim, SwitchNetwork };
enum class NetworkSettingsProgress { Queued, Query, Offline, Select, Online, Restart };
enum class NetworkSignalKind { Unsupported, WifiDisconnected, WifiReadFailed, WifiSignal,
                               CellUnavailable, CellUnknown, CellSignal };

struct NetworkSignalResult {
    std::atomic<bool> completed{false};
    NetworkSignalKind kind = NetworkSignalKind::Unsupported;
    int strength = 0; // Wi-Fi为RSSI，蜂窝网络为CSQ
    char iccid[32] = {}; // 仅供界面展示，不记录日志
};

struct NetworkSettingsResult {
    NetworkSettingsOperation operation = NetworkSettingsOperation::QuerySim;
    NetworkType target_type{}; // 网络切换目标，等待重启期间供页面保持一致显示
    int target_slot = 0; // SIM 输入，0为外置卡，1为内置卡
    std::atomic<NetworkSettingsProgress> progress{NetworkSettingsProgress::Queued};
    std::atomic<int64_t> reboot_at_us{0}; // 成功切换后的系统重启期限，不由页面计时器持有
    std::atomic<bool> completed{false};
    bool ok = false;
    char error[96] = {};
};

using NetworkSettingsRequest = std::shared_ptr<const NetworkSettingsResult>;

/** @brief 查询当前板卡使用的网络类型，不在UI线程访问NVS。 */
NetworkType NetworkSettingsCurrentType();

/** @brief 查询当前网络是否具备SIM控制能力。 */
bool NetworkSettingsSupportsSim();

/** @brief 读取已加载或已从调制解调器确认的SIM槽位快照。 */
int NetworkSettingsSimSlot();

/** @brief 加载SIM配置并查询实际槽位，已有操作执行中或投递失败返回空。 */
NetworkSettingsRequest NetworkSettingsQuerySim();

/** @brief 切换SIM并保存确认后的槽位；成功后独立完成重启，失败尝试恢复射频。 */
NetworkSettingsRequest NetworkSettingsSwitchSim(int slot);

/** @brief 保存网络类型并交由板级实现完成切换与重启。 */
NetworkSettingsRequest NetworkSettingsSwitchType(NetworkType type);

/** @brief 获取尚未完成的网络设置操作，页面重建可继续展示进度。 */
NetworkSettingsRequest NetworkSettingsCurrent();

/** @brief 在网络控制通道采样信号和SIM标识，与切卡指令串行；失败返回空。 */
std::shared_ptr<const NetworkSignalResult> NetworkSettingsSampleSignal();

/** @brief 查询设置页与快捷面板共用的Wi-Fi开关状态。 */
NetworkWifiSwitchState NetworkWifiGetSwitchState();

/** @brief 在Wi-Fi模式下异步切换并保存开关，执行期间忽略重复点击。 */
void NetworkWifiToggle();
