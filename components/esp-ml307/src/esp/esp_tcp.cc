#include <algorithm>
#include <cerrno>
#include <cstring>
#include <functional>
#include <mutex>
#include <arpa/inet.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include "esp_tcp.h"

static const char *TAG = "EspTcp";
constexpr int kConnectTimeoutSec = 15;

EspTcp::EspTcp() {
    event_group_ = xEventGroupCreate();
}

EspTcp::~EspTcp() {
    // 先回收接收任务，再清回调、删 event group。析构里不再通知：
    // owner 可能正在拆自己，回调进已半析构的对象是 UAF。
    ShutdownAndJoin();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        disconnect_callback_ = nullptr;
        stream_callback_ = nullptr;
    }

    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

bool EspTcp::Connect(const std::string& host, int port) {
    if (!event_group_) {
        last_error_ = ENOMEM;
        ESP_LOGE(TAG, "operation=connect stage=event_group result=no_memory");
        return false;
    }
    bool had_session = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        had_session = receive_task_handle_ != nullptr || tcp_fd_ != -1;
    }
    // 只拆除旧传输，不能清空 OnDisconnected：HttpClient 是先注册回调再 Connect。
    ShutdownAndJoin();
    if (had_session) {
        NotifyDisconnected();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        disconnect_notified_ = false;
        connected_ = false;
    }

    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    // host is domain
    struct hostent *server = gethostbyname(host.c_str());
    if (server == NULL) {
        last_error_ = h_errno;
        return false;
    }
    memcpy(&server_addr.sin_addr, server->h_addr, server->h_length);

    tcp_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd_ < 0) {
        last_error_ = errno;
        ESP_LOGE(TAG, "Failed to create socket");
        return false;
    }

    const int flags = fcntl(tcp_fd_, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(tcp_fd_, F_SETFL, flags | O_NONBLOCK);
    }

    int ret = connect(tcp_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr));
    const int connect_error = ret < 0 ? errno : 0;
    if (ret < 0 && (connect_error == EINPROGRESS || connect_error == EWOULDBLOCK)) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(tcp_fd_, &wfds);
        struct timeval tv;
        const int64_t wait_us = request_deadline_us_ ?
            std::max<int64_t>(0, std::min<int64_t>(kConnectTimeoutSec * 1000000LL, request_deadline_us_ - esp_timer_get_time())) : kConnectTimeoutSec * 1000000LL;
        tv.tv_sec = wait_us / 1000000;
        tv.tv_usec = wait_us % 1000000;
        ret = select(tcp_fd_ + 1, nullptr, &wfds, nullptr, &tv);
        const int select_error = ret < 0 ? errno : 0;
        if (ret <= 0) {
            last_error_ = (ret == 0) ? ETIMEDOUT : select_error;
            close(tcp_fd_);
            tcp_fd_ = -1;
            return false;
        }
        int so_error = 0;
        socklen_t so_len = sizeof(so_error);
        const int socket_result = getsockopt(tcp_fd_, SOL_SOCKET, SO_ERROR, &so_error, &so_len);
        const int socket_error = socket_result < 0 ? errno : 0;
        if (socket_result < 0 || so_error != 0) {
            last_error_ = socket_result < 0 ? socket_error : so_error;
            close(tcp_fd_);
            tcp_fd_ = -1;
            return false;
        }
    } else if (ret < 0) {
        last_error_ = connect_error;
        close(tcp_fd_);
        tcp_fd_ = -1;
        return false;
    }

    if (flags >= 0) {
        fcntl(tcp_fd_, F_SETFL, flags);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = true;
        disconnect_notified_ = false;
    }

    xEventGroupClearBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
    const auto task_result = xTaskCreate([](void* arg) {
        EspTcp* tcp = (EspTcp*)arg;
        try {
            tcp->ReceiveTask();
        } catch (...) {
            tcp->last_error_ = ENOMEM;
            tcp->connected_.store(false);
            tcp->NotifyDisconnected();
            ESP_LOGE(TAG, "operation=receive result=no_memory");
        }
        xEventGroupSetBits(tcp->event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
        vTaskDelete(NULL);
    }, "tcp_receive", 4096, this, 1, &receive_task_handle_);
    if (task_result != pdPASS) {
        last_error_ = ENOMEM;
        ESP_LOGE(TAG, "Failed to create TCP receive task");
        ShutdownAndJoin();
        return false;
    }
    return true;
}

void EspTcp::Disconnect() {
    ShutdownAndJoin();
    NotifyDisconnected();
}

void EspTcp::ShutdownAndJoin() {
    const bool wait_for_task =
        receive_task_handle_ != nullptr &&
        xTaskGetCurrentTaskHandle() != receive_task_handle_;

    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = false;
        fd = tcp_fd_;
    }
    if (fd != -1) {
        shutdown(fd, SHUT_RDWR);
    }

    if (wait_for_task) {
        while (!(xEventGroupWaitBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT,
                                    pdFALSE, pdFALSE, pdMS_TO_TICKS(10000)) & ESP_TCP_EVENT_RECEIVE_TASK_EXIT)) {
            ESP_LOGW(TAG, "operation=disconnect stage=join result=waiting");
        }
        receive_task_handle_ = nullptr;
    }
    if (!receive_task_handle_ && fd != -1) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tcp_fd_ == fd) {
            tcp_fd_ = -1;
            close(fd);
        }
    }
}

void EspTcp::NotifyDisconnected() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (disconnect_notified_) {
            return;
        }
        disconnect_notified_ = true;
        cb = disconnect_callback_;
    }
    if (cb) {
        cb();
    }
}

int EspTcp::Send(const std::string& data) {
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_ || tcp_fd_ < 0) {
            ESP_LOGE(TAG, "Not connected");
            return -1;
        }
        fd = tcp_fd_;
    }

    size_t total_sent = 0;
    size_t data_size = data.size();
    const char* data_ptr = data.data();

    while (total_sent < data_size) {
        if (request_deadline_us_) {
            const int64_t remaining = request_deadline_us_ - esp_timer_get_time();
            if (remaining <= 0) { last_error_ = ETIMEDOUT; return -1; }
            timeval timeout{};
            timeout.tv_sec = remaining / 1000000;
            timeout.tv_usec = remaining % 1000000;
            if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
                last_error_ = errno;
                return -1;
            }
        }
        const size_t write_size = data_size - total_sent;
        int ret = send(fd, data_ptr + total_sent, write_size, 0);

        const int send_error=ret<0?errno:0;
        if (ret <= 0) {
            last_error_ = send_error;
            return ret;
        }

        total_sent += ret;
    }

    return total_sent;
}

void EspTcp::ReceiveTask() {
    std::string data;
    while (true) {
        int fd = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_ || tcp_fd_ < 0) {
                break;
            }
            fd = tcp_fd_;
        }

        data.resize(1500);
        int ret = recv(fd, data.data(), data.size(), 0);
        if (ret <= 0) {
            const int socket_error = ret < 0 ? errno : 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!connected_) {
                    return;
                }
                if (socket_error) {
                    last_error_ = socket_error;
                    ESP_LOGW(TAG, "operation=receive result=failed socket_errno=%d", socket_error);
                }
                connected_.store(false);
            }
            NotifyDisconnected();
            return;
        }

        std::function<void(const std::string&)> cb;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cb = stream_callback_;
        }
        if (cb) {
            data.resize(ret);
            cb(data);
        }
    }
}

int EspTcp::GetLastError() {
    return last_error_;
}
