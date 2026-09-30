#ifndef _ESP_TCP_H_
#define _ESP_TCP_H_

#include "tcp.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <mutex>

#define ESP_TCP_EVENT_RECEIVE_TASK_EXIT 1

class EspTcp : public Tcp {
public:
    EspTcp();
    ~EspTcp();

    bool Connect(const std::string& host, int port) override;
    void Disconnect() override;
    int Send(const std::string& data) override;

    int GetLastError() override;

private:
    int tcp_fd_ = -1;
    EventGroupHandle_t event_group_ = nullptr;
    TaskHandle_t receive_task_handle_ = nullptr;
    int last_error_ = 0;
    std::mutex mutex_;
    bool disconnect_notified_ = false;

    void ReceiveTask();
    // 只关 socket、回收接收任务。不碰回调，供 Connect 复用同一对象。
    void ShutdownAndJoin();
    // 每个连接世代最多通知一次；拷贝回调，不断开注册。
    void NotifyDisconnected();
};

#endif // _ESP_TCP_H_
