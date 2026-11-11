#ifndef _ESP_SSL_H_
#define _ESP_SSL_H_

#include "tcp.h"
#include <esp_tls.h>
#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#define ESP_SSL_EVENT_RECEIVE_TASK_EXIT 1 // TLS 接收任务已退出。

class EspSsl : public Tcp {
public:
    /** @brief 创建 TLS 连接及接收任务同步资源 */
    EspSsl();
    /** @brief 等待接收任务退出并释放 TLS 资源 */
    ~EspSsl();

    /** @brief 建立 TLS 连接，启动接收任务；失败原因由 GetLastError 返回 */
    bool Connect(const std::string& host, int port) override;
    /** @brief 请求接收任务停止，等待退出后释放连接；允许重复调用 */
    void Disconnect() override;
    /** @brief 完整发送数据，成功返回字节数，失败返回底层错误值 */
    int Send(const std::string& data) override;

    /** @brief 获取最近一次连接或接收操作的错误码 */
    int GetLastError() override;

private:
    esp_tls_t* tls_client_ = nullptr; // TLS 上下文及套接字的唯一释放入口。
    EventGroupHandle_t event_group_ = nullptr; // 接收任务退出通知。
    TaskHandle_t receive_task_handle_ = nullptr; // 接收任务句柄，等待退出后清空。
    int last_error_ = 0; // 最近一次操作的错误码。
    std::atomic<bool> disconnecting_{false}; // 主动关闭时等待接收任务退出，不将收尾记录为接收故障。

    void ReceiveTask();
};

#endif // _ESP_SSL_H_
