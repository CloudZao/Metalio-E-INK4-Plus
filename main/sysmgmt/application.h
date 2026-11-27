#pragma once
#include <atomic>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

class Application {
public:
    /** @brief 获取整机启动与系统调度实例，不在构造阶段申请 SDK 资源 */
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }

    /** @brief 在内部 RAM 启动栈装配模块；失败发布启动故障，禁止重复启动 */
    bool Start();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

private:
    Application() = default;
    bool InitializeRuntime();
    bool StartDispatcher();
    void FinishStartup(bool completed);
    void BootStandbyCheckpoint();
    void MainEventLoop();
    void DispatchClockTick();
    EventGroupHandle_t event_group_ = nullptr;  // 系统节拍与模块唤醒通知
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    TaskHandle_t dispatcher_ = nullptr;
    std::atomic<bool> started_{false};                 // 保证启动装配只执行一次
    std::atomic<int> clock_ticks_{0};                  // 整机节拍，不随对话状态重置
    std::atomic<bool> activation_work_queued_{false};  // 跨域刷新协调任务在途
    std::atomic<bool> status_work_queued_{false};      // 界面状态刷新合并
    std::atomic<bool> storage_retry_queued_{false};    // Wi-Fi 存储重试合并
};
