#ifndef TCP_H
#define TCP_H


#include <atomic>
#include <string>
#include <cstdint>
#include <functional>

class Tcp {
public:
    virtual ~Tcp() = default;
    virtual bool Connect(const std::string& host, int port) = 0;
    virtual void Disconnect() = 0;
    /** @brief 设置独立连接的 PEM 信任根；空字符串使用系统信任根。 */
    virtual bool SetCaCertificate(const std::string& pem) { return pem.empty(); }
    virtual int Send(const std::string& data) = 0;
    /** @brief 设置本次请求的启动后绝对截止时间，单位微秒，零表示不限制。 */
    virtual void SetRequestDeadline(int64_t deadline_us) {
        request_deadline_us_ = deadline_us;
    }

    virtual void OnStream(std::function<void(const std::string& data)> callback) {
        stream_callback_ = callback;
    }
    
    virtual void OnDisconnected(std::function<void()> callback) {
        disconnect_callback_ = callback;
    }
    
    // 连接状态查询
    bool connected() const { return connected_; }

    // 获取最后一次错误码
    virtual int GetLastError() = 0;

protected:
    std::atomic<int64_t> request_deadline_us_{0};
    std::function<void(const std::string& data)> stream_callback_;
    std::function<void()> disconnect_callback_;
    
    // 连接状态管理
    std::atomic<bool> connected_{false};         // 是否可以正常读写数据
};

#endif // TCP_H
