#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include "tcp.h"

class NetworkInterface;

class WebSocket {
public:
    WebSocket(NetworkInterface* network, int connect_id);
    ~WebSocket();

    void SetHeader(const char* key, const char* value);
    void SetReceiveBufferSize(size_t size);

    /** @brief 设置连接或下一次发送的绝对截止时间，单位微秒。 */
    void SetRequestDeadline(int64_t deadline_us);

    /** @brief 复制本连接的 PEM 信任根，空字符串使用系统信任根。 */
    void SetCaCertificate(const std::string& pem);
    bool IsConnected() const;
    bool Connect(const char* uri);
    bool Send(const std::string& data);
    bool Send(const void* data, size_t len, bool binary = false, bool fin = true);
    void Ping();
    void Close();

    void OnConnected(std::function<void()> callback);
    void OnDisconnected(std::function<void()> callback);
    void OnData(std::function<void(const char*, size_t, bool binary)> callback);
    void OnError(std::function<void(int)> callback);

    // 获取最后一次错误码
    int GetLastError();

private:
    NetworkInterface* network_;
    int connect_id_;
    std::unique_ptr<Tcp> tcp_;
    bool continuation_ = false;
    size_t receive_buffer_size_ = 2048;
    std::string receive_buffer_;
    std::string current_message_; // 每连接独立的分片组装状态
    std::string expected_accept_; // 与本次随机握手键对应的服务端应答
    std::string ca_pem_; // TLS 会话持有期间保持证书有效
    bool fragmented_ = false;
    bool message_binary_ = false;
    std::atomic<int64_t> request_deadline_us_{0};
    bool handshake_completed_ = false;
    std::atomic<bool> connected_{false};

    // Mutex for sending data and replying pong
    std::mutex send_mutex_;
    
    EventGroupHandle_t handshake_event_group_;
    static const EventBits_t HANDSHAKE_SUCCESS_BIT = BIT0;
    static const EventBits_t HANDSHAKE_FAILED_BIT = BIT1;

    std::map<std::string, std::string> headers_;
    std::function<void(const char*, size_t, bool binary)> on_data_;
    std::function<void(int)> on_error_;
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;

    void OnTcpData(const std::string& data);
    void FailHandshake(const char* reason);
    void FailFrame(int code);
    bool SendControlFrame(uint8_t opcode, const void* data, size_t len);
};

#endif // WEBSOCKET_H
