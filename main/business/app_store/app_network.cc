#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstring>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <lwip/icmp.h>
#include <lwip/inet.h>
#include <lwip/inet_chksum.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include "http.h"
#include "network_interface.h"
#include "app_network.h"
#include "boards/common/board/board.h"
#include "infra/network/http/download_gate.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "sysmgmt/power/power_lifecycle.h"
#include "sysmgmt/system_state.h"
#include "sysmgmt/tasks/task_executor.h"

#if CONFIG_IDF_TARGET_ESP32S3
// Xtensa GCC 15.2 的 IPA modref 与运行期服务相同，按文件关闭该项分析。
#pragma GCC optimize ("no-ipa-modref")
#endif

static constexpr const char* TAG = "AppNetwork";
static constexpr size_t kRecordCount = 32;
static constexpr unsigned kConnectionCount = 4;
static constexpr size_t kNetworkStackBytes = 8192; // 应用网络工作线程使用 PSRAM 栈
static constexpr uint32_t kNetworkResolve = 6; // 宿主内部操作，不作为传输协议暴露
static constexpr uint32_t kNetworkPing = 7;
static constexpr const char* kResponseHeaders[] = {
    "content-type", "content-length", "etag", "last-modified", "location", "retry-after", "content-range", "accept-ranges"
};

struct AppNetworkRecord {
    uint32_t length = 0; // 完整消息或字节流分块长度
    uint32_t flags = 0; // xz_app_net_flag_t
};

struct AppNetworkConnection {
    uint32_t id = 0; // 本次应用会话内不复用
    uint32_t protocol = 0;
    uint32_t port = 0;
    uint32_t timeout_ms = 0;
    uint32_t receive_capacity = 0; // ELF 请求的接收缓冲及单条 WS/UDP 消息容量
    uint32_t send_capacity = 0; // ELF 请求的单条异步发送容量
    uint32_t response_limit = 0; // ELF 指定的 HTTP 总长度限制，零表示不限制总量
    xz_app_ping_config_t ping{}; // 目标指针清空，仅由宿主持有标量配置
    xz_app_net_diagnostics_t diagnostics{}; // mutex 保护，地址和统计不借用 ELF 内存
    int64_t deadline_us = 0; // 从接纳开始计算连接或 HTTP 总期限
    std::string address;
    std::string method;
    std::string ca_pem;
    std::array<std::pair<std::string, std::string>, 8> headers;
    uint32_t header_count = 0;
    std::string body; // 仅供首次 HTTP 请求使用
    std::atomic<bool> cancelled{false};
    std::atomic<bool> running{true}; // 包含底层接收线程退出
    DownloadGate gate;
    std::mutex mutex; // 仅保护有界内存复制和状态，不跨网络操作持有
    xz_app_net_status_t status{sizeof(xz_app_net_status_t), XZ_APP_NET_OPENING};
    std::array<AppNetworkRecord, kRecordCount> records{};
    size_t record_head = 0;
    size_t record_count = 0;
    size_t receive_head = 0;
    size_t receive_used = 0;
    uint8_t* buffer = nullptr; // PSRAM，按 ELF 指定容量分为接收环形缓冲和单条发送槽
    size_t send_length = 0;
    uint32_t send_flags = 0;
    bool send_pending = false;
    std::array<std::string, 8> response_headers;
    ~AppNetworkConnection() { heap_caps_free(buffer); }
};

struct AppNetwork {
    bool closed = false; // 仅 UI 线程访问
    bool foreground = false; // 由宿主前台生命周期更新
    bool used = false; // 已成功接纳过联网请求，恢复前台时沿用保网需求
    bool idle_timer_enabled = false; // 默认不因页面空闲断网
    std::optional<PowerNeedHold> foreground_hold; // 页面保网独立于工作线程的收尾引用
    uint32_t next_id = 0;
    std::array<std::shared_ptr<AppNetworkConnection>, kConnectionCount> connections;
};

struct AppNetworkSocket {
    int fd = -1; // 仅由工作线程持有，离开作用域时关闭
    ~AppNetworkSocket() {
        if (fd >= 0) {
            close(fd);
        }
    }
};

static std::atomic<unsigned> s_network_workers{0}; // 已离页的工作仍计入全局上限
static std::atomic<uint16_t> s_ping_identifier{0}; // 活动 Ping 会话的 ICMP 标识

static bool NetworkUnavailable() {
    const auto phase = SystemState::GetInstance().Phase();
    return phase == SystemPhase::Updating || phase == SystemPhase::Restarting || PowerLifecycleIsStandby() ||
           PowerLifecycleIsStandbyResuming() || PowerLifecycleIsPoweringOff();
}

static bool NetworkCancelled(const AppNetworkConnection& connection) {
    return connection.cancelled.load() || NetworkUnavailable();
}

static uint32_t SaturatingAdd(uint32_t value, size_t bytes) {
    return value + std::min<size_t>(bytes, UINT32_MAX - value);
}

static void SetFailure(AppNetworkConnection& connection, int result, int code, const char* stage) {
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(connection.mutex);
        if (connection.status.result == XZ_APP_OK) {
            connection.status.state = XZ_APP_NET_ERROR;
            connection.status.result = result;
            connection.status.error_code = code;
            first = true;
        }
    }
    if (first && !NetworkCancelled(connection)) {
        const esp_log_level_t level = result == XZ_APP_ERR_MEMORY || !std::strcmp(stage, "exception") ? ESP_LOG_ERROR : ESP_LOG_WARN;
        ESP_LOG_LEVEL_LOCAL(level, TAG, "operation=connection id=%lu protocol=%lu stage=%s result=%d code=%d",
                 static_cast<unsigned long>(connection.id), static_cast<unsigned long>(connection.protocol), stage, result, code);
    }
}

static bool QueueReceived(AppNetworkConnection& connection, const void* data, size_t length, uint32_t flags) {
    if (NetworkCancelled(connection)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(connection.mutex);
    if (connection.status.result != XZ_APP_OK) {
        return false;
    }
    const bool stream = connection.protocol == XZ_APP_NET_TCP || connection.protocol == XZ_APP_NET_TLS;
    if (stream && !length) {
        return true;
    }
    const bool merge = stream && connection.record_count != 0;
    const bool record_limit = !merge && connection.record_count == kRecordCount;
    if (record_limit || length > connection.receive_capacity - connection.receive_used) {
        connection.status.state = XZ_APP_NET_ERROR;
        connection.status.result = XZ_APP_ERR_LIMIT;
        ESP_LOGW(TAG, "operation=receive id=%lu result=buffer_limit reason=%s bytes=%u used=%u capacity=%lu records=%u",
                 static_cast<unsigned long>(connection.id), record_limit ? "records" : "bytes",
                 static_cast<unsigned>(length), static_cast<unsigned>(connection.receive_used),
                 static_cast<unsigned long>(connection.receive_capacity), static_cast<unsigned>(connection.record_count));
        return false;
    }
    const size_t offset = (connection.receive_head + connection.receive_used) % connection.receive_capacity;
    const size_t first = std::min<size_t>(length, connection.receive_capacity - offset);
    if (first) {
        std::memcpy(connection.buffer + offset, data, first);
    }
    if (length > first) {
        std::memcpy(connection.buffer, static_cast<const uint8_t*>(data) + first, length - first);
    }
    if (merge) {
        // TCP/TLS 不保留底层回调边界，避免短 TLS 记录提前耗尽记录槽。
        auto& record = connection.records[(connection.record_head + connection.record_count - 1) % kRecordCount];
        record.length += static_cast<uint32_t>(length);
    } else {
        connection.records[(connection.record_head + connection.record_count) % kRecordCount] = {
            static_cast<uint32_t>(length), flags};
        ++connection.record_count;
    }
    connection.receive_used += length;
    connection.status.received_bytes = SaturatingAdd(connection.status.received_bytes, length);
    return true;
}

static void MarkConnected(AppNetworkConnection& connection) {
    std::lock_guard<std::mutex> lock(connection.mutex);
    if (connection.status.result == XZ_APP_OK && connection.status.state == XZ_APP_NET_OPENING) {
        connection.status.state = XZ_APP_NET_CONNECTED;
    }
}

static void MarkDisconnected(AppNetworkConnection& connection) {
    std::lock_guard<std::mutex> lock(connection.mutex);
    if (connection.status.result == XZ_APP_OK) {
        connection.status.state = XZ_APP_NET_CLOSED;
    }
}

static void RunHttp(AppNetworkConnection& connection, NetworkInterface& network) {
    auto http = network.CreateHttp();
    if (!http) {
        SetFailure(connection, XZ_APP_ERR_UNSUPPORTED, 0, "create_http");
        return;
    }
    DownloadGate::Binding binding(&connection.gate, http.get(), true);
    if (!binding || NetworkCancelled(connection)) {
        return;
    }
    if (!connection.ca_pem.empty() && !http->SetCaCertificate(connection.ca_pem)) {
        SetFailure(connection, XZ_APP_ERR_UNSUPPORTED, 0, "http_ca");
        return;
    }
    if (!http->SetReceiveBufferSize(connection.receive_capacity)) {
        SetFailure(connection, XZ_APP_ERR_UNSUPPORTED, 0, "http_buffer");
        return;
    }
    const int64_t deadline = connection.deadline_us;
    http->SetTimeout(connection.timeout_ms);
    http->SetRequestDeadline(deadline);
    http->SetKeepAlive(false);
    for (uint32_t index = 0; index < connection.header_count; ++index) {
        http->SetHeader(connection.headers[index].first, connection.headers[index].second);
    }
    // 显式设置空正文，避免 POST/PUT 被解释为应用尚未结束的流式上传。
    const size_t body_length = connection.body.size();
    http->SetContent(std::move(connection.body));
    if (!http->Open(connection.method, connection.address)) {
        SetFailure(connection, esp_timer_get_time() >= deadline ? XZ_APP_ERR_TIMEOUT : XZ_APP_ERR_IO,
                   http->GetLastError(), "http_open");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(connection.mutex);
        connection.status.sent_bytes = body_length;
    }
    const int status = http->GetStatusCode();
    if (status < 100 || status > 599) {
        const int code = http->GetLastError();
        SetFailure(connection, code == ETIMEDOUT || esp_timer_get_time() >= deadline ? XZ_APP_ERR_TIMEOUT : XZ_APP_ERR_IO,
                   code, "http_headers");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(connection.mutex);
        connection.status.http_status = status;
        for (size_t index = 0; index < connection.response_headers.size(); ++index) {
            auto value = http->GetResponseHeader(kResponseHeaders[index]);
            if (value.size() > 1024) {
                connection.status.result = XZ_APP_ERR_LIMIT;
                connection.status.state = XZ_APP_NET_ERROR;
                ESP_LOGW(TAG, "operation=http_headers id=%lu result=header_limit", static_cast<unsigned long>(connection.id));
                return;
            }
            connection.response_headers[index] = std::move(value);
        }
    }
    MarkConnected(connection);
    char chunk[1024];
    const size_t chunk_size = std::min<size_t>(sizeof(chunk), connection.receive_capacity);
    uint64_t total = 0;
    while (!NetworkCancelled(connection)) {
        if (esp_timer_get_time() >= deadline) {
            SetFailure(connection, XZ_APP_ERR_TIMEOUT, ETIMEDOUT, "http_read");
            return;
        }
        // HTTP 可以背压，给前台读取留出空间；超时仍使用同一绝对期限。
        bool space = false;
        {
            std::lock_guard<std::mutex> lock(connection.mutex);
            space = connection.record_count < kRecordCount && connection.receive_capacity - connection.receive_used >= chunk_size;
        }
        if (!space) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        const int length = http->Read(chunk, chunk_size);
        if (length < 0) {
            const int code = http->GetLastError();
            SetFailure(connection, code == ETIMEDOUT || esp_timer_get_time() >= deadline ? XZ_APP_ERR_TIMEOUT : XZ_APP_ERR_IO,
                       code, "http_read");
            return;
        }
        if (!length) {
            std::lock_guard<std::mutex> lock(connection.mutex);
            connection.status.state = XZ_APP_NET_COMPLETE;
            return;
        }
        if (uint64_t(length) > UINT64_MAX - total ||
            (connection.response_limit && uint64_t(length) > connection.response_limit - total)) {
            SetFailure(connection, XZ_APP_ERR_LIMIT, 0, "http_body");
            return;
        }
        total += length;
        if (!QueueReceived(connection, chunk, length, XZ_APP_NET_BINARY)) {
            return;
        }
    }
}

static void RunConnection(AppNetworkConnection& connection, NetworkInterface& network) {
    std::unique_ptr<Tcp> tcp;
    std::unique_ptr<Udp> udp;
    std::unique_ptr<WebSocket> websocket;
    const int64_t deadline = connection.deadline_us;
    bool connected = false;
    if (connection.protocol == XZ_APP_NET_WEBSOCKET) {
        websocket = network.CreateWebSocket();
        if (websocket) {
            websocket->SetReceiveBufferSize(connection.receive_capacity);
            websocket->SetRequestDeadline(deadline);
            websocket->SetCaCertificate(connection.ca_pem);
            for (uint32_t index = 0; index < connection.header_count; ++index) {
                websocket->SetHeader(connection.headers[index].first.c_str(), connection.headers[index].second.c_str());
            }
            websocket->OnData([&connection](const char* data, size_t length, bool binary) {
                QueueReceived(connection, data, length, binary ? XZ_APP_NET_BINARY : 0);
            });
            websocket->OnDisconnected([&connection]() { MarkDisconnected(connection); });
            websocket->OnError([&connection](int code) { SetFailure(connection, XZ_APP_ERR_IO, code, "websocket_receive"); });
            connected = websocket->Connect(connection.address.c_str());
        }
    } else if (connection.protocol == XZ_APP_NET_UDP) {
        udp = network.CreateUdp();
        if (udp) {
            if (!udp->SetReceiveBufferSize(connection.receive_capacity)) {
                SetFailure(connection, XZ_APP_ERR_UNSUPPORTED, 0, "udp_buffer");
                return;
            }
            udp->OnMessage([&connection](const std::string& data) {
                QueueReceived(connection, data.data(), data.size(), XZ_APP_NET_BINARY);
            });
            connected = udp->Connect(connection.address, connection.port);
        }
    } else {
        tcp = connection.protocol == XZ_APP_NET_TLS ? network.CreateSsl() : network.CreateTcp();
        if (tcp) {
            tcp->SetRequestDeadline(deadline);
            if (!connection.ca_pem.empty() && !tcp->SetCaCertificate(connection.ca_pem)) {
                SetFailure(connection, XZ_APP_ERR_UNSUPPORTED, 0, "tls_ca");
                return;
            }
            tcp->OnStream([&connection](const std::string& data) {
                QueueReceived(connection, data.data(), data.size(), XZ_APP_NET_BINARY);
            });
            tcp->OnDisconnected([&connection]() { MarkDisconnected(connection); });
            connected = tcp->Connect(connection.address, connection.port);
        }
    }
    if (!connected) {
        const int code = tcp ? tcp->GetLastError() : udp ? udp->GetLastError() : websocket ? websocket->GetLastError() : 0;
        SetFailure(connection, code == ENOMEM ? XZ_APP_ERR_MEMORY :
                   esp_timer_get_time() >= deadline ? XZ_APP_ERR_TIMEOUT : XZ_APP_ERR_IO, code, "connect");
        return;
    }
    MarkConnected(connection);
    while (!NetworkCancelled(connection)) {
        std::string payload;
        uint32_t flags = 0;
        bool pending = false;
        {
            std::lock_guard<std::mutex> lock(connection.mutex);
            if (connection.status.state != XZ_APP_NET_CONNECTED || connection.status.result != XZ_APP_OK) {
                break;
            }
            pending = connection.send_pending;
            if (pending) {
                payload.assign(reinterpret_cast<char*>(connection.buffer + connection.receive_capacity), connection.send_length);
                flags = connection.send_flags;
            }
        }
        if (pending) {
            int sent = -1;
            const int64_t send_deadline = esp_timer_get_time() + int64_t(connection.timeout_ms) * 1000;
            if (tcp) {
                tcp->SetRequestDeadline(send_deadline);
                sent = tcp->Send(payload);
            } else if (udp) {
                sent = udp->Send(payload);
            } else {
                websocket->SetRequestDeadline(send_deadline);
                if (websocket->Send(payload.data(), payload.size(), (flags & XZ_APP_NET_BINARY) != 0)) {
                    sent = payload.size();
                }
            }
            if (sent != static_cast<int>(payload.size())) {
                const int code = tcp ? tcp->GetLastError() : udp ? udp->GetLastError() : websocket->GetLastError();
                SetFailure(connection, esp_timer_get_time() >= send_deadline ? XZ_APP_ERR_TIMEOUT : XZ_APP_ERR_IO, code, "send");
                break;
            }
            std::lock_guard<std::mutex> lock(connection.mutex);
            connection.send_pending = false;
            connection.status.sent_bytes = SaturatingAdd(connection.status.sent_bytes, payload.size());
        }
        if ((tcp && !tcp->connected()) || (websocket && !websocket->IsConnected())) {
            MarkDisconnected(connection);
            break;
        }
        if (udp && !udp->connected()) {
            SetFailure(connection, XZ_APP_ERR_IO, udp->GetLastError(), "udp_receive");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    // 先等待底层接收任务退出，再释放其回调引用的宿主状态。
    if (tcp) {
        tcp->Disconnect();
    }
    if (udp) {
        udp->Disconnect();
    }
    websocket.reset();
}

static void UpdateProbeElapsed(AppNetworkConnection& connection, int64_t started) {
    std::lock_guard<std::mutex> lock(connection.mutex);
    connection.diagnostics.elapsed_ms = static_cast<uint32_t>(std::min<int64_t>(
        (esp_timer_get_time() - started) / 1000, UINT32_MAX));
}

static bool ResolveProbeAddress(AppNetworkConnection& connection, sockaddr_in& target) {
    const int64_t started = esp_timer_get_time();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const int code = getaddrinfo(connection.address.c_str(), nullptr, &hints, &addresses);
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> owner(addresses, freeaddrinfo);
    UpdateProbeElapsed(connection, started);
    if (NetworkCancelled(connection)) {
        return false;
    }
    if (esp_timer_get_time() >= connection.deadline_us) {
        SetFailure(connection, XZ_APP_ERR_TIMEOUT, ETIMEDOUT, "dns");
        return false;
    }
    if (code) {
        SetFailure(connection, code == EAI_MEMORY ? XZ_APP_ERR_MEMORY : XZ_APP_ERR_IO, code, "dns");
        return false;
    }
    for (auto* address = addresses; address; address = address->ai_next) {
        if (address->ai_family == AF_INET && address->ai_addr && address->ai_addrlen >= sizeof(target)) {
            std::memcpy(&target, address->ai_addr, sizeof(target));
            target.sin_port = 0;
            std::lock_guard<std::mutex> lock(connection.mutex);
            if (!inet_ntop(AF_INET, &target.sin_addr, connection.diagnostics.address,
                           sizeof(connection.diagnostics.address))) {
                break;
            }
            return true;
        }
    }
    SetFailure(connection, XZ_APP_ERR_IO, EAI_NONAME, "dns_address");
    return false;
}

static bool PingReplyMatches(const uint8_t* packet, size_t length, size_t payload_size,
                             uint16_t identifier, uint16_t sequence, uint32_t& ttl) {
    if (length < 28 || (packet[0] >> 4) != 4) {
        return false;
    }
    const size_t header_size = (packet[0] & 0x0f) * 4;
    if (header_size < 20 || header_size > 60 || length != header_size + sizeof(icmp_echo_hdr) + payload_size) {
        return false;
    }
    const auto* reply = reinterpret_cast<const icmp_echo_hdr*>(packet + header_size);
    if (reply->type != ICMP_ER || reply->code || reply->id != htons(identifier) || reply->seqno != htons(sequence) ||
        inet_chksum(packet + header_size, static_cast<uint16_t>(length - header_size)) != 0) {
        return false;
    }
    for (size_t index = 0; index < payload_size; ++index) {
        if (packet[header_size + sizeof(icmp_echo_hdr) + index] != static_cast<uint8_t>(index + sequence)) {
            return false;
        }
    }
    ttl = packet[8];
    return true;
}

static void RunPing(AppNetworkConnection& connection, const sockaddr_in& target) {
    if (NetworkCancelled(connection)) {
        return;
    }
    const auto& config = connection.ping;
    const size_t packet_size = sizeof(icmp_echo_hdr) + config.data_size;
    const size_t buffer_size = packet_size + 60;
    connection.buffer = static_cast<uint8_t*>(heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!connection.buffer) {
        SetFailure(connection, XZ_APP_ERR_MEMORY, ENOMEM, "ping_buffer");
        return;
    }
    AppNetworkSocket socket;
    socket.fd = ::socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (socket.fd < 0) {
        SetFailure(connection, XZ_APP_ERR_IO, errno, "ping_socket");
        return;
    }
    const int ttl = static_cast<int>(config.ttl);
    timeval write_timeout{};
    write_timeout.tv_usec = 100000;
    if (setsockopt(socket.fd, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) != 0 ||
        setsockopt(socket.fd, SOL_SOCKET, SO_SNDTIMEO, &write_timeout, sizeof(write_timeout)) != 0) {
        SetFailure(connection, XZ_APP_ERR_IO, errno, "ping_options");
        return;
    }
    const uint16_t identifier = ++s_ping_identifier;
    uint64_t attempts = 0;
    uint64_t rtt_sum = 0;
    uint64_t replies = 0;
    const int64_t started = esp_timer_get_time();
    MarkConnected(connection);
    while (!NetworkCancelled(connection) && (!config.count || attempts < config.count)) {
        const uint16_t sequence = static_cast<uint16_t>((attempts % UINT16_MAX) + 1);
        auto* request = reinterpret_cast<icmp_echo_hdr*>(connection.buffer);
        std::memset(request, 0, sizeof(*request));
        request->type = ICMP_ECHO;
        request->id = htons(identifier);
        request->seqno = htons(sequence);
        for (size_t index = 0; index < config.data_size; ++index) {
            connection.buffer[sizeof(*request) + index] = static_cast<uint8_t>(index + sequence);
        }
        request->chksum = inet_chksum(request, static_cast<uint16_t>(packet_size));
        const int64_t sent_at = esp_timer_get_time();
        const int sent = sendto(socket.fd, connection.buffer, packet_size, 0,
                                reinterpret_cast<const sockaddr*>(&target), sizeof(target));
        if (sent != static_cast<int>(packet_size)) {
            SetFailure(connection, XZ_APP_ERR_IO, sent < 0 ? errno : EIO, "ping_send");
            return;
        }
        ++attempts;
        {
            std::lock_guard<std::mutex> lock(connection.mutex);
            connection.diagnostics.sent = SaturatingAdd(connection.diagnostics.sent, 1);
            connection.diagnostics.sequence = sequence;
            connection.status.sent_bytes = SaturatingAdd(connection.status.sent_bytes, config.data_size);
        }
        const int64_t deadline = sent_at + int64_t(config.timeout_ms) * 1000;
        bool received = false;
        while (!NetworkCancelled(connection) && esp_timer_get_time() < deadline) {
            const int64_t slice_us = std::min<int64_t>(100000, deadline - esp_timer_get_time());
            if (slice_us <= 0) {
                break;
            }
            timeval timeout{};
            timeout.tv_usec = std::max<int64_t>(1000, slice_us);
            if (setsockopt(socket.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
                SetFailure(connection, XZ_APP_ERR_IO, errno, "ping_timeout_option");
                return;
            }
            sockaddr_in source{};
            socklen_t source_size = sizeof(source);
            const int length = recvfrom(socket.fd, connection.buffer, buffer_size, 0,
                                         reinterpret_cast<sockaddr*>(&source), &source_size);
            if (NetworkCancelled(connection) || esp_timer_get_time() >= deadline) {
                break;
            }
            if (length < 0) {
                const int code = errno;
                if (code == EAGAIN || code == EWOULDBLOCK || code == EINTR || code == ETIMEDOUT) {
                    continue;
                }
                SetFailure(connection, XZ_APP_ERR_IO, code, "ping_receive");
                return;
            }
            uint32_t reply_ttl = 0;
            if (source.sin_addr.s_addr != target.sin_addr.s_addr ||
                !PingReplyMatches(connection.buffer, static_cast<size_t>(length), config.data_size,
                                   identifier, sequence, reply_ttl)) {
                continue;
            }
            const uint32_t rtt = static_cast<uint32_t>(std::min<int64_t>(
                (esp_timer_get_time() - sent_at) / 1000, UINT32_MAX));
            rtt_sum += rtt;
            ++replies;
            {
                std::lock_guard<std::mutex> lock(connection.mutex);
                auto& result = connection.diagnostics;
                result.received = SaturatingAdd(result.received, 1);
                result.ttl = reply_ttl;
                result.last_rtt_ms = rtt;
                result.min_rtt_ms = replies == 1 ? rtt : std::min(result.min_rtt_ms, rtt);
                result.max_rtt_ms = std::max(result.max_rtt_ms, rtt);
                result.average_rtt_ms = static_cast<uint32_t>(rtt_sum / replies);
                connection.status.received_bytes = SaturatingAdd(connection.status.received_bytes, config.data_size);
            }
            received = true;
            break;
        }
        if (!received && !NetworkCancelled(connection)) {
            std::lock_guard<std::mutex> lock(connection.mutex);
            connection.diagnostics.timeouts = SaturatingAdd(connection.diagnostics.timeouts, 1);
        }
        UpdateProbeElapsed(connection, started);
        if (config.count && attempts >= config.count) {
            break;
        }
        const int64_t next_start = sent_at + int64_t(config.interval_ms) * 1000;
        while (!NetworkCancelled(connection) && esp_timer_get_time() < next_start) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    UpdateProbeElapsed(connection, started);
    if (!NetworkCancelled(connection)) {
        if (!replies) {
            SetFailure(connection, XZ_APP_ERR_TIMEOUT, ETIMEDOUT, "ping_reply");
        } else {
            std::lock_guard<std::mutex> lock(connection.mutex);
            connection.status.state = XZ_APP_NET_COMPLETE;
        }
    }
}

static void NetworkWorker(void* argument) {
    auto connection = std::move(*static_cast<std::shared_ptr<AppNetworkConnection>*>(argument));
    delete static_cast<std::shared_ptr<AppNetworkConnection>*>(argument);
    {
        PowerNeedHold hold(PowerNeed::ForegroundNetwork);
        try {
            auto& board = Board::GetInstance();
            if (!NetworkCancelled(*connection)) {
                const int remaining_ms = static_cast<int>(std::max<int64_t>(0, (connection->deadline_us - esp_timer_get_time()) / 1000));
                if (!remaining_ms) {
                    SetFailure(*connection, XZ_APP_ERR_TIMEOUT, ETIMEDOUT, "network_ready");
                } else if (board.GetNetworkErrorText(nullptr) || !board.EnsureNetworkReady(remaining_ms)) {
                    const bool timed_out = esp_timer_get_time() >= connection->deadline_us;
                    SetFailure(*connection, timed_out ? XZ_APP_ERR_TIMEOUT : XZ_APP_ERR_NOT_READY,
                               timed_out ? ETIMEDOUT : 0, "network_ready");
                } else if (!NetworkCancelled(*connection)) {
                    if (esp_timer_get_time() >= connection->deadline_us) {
                        SetFailure(*connection, XZ_APP_ERR_TIMEOUT, ETIMEDOUT, "network_ready");
                    } else if (!connection->protocol) {
                        std::lock_guard<std::mutex> lock(connection->mutex);
                        connection->status.state = XZ_APP_NET_COMPLETE;
                    } else if (connection->protocol == kNetworkResolve || connection->protocol == kNetworkPing) {
                        sockaddr_in target{};
                        if (ResolveProbeAddress(*connection, target)) {
                            if (connection->protocol == kNetworkPing) {
                                RunPing(*connection, target);
                            } else {
                                std::lock_guard<std::mutex> lock(connection->mutex);
                                connection->status.state = XZ_APP_NET_COMPLETE;
                            }
                        }
                    } else {
                        auto* network = board.GetNetwork();
                        if (!network) {
                            SetFailure(*connection, XZ_APP_ERR_NOT_READY, 0, "network_backend");
                        } else if (connection->protocol == XZ_APP_NET_HTTP) {
                            RunHttp(*connection, *network);
                        } else {
                            RunConnection(*connection, *network);
                        }
                    }
                }
            }
        } catch (const std::bad_alloc&) {
            SetFailure(*connection, XZ_APP_ERR_MEMORY, ENOMEM, "allocate");
        } catch (...) {
            SetFailure(*connection, XZ_APP_ERR_IO, 0, "exception");
        }
        {
            std::lock_guard<std::mutex> lock(connection->mutex);
            connection->send_pending = false;
            if (NetworkCancelled(*connection)) {
                connection->status.state = XZ_APP_NET_CLOSED;
                connection->status.result = XZ_APP_ERR_CANCELLED;
            }
        }
        connection->running.store(false);
        ESP_LOGI(TAG, "operation=connection id=%lu result=finished", static_cast<unsigned long>(connection->id));
    }
    connection.reset();
    --s_network_workers;
    vTaskDeleteWithCaps(nullptr);
}

static std::shared_ptr<AppNetworkConnection> FindConnection(const std::shared_ptr<AppNetwork>& state, uint32_t id) {
    if (state && !state->closed && id) {
        for (const auto& connection : state->connections) {
            if (connection && connection->id == id) {
                return connection;
            }
        }
    }
    return {};
}

static int StartNetworkWorker(const std::shared_ptr<AppNetwork>& state,
                              const std::shared_ptr<AppNetworkConnection>& connection, uint32_t* id) {
    auto slot = std::find(state->connections.begin(), state->connections.end(), nullptr);
    if (slot == state->connections.end() || state->next_id == UINT32_MAX) {
        return XZ_APP_ERR_LIMIT;
    }
    auto* owner = new (std::nothrow) std::shared_ptr<AppNetworkConnection>(connection);
    if (!owner) {
        ESP_LOGE(TAG, "operation=open stage=owner result=no_memory");
        return XZ_APP_ERR_MEMORY;
    }
    if (s_network_workers.fetch_add(1) >= kConnectionCount) {
        --s_network_workers;
        delete owner;
        return XZ_APP_ERR_BUSY;
    }
    // 计入活动数后再检查升级状态，保证升级排空能看到此前已接纳的工作。
    if (NetworkUnavailable()) {
        --s_network_workers;
        delete owner;
        return XZ_APP_ERR_NOT_READY;
    }
    connection->id = ++state->next_id;
    if (xTaskCreatePinnedToCoreWithCaps(NetworkWorker, "app_network", kNetworkStackBytes, owner, 2, nullptr, tskNO_AFFINITY,
                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        --s_network_workers;
        delete owner;
        ESP_LOGE(TAG, "operation=open stage=task result=no_memory");
        return XZ_APP_ERR_MEMORY;
    }
    *slot = connection;
    *id = connection->id;
    state->used = true;
    AppNetworkForeground(state, true);
    const char* operation = "open";
    if (!connection->protocol) {
        operation = "prepare";
    } else if (connection->protocol == kNetworkResolve) {
        operation = "resolve";
    } else if (connection->protocol == kNetworkPing) {
        operation = "ping";
    }
    ESP_LOGI(TAG, "operation=%s id=%lu protocol=%lu", operation,
             static_cast<unsigned long>(*id), static_cast<unsigned long>(connection->protocol));
    return XZ_APP_OK;
}

static void CancelConnection(const std::shared_ptr<AppNetworkConnection>& connection) {
    connection->cancelled.store(true);
    connection->gate.RequestCancel();
    if (connection->protocol == XZ_APP_NET_HTTP && connection->running.load()) {
        bool scheduled = false;
        try {
            scheduled = TaskExecutor::Schedule([connection]() { connection->gate.AbortBoundHttp(); }, TaskDomain::Cleanup);
        } catch (const std::bad_alloc&) {
            // 回调封装也可能在进入调度器前分配失败，原工作仍按总期限收尾。
        }
        if (!scheduled) {
            ESP_LOGW(TAG, "operation=cancel id=%lu result=deferred_to_deadline", static_cast<unsigned long>(connection->id));
        }
    }
}

static bool BoundedText(const char* value, size_t limit, bool empty = false) {
    return value && strnlen(value, limit + 1) <= limit && (empty || *value);
}

static bool HeaderName(const char* name) {
    if (!BoundedText(name, 64)) {
        return false;
    }
    const char* punctuation = "!#$%&'*+-.^_`|~";
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p) {
        if (!std::isalnum(*p) && !std::strchr(punctuation, *p)) {
            return false;
        }
    }
    return true;
}

static std::string Lowercase(std::string value) {
    for (char& c : value) {
        c = std::tolower(static_cast<unsigned char>(c));
    }
    return value;
}

static bool ValidConfig(const xz_app_net_config_t& config) {
    if (config.struct_size < sizeof(config) || config.protocol < XZ_APP_NET_HTTP || config.protocol > XZ_APP_NET_UDP ||
        config.timeout_ms < 1000 || config.timeout_ms > 60000 || !BoundedText(config.address, 1024) ||
        config.header_count > 8 || (config.header_count && !config.headers) ||
        !config.receive_buffer_size || config.receive_buffer_size > INT_MAX || config.send_buffer_size > INT_MAX ||
        config.body_length > config.send_buffer_size || (config.body_length && !config.body) || (config.ca_pem && !BoundedText(config.ca_pem, 8192))) {
        return false;
    }
    if ((config.protocol == XZ_APP_NET_WEBSOCKET && (config.receive_buffer_size > 65535 || config.send_buffer_size > 65535)) ||
        (config.protocol == XZ_APP_NET_UDP && (config.receive_buffer_size > 65507 || config.send_buffer_size > 65507)) ||
        (config.protocol != XZ_APP_NET_HTTP && config.response_size_limit)) {
        return false;
    }
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(config.address); *p; ++p) {
        if (*p <= 32 || *p == 127 || *p == '#' || *p == '@') {
            return false;
        }
    }
    const bool url = config.protocol == XZ_APP_NET_HTTP || config.protocol == XZ_APP_NET_WEBSOCKET;
    if (url) {
        const char* separator = std::strstr(config.address, "://");
        if (!separator || config.port) {
            return false;
        }
        const std::string scheme(config.address, separator);
        if ((config.protocol == XZ_APP_NET_HTTP && scheme != "http" && scheme != "https") ||
            (config.protocol == XZ_APP_NET_WEBSOCKET && scheme != "ws" && scheme != "wss")) {
            return false;
        }
        const char* authority = separator + 3;
        const size_t length = std::strcspn(authority, "/?");
        if (!length || length > 259 || std::memchr(authority, '[', length) || std::memchr(authority, ']', length)) {
            return false;
        }
        const char* colon = static_cast<const char*>(std::memchr(authority, ':', length));
        if (colon) {
            uint32_t port = 0;
            if (colon == authority || colon + 1 == authority + length) {
                return false;
            }
            for (const char* p = colon + 1; p < authority + length; ++p) {
                if (*p < '0' || *p > '9' || port > 6553) {
                    return false;
                }
                port = port * 10 + (*p - '0');
            }
            if (!port || port > 65535) {
                return false;
            }
        }
    } else if (!config.port || config.port > 65535 || !BoundedText(config.address, 253) ||
               std::strpbrk(config.address, "/:\\?[]") || config.header_count || config.method || config.body_length) {
        return false;
    }
    if (config.protocol == XZ_APP_NET_HTTP) {
        if (!BoundedText(config.method, 6) ||
            (std::strcmp(config.method, "GET") && std::strcmp(config.method, "POST") && std::strcmp(config.method, "PUT") &&
             std::strcmp(config.method, "PATCH") && std::strcmp(config.method, "DELETE") && std::strcmp(config.method, "HEAD"))) {
            return false;
        }
    } else if (config.method || config.body_length) {
        return false;
    }
    for (uint32_t index = 0; index < config.header_count; ++index) {
        const auto& header = config.headers[index];
        if (!HeaderName(header.name) || !BoundedText(header.value, 512, true) || std::strpbrk(header.value, "\r\n")) {
            return false;
        }
        const auto name = Lowercase(header.name);
        if (name == "host" || name == "content-length" || name == "transfer-encoding" || name == "connection" ||
            name == "upgrade" || name.rfind("sec-websocket-", 0) == 0) {
            return false;
        }
    }
    return true;
}

static int StartNetworkProbe(const std::shared_ptr<AppNetwork>& state, uint32_t protocol, const char* hostname,
                              uint32_t timeout_ms, const xz_app_ping_config_t* ping, uint32_t* id) {
    if (!state || state->closed || NetworkUnavailable()) {
        return XZ_APP_ERR_NOT_READY;
    }
    if (!id) {
        return XZ_APP_ERR_INVALID;
    }
    *id = 0;
    if (timeout_ms < 1000 || timeout_ms > 60000 || !BoundedText(hostname, 253)) {
        return XZ_APP_ERR_INVALID;
    }
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(hostname); *p; ++p) {
        if (*p >= 128 || (!std::isalnum(*p) && *p != '.' && *p != '-' && *p != '_')) {
            return XZ_APP_ERR_INVALID;
        }
    }
    try {
        auto connection = std::make_shared<AppNetworkConnection>();
        connection->protocol = protocol;
        connection->address = hostname;
        connection->timeout_ms = timeout_ms;
        connection->deadline_us = esp_timer_get_time() + int64_t(timeout_ms) * 1000;
        if (ping) {
            connection->ping = *ping;
            connection->ping.address = nullptr;
        }
        return StartNetworkWorker(state, connection, id);
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "operation=probe stage=allocate result=no_memory");
        return XZ_APP_ERR_MEMORY;
    }
}

std::shared_ptr<AppNetwork> AppNetworkCreate() {
    try {
        return std::make_shared<AppNetwork>();
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "operation=create result=no_memory");
        return {};
    }
}

void AppNetworkStop(const std::shared_ptr<AppNetwork>& state, bool closed) {
    if (!state) {
        return;
    }
    state->closed |= closed;
    AppNetworkForeground(state, false);
    for (auto& connection : state->connections) {
        if (connection) {
            CancelConnection(connection);
            connection.reset();
        }
    }
}

void AppNetworkForeground(const std::shared_ptr<AppNetwork>& state, bool foreground) {
    if (!state) {
        return;
    }
    state->foreground = foreground && !state->closed && !NetworkUnavailable();
    const bool keep_network = state->foreground && state->used && !state->idle_timer_enabled;
    if (keep_network && !state->foreground_hold) {
        state->foreground_hold.emplace(PowerNeed::ForegroundNetwork);
    } else if (!keep_network) {
        state->foreground_hold.reset();
    }
}

int AppNetworkSetIdleTimer(const std::shared_ptr<AppNetwork>& state, int enabled) {
    if (!state || state->closed) {
        return XZ_APP_ERR_NOT_READY;
    }
    if (enabled != 0 && enabled != 1) {
        return XZ_APP_ERR_INVALID;
    }
    if (state->idle_timer_enabled != (enabled != 0)) {
        state->idle_timer_enabled = enabled != 0;
        AppNetworkForeground(state, state->foreground);
        ESP_LOGI(TAG, "operation=idle_timer enabled=%d", enabled);
    }
    return XZ_APP_OK;
}

int AppNetworkPrepare(const std::shared_ptr<AppNetwork>& state, uint32_t timeout_ms, uint32_t* id) {
    if (!state || state->closed || NetworkUnavailable()) {
        return XZ_APP_ERR_NOT_READY;
    }
    if (!id) {
        return XZ_APP_ERR_INVALID;
    }
    *id = 0;
    if (timeout_ms < 1000 || timeout_ms > 60000) {
        return XZ_APP_ERR_INVALID;
    }
    try {
        auto connection = std::make_shared<AppNetworkConnection>();
        connection->timeout_ms = timeout_ms;
        connection->deadline_us = esp_timer_get_time() + int64_t(timeout_ms) * 1000;
        return StartNetworkWorker(state, connection, id);
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "operation=prepare stage=allocate result=no_memory");
        return XZ_APP_ERR_MEMORY;
    }
}

int AppNetworkResolve(const std::shared_ptr<AppNetwork>& state, const char* hostname, uint32_t timeout_ms, uint32_t* id) {
    return StartNetworkProbe(state, kNetworkResolve, hostname, timeout_ms, nullptr, id);
}

int AppNetworkPing(const std::shared_ptr<AppNetwork>& state, const xz_app_ping_config_t* config, uint32_t* id) {
    if (!config || config->struct_size < sizeof(*config) ||
        !config->timeout_ms || config->data_size > 65507 || !config->ttl || config->ttl > 255) {
        return XZ_APP_ERR_INVALID;
    }
    return StartNetworkProbe(state, kNetworkPing, config->address, config->network_timeout_ms, config, id);
}

int AppNetworkDiagnostics(const std::shared_ptr<AppNetwork>& state, uint32_t id, xz_app_net_diagnostics_t* result) {
    auto connection = FindConnection(state, id);
    if (!connection) {
        return XZ_APP_ERR_NOT_FOUND;
    }
    if (connection->protocol != kNetworkResolve && connection->protocol != kNetworkPing) {
        return XZ_APP_ERR_UNSUPPORTED;
    }
    if (!result || result->struct_size < sizeof(uint32_t)) {
        return XZ_APP_ERR_INVALID;
    }
    const size_t capacity = std::min<size_t>(result->struct_size, sizeof(*result));
    std::lock_guard<std::mutex> lock(connection->mutex);
    auto diagnostics = connection->diagnostics;
    diagnostics.struct_size = sizeof(diagnostics);
    std::memcpy(result, &diagnostics, capacity);
    return XZ_APP_OK;
}

int AppNetworkOpen(const std::shared_ptr<AppNetwork>& state, const xz_app_net_config_t* config, uint32_t* id) {
    if (!state || state->closed || NetworkUnavailable()) {
        return XZ_APP_ERR_NOT_READY;
    }
    if (!config || !id) {
        return XZ_APP_ERR_INVALID;
    }
    *id = 0;
    try {
        if (!ValidConfig(*config)) {
            return XZ_APP_ERR_INVALID;
        }
        const uint64_t receive_bytes = config->receive_buffer_size;
        const uint64_t send_bytes = config->protocol == XZ_APP_NET_HTTP ? 0 : config->send_buffer_size;
        const uint64_t buffer_bytes = receive_bytes + send_bytes;
        if (buffer_bytes > SIZE_MAX) {
            return XZ_APP_ERR_LIMIT;
        }
        auto connection = std::make_shared<AppNetworkConnection>();
        connection->receive_capacity = config->receive_buffer_size;
        connection->send_capacity = static_cast<uint32_t>(send_bytes);
        connection->response_limit = config->response_size_limit;
        connection->buffer = static_cast<uint8_t*>(heap_caps_malloc(static_cast<size_t>(buffer_bytes), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!connection->buffer) {
            ESP_LOGE(TAG, "operation=open stage=buffer result=no_memory");
            return XZ_APP_ERR_MEMORY;
        }
        connection->protocol = config->protocol;
        connection->port = config->port;
        connection->timeout_ms = config->timeout_ms;
        connection->deadline_us = esp_timer_get_time() + int64_t(config->timeout_ms) * 1000;
        connection->address = config->address;
        connection->method = config->method ? config->method : "";
        connection->ca_pem = config->ca_pem ? config->ca_pem : "";
        if (config->body_length) {
            connection->body.assign(static_cast<const char*>(config->body), config->body_length);
        }
        connection->header_count = config->header_count;
        for (uint32_t index = 0; index < config->header_count; ++index) {
            connection->headers[index] = {config->headers[index].name, config->headers[index].value};
        }
        return StartNetworkWorker(state, connection, id);
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "operation=open stage=copy result=no_memory");
        return XZ_APP_ERR_MEMORY;
    }
}

int AppNetworkSend(const std::shared_ptr<AppNetwork>& state, uint32_t id, const void* data,
                   uint32_t length, uint32_t flags) {
    auto connection = FindConnection(state, id);
    if (!connection || NetworkCancelled(*connection)) {
        return XZ_APP_ERR_NOT_READY;
    }
    if (!connection->protocol || connection->protocol == XZ_APP_NET_HTTP || connection->protocol >= kNetworkResolve) {
        return XZ_APP_ERR_UNSUPPORTED;
    }
    if ((!data && length) || length > connection->send_capacity || (flags & ~XZ_APP_NET_BINARY)) {
        return XZ_APP_ERR_INVALID;
    }
    std::lock_guard<std::mutex> lock(connection->mutex);
    if (connection->status.state != XZ_APP_NET_CONNECTED) {
        return XZ_APP_ERR_NOT_READY;
    }
    if (connection->send_pending) {
        return XZ_APP_ERR_BUSY;
    }
    if (length) {
        std::memcpy(connection->buffer + connection->receive_capacity, data, length);
    }
    connection->send_length = length;
    connection->send_flags = flags;
    connection->send_pending = true;
    return XZ_APP_OK;
}

int AppNetworkStatus(const std::shared_ptr<AppNetwork>& state, uint32_t id, xz_app_net_status_t* result) {
    auto connection = FindConnection(state, id);
    if (!connection) {
        return XZ_APP_ERR_NOT_FOUND;
    }
    if (!result || result->struct_size < sizeof(uint32_t)) {
        return XZ_APP_ERR_INVALID;
    }
    const size_t capacity = std::min<size_t>(result->struct_size, sizeof(*result));
    std::lock_guard<std::mutex> lock(connection->mutex);
    auto status = connection->status;
    status.pending_send = connection->send_pending;
    status.queued_messages = connection->record_count;
    status.next_length = connection->record_count ? connection->records[connection->record_head].length : 0;
    status.worker_running = connection->running.load();
    std::memcpy(result, &status, capacity);
    return XZ_APP_OK;
}

int AppNetworkRead(const std::shared_ptr<AppNetwork>& state, uint32_t id, void* buffer,
                   uint32_t capacity, uint32_t* flags) {
    auto connection = FindConnection(state, id);
    if (!connection) {
        return XZ_APP_ERR_NOT_FOUND;
    }
    if (!flags || (!buffer && capacity)) {
        return XZ_APP_ERR_INVALID;
    }
    std::lock_guard<std::mutex> lock(connection->mutex);
    if (!connection->record_count) {
        return XZ_APP_ERR_NOT_READY;
    }
    auto& record = connection->records[connection->record_head];
    const bool stream = connection->protocol == XZ_APP_NET_TCP || connection->protocol == XZ_APP_NET_TLS;
    if (record.length > capacity && (!stream || !capacity)) {
        return XZ_APP_ERR_LIMIT;
    }
    const uint32_t length = std::min(record.length, capacity);
    const size_t first = std::min<size_t>(length, connection->receive_capacity - connection->receive_head);
    if (first) {
        std::memcpy(buffer, connection->buffer + connection->receive_head, first);
    }
    if (length > first) {
        std::memcpy(static_cast<uint8_t*>(buffer) + first, connection->buffer, length - first);
    }
    connection->receive_head = (connection->receive_head + length) % connection->receive_capacity;
    connection->receive_used -= length;
    *flags = record.flags;
    record.length -= length;
    if (!record.length) {
        connection->record_head = (connection->record_head + 1) % kRecordCount;
        --connection->record_count;
    }
    return length;
}

int AppNetworkHeader(const std::shared_ptr<AppNetwork>& state, uint32_t id, const char* name,
                     char* buffer, uint32_t capacity) {
    auto connection = FindConnection(state, id);
    if (!connection) {
        return XZ_APP_ERR_NOT_FOUND;
    }
    if (!HeaderName(name) || !buffer || !capacity) {
        return XZ_APP_ERR_INVALID;
    }
    if (connection->protocol != XZ_APP_NET_HTTP) {
        return XZ_APP_ERR_UNSUPPORTED;
    }
    try {
        const auto lower = Lowercase(name);
        std::lock_guard<std::mutex> lock(connection->mutex);
        if (!connection->status.http_status) {
            return XZ_APP_ERR_NOT_READY;
        }
        for (size_t index = 0; index < connection->response_headers.size(); ++index) {
            if (lower == kResponseHeaders[index]) {
                const auto& value = connection->response_headers[index];
                if (value.empty()) {
                    return XZ_APP_ERR_NOT_FOUND;
                }
                if (value.size() >= capacity) {
                    return XZ_APP_ERR_LIMIT;
                }
                std::memcpy(buffer, value.c_str(), value.size() + 1);
                return value.size();
            }
        }
        return XZ_APP_ERR_UNSUPPORTED;
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "operation=header result=no_memory");
        return XZ_APP_ERR_MEMORY;
    }
}

int AppNetworkClose(const std::shared_ptr<AppNetwork>& state, uint32_t id) {
    if (!state || state->closed) {
        return XZ_APP_ERR_NOT_READY;
    }
    for (auto& connection : state->connections) {
        if (connection && connection->id == id) {
            CancelConnection(connection);
            connection.reset();
            return XZ_APP_OK;
        }
    }
    return XZ_APP_ERR_NOT_FOUND;
}

bool AppNetworkBusy() {
    return s_network_workers.load() != 0;
}
