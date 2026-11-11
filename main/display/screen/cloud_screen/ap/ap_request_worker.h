#pragma once

#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

/** @brief AP 内部有界异步请求任务；启动和停止由无线调度层串行调用。 */
class ApRequestWorker {
public:
    ApRequestWorker() = default;
    ApRequestWorker(const ApRequestWorker&) = delete;
    ApRequestWorker& operator=(const ApRequestWorker&) = delete;
    static constexpr unsigned kMaxClients = 4; // 与 HTTP 服务共用的客户端连接上限
    /** @brief 创建 PSRAM 队列及工作任务；涉及 NVS 时使用内部 RAM 栈。 */
    bool Start(const char* name, bool internal_stack = false);
    /** @brief 接管 SDK 异步请求；失败时不保留请求所有权。 */
    esp_err_t Submit(httpd_req_t* req, esp_err_t (*handler)(httpd_req_t*));
    /** @brief 延后回收连接资源；返回 false 时由调用方回收。 */
    bool Cleanup(void (*cleanup)(void*), void* context);
    /** @brief 停止接收新请求并唤醒所有正在阻塞的网络读写。 */
    void CancelRequests();
    /** @brief 等待此前投递的请求和清理任务全部完成。 */
    void Drain();
    /** @brief 排空队列并退出任务，释放队列缓冲；须先停止 HTTP 服务。 */
    void Stop();

private:
    enum class Kind {
        Request, // SDK 异步请求
        Cleanup, // 会话资源回收
        Barrier, // 排空确认
        Stop, // 任务退出
    };
    struct Job {
        Kind kind = Kind::Request; // 请求、资源回收或生命周期屏障
        httpd_req_t* req = nullptr; // 由工作任务完成的 SDK 异步请求
        esp_err_t (*handler)(httpd_req_t*) = nullptr; // 原业务处理器
        void (*cleanup)(void*) = nullptr; // 连接关闭后的资源回收
        void* context = nullptr; // 清理函数的会话参数
    };
    static void Run(void* arg);
    void RemoveFd(int fd);
    static constexpr unsigned kDepth = 8; // 请求数受 HTTP 最大连接数约束，额外容量用于回收和屏障
    StaticQueue_t queue_storage_{}; // 队列控制块，仅占少量内部 RAM
    StaticSemaphore_t mutex_storage_{}, done_storage_{}; // 生命周期互斥及屏障控制块
    QueueHandle_t queue_ = nullptr; // 有界工作队列
    SemaphoreHandle_t mutex_ = nullptr, done_ = nullptr; // 请求注册表锁及完成屏障
    uint8_t* buffer_ = nullptr; // PSRAM 队列内容缓冲
    bool accepting_ = false; // 受 mutex_ 保护的请求接收开关
    int fds_[kMaxClients]{}; // 未完成请求；容量与 AP 最大客户端连接数一致
};
