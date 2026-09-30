#include "esp_tcp.h"

#include <esp_log.h>
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <functional>
#include <mutex>

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
        ESP_LOGE(TAG, "Failed to get host by name");
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
    if (ret < 0 && (errno == EINPROGRESS || errno == EWOULDBLOCK)) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(tcp_fd_, &wfds);
        struct timeval tv;
        tv.tv_sec = kConnectTimeoutSec;
        tv.tv_usec = 0;
        ret = select(tcp_fd_ + 1, nullptr, &wfds, nullptr, &tv);
        if (ret <= 0) {
            last_error_ = (ret == 0) ? ETIMEDOUT : errno;
            ESP_LOGD(TAG, "Connect to %s:%d timed out or select failed", host.c_str(), port);
            close(tcp_fd_);
            tcp_fd_ = -1;
            return false;
        }
        int so_error = 0;
        socklen_t so_len = sizeof(so_error);
        getsockopt(tcp_fd_, SOL_SOCKET, SO_ERROR, &so_error, &so_len);
        if (so_error != 0) {
            last_error_ = so_error;
            ESP_LOGD(TAG, "Failed to connect to %s:%d, code=0x%x errno=%d (%s)", host.c_str(),
                     port, last_error_, last_error_,
                     strerror(last_error_) != nullptr ? strerror(last_error_) : "?");
            close(tcp_fd_);
            tcp_fd_ = -1;
            return false;
        }
    } else if (ret < 0) {
        last_error_ = errno;
        ESP_LOGD(TAG, "Failed to connect to %s:%d, code=0x%x errno=%d (%s)", host.c_str(), port,
                 last_error_, last_error_,
                 strerror(last_error_) != nullptr ? strerror(last_error_) : "?");
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
    xTaskCreate([](void* arg) {
        EspTcp* tcp = (EspTcp*)arg;
        tcp->ReceiveTask();
        xEventGroupSetBits(tcp->event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
        vTaskDelete(NULL);
    }, "tcp_receive", 4096, this, 1, &receive_task_handle_);
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
        tcp_fd_ = -1;
    }
    if (fd != -1) {
        close(fd);
    }

    if (wait_for_task) {
        auto bits = xEventGroupWaitBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
        if (!(bits & ESP_TCP_EVENT_RECEIVE_TASK_EXIT)) {
            ESP_LOGE(TAG, "Failed to wait for receive task exit");
        }
        receive_task_handle_ = nullptr;
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
        int ret = send(fd, data_ptr + total_sent, data_size - total_sent, 0);

        if (ret <= 0) {
            ESP_LOGE(TAG, "Send failed: ret=%d, errno=%d", ret, errno);
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
            if (ret < 0) {
                ESP_LOGE(TAG, "TCP receive failed: %d", ret);
            }
            ShutdownAndJoin();
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
