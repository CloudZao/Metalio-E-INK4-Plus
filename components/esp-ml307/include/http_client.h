#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include "http.h"
#include "tcp.h"

#define EC801E_HTTP_EVENT_HEADERS_RECEIVED (1 << 0)
#define EC801E_HTTP_EVENT_BODY_RECEIVED (1 << 1)
#define EC801E_HTTP_EVENT_ERROR (1 << 2)
#define EC801E_HTTP_EVENT_COMPLETE (1 << 3)

class NetworkInterface;

class HttpClient : public Http {
public:
    HttpClient(NetworkInterface* network, int connect_id = 0);
    ~HttpClient();

    void SetTimeout(int timeout_ms) override;

    /** @brief 设置本次请求的启动后绝对截止时间，单位微秒，零表示不限制。 */
    void SetRequestDeadline(int64_t deadline_us) override {
        request_deadline_us_ = deadline_us;
    }
    /** @brief 为本客户端后续 HTTPS 连接复制 PEM 信任根。 */
    bool SetCaCertificate(const std::string& pem) override {
        ca_pem_ = pem;
        return true;
    }

    void SetHeader(const std::string& key, const std::string& value) override;

    /** @brief 设置本客户端响应正文的接收队列容量。 */
    bool SetReceiveBufferSize(size_t bytes) override {
        if (!bytes) {
            return false;
        }
        max_body_chunks_size_ = bytes;
        return true;
    }
    void SetContent(std::string&& content) override;
    bool Open(const std::string& method, const std::string& url) override;
    void Close() override;
    void Cancel() override;
    int Read(char* buffer, size_t buffer_size) override;
    int Write(const char* buffer, size_t buffer_size) override;
    
    // Keep-Alive support
    void SetKeepAlive(bool enable);
    bool IsConnectionReusable(const std::string& host, int port) const;

    int GetStatusCode() override;
    std::string GetResponseHeader(const std::string& key) const override;
    size_t GetBodyLength() override;
    std::string ReadAll() override;
    int GetLastError() override;

private:
    // 数据块结构，用于队列缓冲
    struct DataChunk {
        std::string data;
        size_t offset = 0;  // 当前读取偏移
        
        // 使用移动构造函数避免拷贝
        DataChunk(std::string&& d) : data(std::move(d)), offset(0) {}
        DataChunk(const std::string& d) : data(d), offset(0) {}
        
        size_t available() const {
            return data.size() - offset;
        }
        
        size_t read(char* buffer, size_t size) {
            size_t bytes_to_read = std::min(size, available());
            if (bytes_to_read > 0) {
                memcpy(buffer, data.data() + offset, bytes_to_read);
                offset += bytes_to_read;
            }
            return bytes_to_read;
        }
        
        bool empty() const {
            return offset >= data.size();
        }
    };

    // 头部条目结构体，用于高效存储和查找
    struct HeaderEntry {
        std::string original_key;  // 保留原始大小写的key（用于输出HTTP头部）
        std::string value;         // 头部值
        
        HeaderEntry() = default;
        HeaderEntry(const std::string& key, const std::string& val) 
            : original_key(key), value(val) {}
    };

    std::atomic<bool> cancelled_{false};
    NetworkInterface* network_;
    int connect_id_;
    EventGroupHandle_t event_group_handle_;
    std::mutex mutex_;
    std::condition_variable cv_;
    
    // 用于读取操作的专门锁和缓冲区队列
    std::mutex read_mutex_;
    std::deque<DataChunk> body_chunks_;
    std::condition_variable write_cv_;
    // 下载大文件时过小会反压 TCP；与 DownloadHttpToFile 的 32KB 读块匹配
    size_t max_body_chunks_size_ = 48 * 1024; // 默认业务队列容量，应用请求可在连接前指定
    
    size_t header_bytes_ = 0; // 本次响应头累计字节数，含状态行与换行
    int status_code_ = -1;
    int timeout_ms_ = 30000;
    std::string ca_pem_; // 单客户端信任根，不修改系统证书配置
    int64_t request_deadline_us_ = 0;
    int RemainingTimeoutMs() const;
    std::string rx_buffer_;
    std::map<std::string, HeaderEntry> headers_;  // key为小写，用于快速查找
    std::string url_;
    std::string method_;
    std::string protocol_;
    std::string host_;
    std::string path_;
    int port_ = 80;
    std::optional<std::string> content_ = std::nullopt;
    std::map<std::string, HeaderEntry> response_headers_;  // key为小写，用于快速查找
    
    // 移除原来的 body_ 变量，现在使用 body_chunks_ 队列
    size_t body_offset_ = 0;
    size_t content_length_ = 0;
    size_t total_body_received_ = 0;  // 总共接收的响应体字节数
    std::atomic<bool> eof_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> headers_received_{false};
    bool request_chunked_ = false;
    bool response_chunked_ = false;
    std::atomic<bool> connection_error_{false};  // 新增：标记连接是否异常断开
    bool keep_alive_ = false;  // 新增：是否启用 Keep-Alive（默认不启用）
    bool server_keep_alive_ = false;  // 新增：服务器是否支持 Keep-Alive
    std::atomic<int> last_error_{0};  // 存储最后一次错误码
    
    // HTTP 协议解析状态
    enum class ParseState {
        STATUS_LINE,
        HEADERS,
        BODY,
        CHUNK_SIZE,
        CHUNK_DATA,
        CHUNK_CRLF,
        CHUNK_TRAILER,
        COMPLETE
    };
    ParseState parse_state_ = ParseState::STATUS_LINE;
    size_t chunk_size_ = 0;
    size_t chunk_received_ = 0;

    // 接收任务会回调 this；必须最后析构，保证上面的锁和解析状态仍有效。
    std::unique_ptr<Tcp> tcp_;

    // 私有方法
    bool ParseUrl(const std::string& url);
    std::string BuildHttpRequest();
    void OnTcpData(const std::string& data);
    void OnTcpDisconnected();
    void ProcessReceivedData();
    bool ParseStatusLine(const std::string& line);
    bool ParseHeaderLine(const std::string& line);
    void ParseRegularBody(const std::string& data);
    size_t ParseChunkSize(const std::string& chunk_size_line);
    std::string GetNextLine(std::string& buffer);
    bool HasCompleteLine(const std::string& buffer);
    void SetError();
    
    // 新增：向读取队列添加数据的方法
    void AddBodyData(std::string data);
    
    // 新增：检查数据是否完整接收
    bool IsDataComplete() const;
    
    // 新增：重置请求状态（用于连接复用）
    void ResetRequestState();
};

#endif
