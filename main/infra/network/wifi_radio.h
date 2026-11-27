#pragma once
#include <mutex>
#include <esp_err.h>
#include <esp_netif.h>

enum class WifiApStartResult {
    Ok,           // 热点已启动
    InitFailed,   // 初始化或占用条件不满足
    StartFailed,  // 配置或启动失败，已尝试恢复
};

/** @brief 协调传输热点与低功耗的射频占用，不持有传输业务状态。 */
class WifiRadio {
public:
    /** @brief 获取公共射频协调器。 */
    static WifiRadio& GetInstance();

    /** @brief 暂停 STA 后启动传输热点；失败时恢复进入前的状态。 */
    WifiApStartResult StartAccessPoint(const char* ssid);

    /** @brief 停止所拥有的热点；按用户开关及休眠状态决定是否恢复 STA。 */
    esp_err_t StopAccessPoint(bool resume_station);

    /** @brief 暂停 STA；standby 为 true 时同时阻止新热点，活动热点须由业务先收尾。 */
    esp_err_t PauseForSleep(bool standby = true);

    /** @brief 解除休眠限制并恢复用户允许的 STA，不中断活动热点。 */
    esp_err_t ResumeFromSleep();

    /** @brief 预留手动STA配网占用；热点或休眠占用期间拒绝接纳。 */
    bool BeginStationSetup();

    /** @brief 配网驱动完成收尾后归还占用。 */
    void EndStationSetup();

    /** @brief 查询STA是否由配网能力管理，其他入口不得启动或暂停该驱动。 */
    bool IsStationSetupActive();

private:
    WifiRadio() = default;
    esp_err_t StopAccessPointLocked(bool resume_station);
    std::mutex mutex_;                 // 串行化热点与休眠切换
    esp_netif_t* ap_netif_ = nullptr;  // 固定复用的热点接口
    int previous_mode_ = 0;            // 借用驱动时恢复原模式
    bool driver_owned_ = false;        // 驱动由协调器初始化
    bool station_paused_ = false;      // 进入热点时保留的 STA 恢复责任
    bool ap_owned_ = false;            // 已开始热点配置，包括未完成启动的情况
    bool stop_pending_ = false;        // 业务已要求释放，失败时由恢复入口继续收尾
    bool ap_started_ = false;          // 热点已完成启动
    bool sleeping_ = false;            // 休眠期间拒绝启动热点
    bool station_setup_ = false;       // 手动配网期间阻止其他入口操作所借用的驱动
};
