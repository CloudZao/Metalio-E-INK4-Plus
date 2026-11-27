#include <cstring>
#include <cerrno>
#include <arpa/inet.h>
#include <esp_log.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include "esp_udp.h"

static const char *TAG = "EspUdp";

EspUdp::EspUdp() : udp_fd_(-1) {
    event_group_ = xEventGroupCreate();
}

EspUdp::~EspUdp() {
    Disconnect();

    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

bool EspUdp::Connect(const std::string& host, int port) {
    if (!event_group_) {
        last_error_ = ENOMEM;
        ESP_LOGE(TAG, "operation=connect stage=event_group result=no_memory");
        return false;
    }
    Disconnect();

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

    udp_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd_ < 0) {
        last_error_ = errno;
        ESP_LOGE(TAG, "Failed to create socket");
        return false;
    }

    int ret = connect(udp_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        last_error_ = errno;

        close(udp_fd_);
        udp_fd_ = -1;
        return false;
    }

    timeval timeout{};
    timeout.tv_usec = 200000;
    if (setsockopt(udp_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(udp_fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        last_error_ = errno;
        Disconnect();
        return false;
    }
    connected_ = true;

    xEventGroupClearBits(event_group_, ESP_UDP_EVENT_RECEIVE_TASK_EXIT);
    const auto created = xTaskCreate([](void* arg) {
        EspUdp* udp = (EspUdp*)arg;
        try {
            udp->ReceiveTask();
        } catch (...) {
            udp->last_error_ = ENOMEM;
            udp->connected_.store(false);
            ESP_LOGE(TAG, "operation=receive result=no_memory");
        }
        xEventGroupSetBits(udp->event_group_, ESP_UDP_EVENT_RECEIVE_TASK_EXIT);
        vTaskDelete(NULL);
    }, "udp_receive", 2048, this, 1, &receive_task_handle_);
    if (created != pdPASS) {
        last_error_ = ENOMEM;
        ESP_LOGE(TAG, "operation=connect stage=task result=no_memory");
        Disconnect();
        return false;
    }
    return true;
}

void EspUdp::Disconnect() {
    connected_.store(false);
    if (receive_task_handle_) {
        while (!(xEventGroupWaitBits(event_group_, ESP_UDP_EVENT_RECEIVE_TASK_EXIT,
                                    pdFALSE, pdFALSE, pdMS_TO_TICKS(1000)) & ESP_UDP_EVENT_RECEIVE_TASK_EXIT)) {
            ESP_LOGW(TAG, "operation=disconnect stage=join result=waiting");
        }
        receive_task_handle_ = nullptr;
    }
    if (udp_fd_ != -1) {
        close(udp_fd_);
        udp_fd_ = -1;
    }
}

int EspUdp::Send(const std::string& data) {
    if (!connected_) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    int ret = send(udp_fd_, data.data(), data.size(), 0);
    if (ret < 0 || ret != static_cast<int>(data.size())) {
        last_error_ = ret < 0 ? errno : EIO;
        ESP_LOGE(TAG, "operation=send result=failed code=%d", last_error_.load());
        return -1;
    }
    return ret;
}

void EspUdp::ReceiveTask() {
    std::string data;
    while (connected_.load()) {
        // 多读一字节以识别超长报文，禁止把截断数据伪装成完整消息。
        data.resize(receive_buffer_size_ + 1);
        const int received = recv(udp_fd_, data.data(), data.size(), 0);
        const int code = received < 0 ? errno : 0;
        if (!connected_.load()) {
            break;
        }
        if (received < 0 && (code == EAGAIN || code == EWOULDBLOCK || code == EINTR)) {
            continue;
        }
        if (received < 0 || static_cast<size_t>(received) > receive_buffer_size_) {
            last_error_ = received < 0 ? code : EMSGSIZE;
            connected_.store(false);
            ESP_LOGW(TAG, "operation=receive result=failed code=%d", last_error_.load());
            break;
        }
        if (message_callback_) {
            data.resize(received);
            message_callback_(data);
        }
    }
}

int EspUdp::GetLastError() {
    return last_error_;
}
