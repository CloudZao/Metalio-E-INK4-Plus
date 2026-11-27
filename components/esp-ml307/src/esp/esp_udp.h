#ifndef ESP_UDP_H
#define ESP_UDP_H

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include "udp.h"

#define ESP_UDP_EVENT_RECEIVE_TASK_EXIT 1

class EspUdp : public Udp {
public:
    EspUdp();
    ~EspUdp();

    bool Connect(const std::string& host, int port) override;
    void Disconnect() override;
    int Send(const std::string& data) override;
    int GetLastError() override;

    /** @brief 设置 IPv4 UDP 接收报文容量，范围 1—65507 字节。 */
    bool SetReceiveBufferSize(size_t bytes) override {
        if (!bytes || bytes > 65507) {
            return false;
        }
        receive_buffer_size_ = bytes;
        return true;
    }

private:
    int udp_fd_;
    EventGroupHandle_t event_group_ = nullptr;
    TaskHandle_t receive_task_handle_ = nullptr;
    std::atomic<int> last_error_{0};
    size_t receive_buffer_size_ = 1500; // 默认业务报文容量，应用连接可单独指定

    void ReceiveTask();
};

#endif // ESP_UDP_H
