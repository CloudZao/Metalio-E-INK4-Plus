#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <string>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <psa/crypto.h>
#include "web_socket.h"
#include "network_interface.h"

static constexpr const char* TAG = "WebSocket";

static std::string WebSocketBase64(const unsigned char* data, size_t length) {
    static constexpr const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((length + 2) / 3 * 4);
    for (size_t offset = 0; offset < length; offset += 3) {
        const uint32_t value = (uint32_t(data[offset]) << 16) |
            (offset + 1 < length ? uint32_t(data[offset + 1]) << 8 : 0) |
            (offset + 2 < length ? data[offset + 2] : 0);
        result += alphabet[(value >> 18) & 63];
        result += alphabet[(value >> 12) & 63];
        result += offset + 1 < length ? alphabet[(value >> 6) & 63] : '=';
        result += offset + 2 < length ? alphabet[value & 63] : '=';
    }
    return result;
}

static std::string WebSocketHeader(const std::string& headers, const char* name) {
    size_t start = headers.find("\r\n");
    while (start != std::string::npos) {
        start += 2;
        const size_t end = headers.find("\r\n", start);
        const size_t colon = headers.find(':', start);
        if (end == std::string::npos || end == start) {
            break;
        }
        if (colon != std::string::npos && colon < end) {
            std::string key = headers.substr(start, colon - start);
            for (char& c : key) {
                c = std::tolower(static_cast<unsigned char>(c));
            }
            if (key == name) {
                size_t first = colon + 1;
                size_t last = end;
                while (first < last && (headers[first] == ' ' || headers[first] == '\t')) {
                    ++first;
                }
                while (last > first && (headers[last - 1] == ' ' || headers[last - 1] == '\t')) {
                    --last;
                }
                return headers.substr(first, last - first);
            }
        }
        start = end;
    }
    return {};
}

WebSocket::WebSocket(NetworkInterface* network, int connect_id) : network_(network), connect_id_(connect_id) {
    handshake_event_group_ = xEventGroupCreate();
}

WebSocket::~WebSocket() {
    if (tcp_) {
        tcp_->Disconnect();
        tcp_.reset();
    }
    if (handshake_event_group_) {
        vEventGroupDelete(handshake_event_group_);
    }
}

void WebSocket::SetHeader(const char* key, const char* value) {
    headers_[key] = value;
}

void WebSocket::SetReceiveBufferSize(size_t size) {
    receive_buffer_size_ = std::max<size_t>(1, std::min<size_t>(size, 65535));
}

void WebSocket::SetRequestDeadline(int64_t deadline_us) {
    request_deadline_us_.store(deadline_us);
}

void WebSocket::SetCaCertificate(const std::string& pem) {
    ca_pem_ = pem;
}

bool WebSocket::IsConnected() const {
    return connected_.load();
}

bool WebSocket::Connect(const char* uri) {
    if (!network_ || !uri || !handshake_event_group_) {
        return false;
    }
    tcp_.reset();
    connected_.store(false);
    handshake_completed_ = false;
    continuation_ = false;
    fragmented_ = false;
    current_message_.clear();
    receive_buffer_.clear();
    const std::string address(uri);
    const size_t separator = address.find("://");
    if (separator == std::string::npos) {
        return false;
    }
    const std::string scheme = address.substr(0, separator);
    if (scheme != "ws" && scheme != "wss" && scheme != "http" && scheme != "https") {
        return false;
    }
    const bool secure = scheme == "wss" || scheme == "https";
    const size_t authority_start = separator + 3;
    const size_t path_start = address.find_first_of("/?", authority_start);
    const std::string authority = address.substr(authority_start, path_start - authority_start);
    const size_t colon = authority.find(':');
    const std::string host = authority.substr(0, colon);
    if (host.empty() || host.find('@') != std::string::npos || authority.find_first_of("\r\n []") != std::string::npos) {
        return false;
    }
    unsigned port = secure ? 443 : 80;
    if (colon != std::string::npos) {
        port = 0;
        if (colon + 1 == authority.size()) {
            return false;
        }
        for (size_t index = colon + 1; index < authority.size(); ++index) {
            const char c = authority[index];
            if (c < '0' || c > '9' || port > 6553) {
                return false;
            }
            port = port * 10 + c - '0';
        }
        if (!port || port > 65535) {
            return false;
        }
    }
    std::string path = path_start == std::string::npos ? "/" : address.substr(path_start);
    if (path.front() == '?') {
        path.insert(0, "/");
    }
    if (path.find_first_of("\r\n ") != std::string::npos) {
        return false;
    }
    unsigned char random_key[16];
    esp_fill_random(random_key, sizeof(random_key));
    const auto key = WebSocketBase64(random_key, sizeof(random_key));
    const auto accept_source = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[20];
    size_t digest_length = 0;
    if (psa_hash_compute(PSA_ALG_SHA_1, reinterpret_cast<const unsigned char*>(accept_source.data()),
                         accept_source.size(), digest, sizeof(digest), &digest_length) != PSA_SUCCESS || digest_length != sizeof(digest)) {
        return false;
    }
    expected_accept_ = WebSocketBase64(digest, sizeof(digest));
    tcp_ = secure ? network_->CreateSsl(connect_id_) : network_->CreateTcp(connect_id_);
    if (!tcp_ || (!ca_pem_.empty() && !tcp_->SetCaCertificate(ca_pem_))) {
        return false;
    }
    tcp_->SetRequestDeadline(request_deadline_us_.load());
    // 必须在连接和发送握手之前登记，避免快速响应丢失或被旧事件覆盖。
    xEventGroupClearBits(handshake_event_group_, HANDSHAKE_SUCCESS_BIT | HANDSHAKE_FAILED_BIT);
    tcp_->OnStream([this](const std::string& data) { OnTcpData(data); });
    tcp_->OnDisconnected([this]() {
        xEventGroupSetBits(handshake_event_group_, HANDSHAKE_FAILED_BIT);
        if (connected_.exchange(false) && on_disconnected_) {
            on_disconnected_();
        }
    });
    if (!tcp_->Connect(host, port)) {
        return false;
    }
    std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + authority +
        "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + key + "\r\n";
    for (const auto& header : headers_) {
        std::string name = header.first;
        for (char& c : name) {
            c = std::tolower(static_cast<unsigned char>(c));
        }
        if (name == "host" || name == "connection" || name == "upgrade" || name == "sec-websocket-version" || name == "sec-websocket-key") {
            continue;
        }
        if (header.first.find_first_of("\r\n:") != std::string::npos || header.second.find_first_of("\r\n") != std::string::npos) {
            return false;
        }
        request += header.first + ": " + header.second + "\r\n";
    }
    request += "\r\n";
    if (tcp_->Send(request) != static_cast<int>(request.size())) {
        return false;
    }
    const int64_t remaining = request_deadline_us_.load() ?
        std::max<int64_t>(0, (request_deadline_us_.load() - esp_timer_get_time()) / 1000) : 10000;
    const auto bits = xEventGroupWaitBits(handshake_event_group_, HANDSHAKE_SUCCESS_BIT | HANDSHAKE_FAILED_BIT,
                                          pdFALSE, pdFALSE, pdMS_TO_TICKS(remaining));
    if (!(bits & HANDSHAKE_SUCCESS_BIT) || (bits & HANDSHAKE_FAILED_BIT)) {
        ESP_LOGW(TAG, "operation=connect stage=handshake result=failed");
        return false;
    }
    if (on_connected_) {
        on_connected_();
    }
    return connected_.load();
}

bool WebSocket::Send(const std::string& data) {
    return Send(data.data(), data.size(), false);
}

bool WebSocket::Send(const void* data, size_t length, bool binary, bool fin) {
    if (!connected_.load() || !tcp_ || length > 65535 || (!data && length)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(send_mutex_);
    const int64_t deadline = request_deadline_us_.load();
    tcp_->SetRequestDeadline(deadline ? deadline : esp_timer_get_time() + 10000000);
    std::string frame;
    frame.reserve(length + 8);
    const uint8_t opcode = continuation_ ? 0 : binary ? 2 : 1;
    frame += static_cast<char>((fin ? 0x80 : 0) | opcode);
    if (length < 126) {
        frame += static_cast<char>(0x80 | length);
    } else {
        frame += static_cast<char>(0x80 | 126);
        frame += static_cast<char>(length >> 8);
        frame += static_cast<char>(length & 255);
    }
    uint8_t mask[4];
    esp_fill_random(mask, sizeof(mask));
    frame.append(reinterpret_cast<const char*>(mask), sizeof(mask));
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t index = 0; index < length; ++index) {
        frame += static_cast<char>(bytes[index] ^ mask[index % 4]);
    }
    if (tcp_->Send(frame) != static_cast<int>(frame.size())) {
        return false;
    }
    continuation_ = !fin;
    return true;
}

void WebSocket::Ping() {
    SendControlFrame(9, nullptr, 0);
}

void WebSocket::Close() {
    if (connected_.load()) {
        SendControlFrame(8, nullptr, 0);
    }
}

void WebSocket::OnConnected(std::function<void()> callback) { on_connected_ = std::move(callback); }
void WebSocket::OnDisconnected(std::function<void()> callback) { on_disconnected_ = std::move(callback); }
void WebSocket::OnData(std::function<void(const char*, size_t, bool)> callback) { on_data_ = std::move(callback); }
void WebSocket::OnError(std::function<void(int)> callback) { on_error_ = std::move(callback); }

int WebSocket::GetLastError() {
    return tcp_ ? tcp_->GetLastError() : 0;
}

void WebSocket::FailHandshake(const char* reason) {
    ESP_LOGW(TAG, "operation=handshake result=%s", reason);
    xEventGroupSetBits(handshake_event_group_, HANDSHAKE_FAILED_BIT);
}

void WebSocket::FailFrame(int code) {
    connected_.store(false);
    receive_buffer_.clear();
    current_message_.clear();
    ESP_LOGW(TAG, "operation=receive result=invalid_frame code=%d", code);
    if (on_error_) {
        on_error_(code);
    }
}

void WebSocket::OnTcpData(const std::string& data) {
    const size_t limit = receive_buffer_size_ * 2 + 4096;
    if (data.size() > limit || receive_buffer_.size() > limit - data.size()) {
        if (!handshake_completed_) {
            FailHandshake("header_limit");
        } else {
            FailFrame(EMSGSIZE);
        }
        return;
    }
    receive_buffer_.append(data);
    if (!handshake_completed_) {
        const auto end = receive_buffer_.find("\r\n\r\n");
        if (end == std::string::npos) {
            return;
        }
        const auto headers = receive_buffer_.substr(0, end + 4);
        auto upgrade = WebSocketHeader(headers, "upgrade");
        auto connection = WebSocketHeader(headers, "connection");
        for (char& c : upgrade) {
            c = std::tolower(static_cast<unsigned char>(c));
        }
        for (char& c : connection) {
            c = std::tolower(static_cast<unsigned char>(c));
        }
        if ((headers.rfind("HTTP/1.1 101 ", 0) != 0 && headers.rfind("HTTP/1.1 101\r\n", 0) != 0) ||
            WebSocketHeader(headers, "sec-websocket-accept") != expected_accept_ || upgrade != "websocket" ||
            connection.find("upgrade") == std::string::npos) {
            FailHandshake("invalid_response");
            return;
        }
        receive_buffer_.erase(0, end + 4);
        handshake_completed_ = true;
        connected_.store(true);
        xEventGroupSetBits(handshake_event_group_, HANDSHAKE_SUCCESS_BIT);
    }
    while (connected_.load() && receive_buffer_.size() >= 2) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(receive_buffer_.data());
        const uint8_t opcode = bytes[0] & 15;
        const bool fin = (bytes[0] & 128) != 0;
        uint64_t length = bytes[1] & 127;
        size_t header_length = 2;
        if ((bytes[0] & 0x70) || (bytes[1] & 128)) {
            FailFrame(EPROTO);
            return;
        }
        if (length == 126) {
            if (receive_buffer_.size() < 4) {
                return;
            }
            length = (uint32_t(bytes[2]) << 8) | bytes[3];
            header_length = 4;
        } else if (length == 127) {
            if (receive_buffer_.size() < 10) {
                return;
            }
            length = 0;
            for (size_t index = 2; index < 10; ++index) {
                length = (length << 8) | bytes[index];
            }
            header_length = 10;
        }
        if (length > receive_buffer_size_ || ((opcode & 8) && (!fin || length > 125))) {
            FailFrame(EMSGSIZE);
            return;
        }
        if (length > receive_buffer_.size() - header_length) {
            return;
        }
        const char* payload = receive_buffer_.data() + header_length;
        if (opcode <= 2) {
            if ((opcode == 0 && !fragmented_) || (opcode != 0 && fragmented_) ||
                current_message_.size() > receive_buffer_size_ - length) {
                FailFrame(EPROTO);
                return;
            }
            if (opcode != 0) {
                message_binary_ = opcode == 2;
                current_message_.clear();
            }
            current_message_.append(payload, length);
            fragmented_ = !fin;
            if (fin) {
                if (on_data_) {
                    on_data_(current_message_.data(), current_message_.size(), message_binary_);
                }
                current_message_.clear();
            }
        } else if (opcode == 8) {
            connected_.store(false);
            if (on_disconnected_) {
                on_disconnected_();
            }
        } else if (opcode == 9) {
            if (!SendControlFrame(10, payload, length)) {
                FailFrame(EIO);
                return;
            }
        } else if (opcode != 10) {
            FailFrame(EPROTO);
            return;
        }
        receive_buffer_.erase(0, header_length + length);
    }
}

bool WebSocket::SendControlFrame(uint8_t opcode, const void* data, size_t length) {
    if (!tcp_ || length > 125 || (!data && length)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(send_mutex_);
    tcp_->SetRequestDeadline(esp_timer_get_time() + 10000000);
    std::string frame;
    frame.reserve(length + 6);
    frame += static_cast<char>(128 | opcode);
    frame += static_cast<char>(128 | length);
    uint8_t mask[4];
    esp_fill_random(mask, sizeof(mask));
    frame.append(reinterpret_cast<const char*>(mask), sizeof(mask));
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t index = 0; index < length; ++index) {
        frame += static_cast<char>(bytes[index] ^ mask[index % 4]);
    }
    return tcp_->Send(frame) == static_cast<int>(frame.size());
}
