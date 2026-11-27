#pragma once
#include <atomic>
#include "device_state.h"

enum class SystemPhase {
    Starting,    // 核心启动尚未完成
    Running,     // 核心启动完成
    Updating,    // 独占镜像写入阶段
    Restarting,  // 已接受重启，禁止重新建立业务会话
    Failed,      // 核心启动失败
};

class SystemState {
public:
    /** @brief 获取系统运行状态；只保存状态，不持有业务或硬件对象 */
    static SystemState& GetInstance() {
        static SystemState instance;
        return instance;
    }

    /** @brief 查询当前运行状态 */
    DeviceState Device() const {
        return device_.load(std::memory_order_acquire);
    }

    /** @brief 查询与会话状态独立的整机阶段 */
    SystemPhase Phase() const {
        return phase_.load(std::memory_order_acquire);
    }

    /** @brief 查询核心启动是否完成 */
    bool IsReady() const {
        return ready_.load(std::memory_order_acquire);
    }

    /** @brief 查询启动流程是否正在访问硬件，待机暂停时为 false */
    bool IsStartupBusy() const {
        return startup_busy_.load(std::memory_order_acquire);
    }

    /** @brief 查询对话会话是否已经建立 */
    bool IsVoiceActive() const {
        return voice_active_.load(std::memory_order_acquire);
    }

private:
    friend class Application;
    friend class ConversationController;
    friend class FirmwareUpdate;
    friend bool SystemRequestReboot();
    SystemState() = default;
    bool PublishDevice(DeviceState state);
    void MarkReady();
    void FailStartup();
    bool BeginUpdate();
    void EndUpdate();
    void BeginRestart();
    void SetStartupBusy(bool busy) {
        startup_busy_.store(busy, std::memory_order_release);
    }
    void SetVoiceActive(bool active) {
        voice_active_.store(active, std::memory_order_release);
    }
    std::atomic<DeviceState> device_{kDeviceStateUnknown};   // 系统调度发布的运行状态
    std::atomic<SystemPhase> phase_{SystemPhase::Starting};  // 独立的整机生命周期
    std::atomic<bool> ready_{false};                         // 核心启动完成，不等待可选预热
    std::atomic<bool> startup_busy_{true};                   // 启动流程是否仍持有硬件访问责任
    std::atomic<bool> voice_active_{false};                  // 对话会话存活状态
};
