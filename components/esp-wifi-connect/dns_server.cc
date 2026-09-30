#include "dns_server.h"

#include <cstring>

#include <esp_log.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#define TAG "DnsServer"

DnsServer::DnsServer() {}

DnsServer::~DnsServer() {
    Stop();
}

void DnsServer::Start(esp_ip4_addr_t gateway) {
    if (fd_ >= 0 || task_ != nullptr) {
        return;
    }
    ESP_LOGI(TAG, "Starting DNS server");
    gateway_ = gateway;

    fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd_ < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        return;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port_);

    if (bind(fd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "failed to bind port %d", port_);
        close(fd_);
        fd_ = -1;
        return;
    }

    if (xTaskCreate(
            [](void* arg) {
                auto* dns_server = static_cast<DnsServer*>(arg);
                dns_server->Run();
            },
            "DnsServerTask", 4096, this, 5, &task_) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        close(fd_);
        fd_ = -1;
        task_ = nullptr;
    }
}

void DnsServer::Stop() {
    if (fd_ < 0 && task_ == nullptr) {
        return;
    }
    ESP_LOGI(TAG, "Stopping DNS server");
    const int fd = fd_;
    fd_ = -1;
    if (fd >= 0) {
        // 打断 recvfrom，便于任务退出
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    // 等任务自行 vTaskDelete，避免 wifi_stop 时 socket/netif 竞态
    for (int i = 0; i < 100 && task_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (task_ != nullptr) {
        ESP_LOGW(TAG, "DNS task still alive after stop wait");
        task_ = nullptr;
    }
}

void DnsServer::Run() {
    char buffer[512];
    while (fd_ >= 0) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int len = recvfrom(fd_, buffer, sizeof(buffer), 0, (struct sockaddr*)&client_addr,
                           &client_addr_len);
        if (fd_ < 0) {
            break;
        }
        if (len < 0) {
            continue;
        }

        buffer[2] |= 0x80;
        buffer[3] |= 0x80;
        buffer[7] = 1;

        memcpy(&buffer[len], "\xc0\x0c", 2);
        len += 2;
        memcpy(&buffer[len], "\x00\x01\x00\x01\x00\x00\x00\x1c\x00\x04", 10);
        len += 10;
        memcpy(&buffer[len], &gateway_.addr, 4);
        len += 4;

        sendto(fd_, buffer, len, 0, (struct sockaddr*)&client_addr, client_addr_len);
    }
    task_ = nullptr;
    vTaskDelete(nullptr);
}
