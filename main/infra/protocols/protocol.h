#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cJSON.h>
#include "infra/audio/audio_stream_packet.h"

struct BinaryProtocol2 {
    uint16_t version;       // 协议版本，使用网络字节序
    uint16_t type;          // 消息类型：0 为 Opus，1 为 JSON
    uint32_t reserved;      // 保留字段
    uint32_t timestamp;     // 音频帧时间戳，保留协议格式
    uint32_t payload_size;  // 载荷字节数，使用网络字节序
    uint8_t payload[];      // 载荷数据
} __attribute__((packed));

struct BinaryProtocol3 {
    uint8_t type;           // 消息类型：0 为 Opus
    uint8_t reserved;       // 保留字段
    uint16_t payload_size;  // 载荷字节数，使用网络字节序
    uint8_t payload[];      // 载荷数据
} __attribute__((packed));

enum ListeningMode {
    kListeningModeAutoStop,    // 由服务端判断输入结束
    kListeningModeManualStop,  // 由客户端显式结束输入
};

class Protocol {
public:
    /** @brief 通过协议基类释放具体传输会话 */
    virtual ~Protocol() = default;

    /** @brief 获取服务端协商的音频采样率 */
    inline int server_sample_rate() const {
        return server_sample_rate_;
    }

    /** @brief 获取服务端协商的音频帧时长，单位为毫秒 */
    inline int server_frame_duration() const {
        return server_frame_duration_;
    }

    /** @brief 获取当前会话标识的只读引用 */
    inline const std::string& session_id() const {
        return session_id_;
    }

    /** @brief 设置音频接收回调，音频包所有权交给接收方 */
    void OnIncomingAudio(std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback);

    /** @brief 设置 JSON 接收回调，节点仅在回调期间有效 */
    void OnIncomingJson(std::function<void(const cJSON* root)> callback);

    /** @brief 设置音频通道协商完成后的通知 */
    void OnAudioChannelOpened(std::function<void()> callback);

    /** @brief 设置音频通道关闭通知 */
    void OnAudioChannelClosed(std::function<void()> callback);

    /** @brief 设置网络错误通知 */
    void OnNetworkError(std::function<void(const std::string& message)> callback);

    /** @brief 设置控制连接建立后的通知 */
    void OnConnected(std::function<void()> callback);

    /** @brief 设置控制连接断开后的通知 */
    void OnDisconnected(std::function<void()> callback);

    /** @brief 初始化协议资源或建立按需使用的控制连接 */
    virtual bool Start() = 0;

    /** @brief 建立并协商音频通道，失败时返回 false */
    virtual bool OpenAudioChannel() = 0;

    /** @brief 关闭当前音频通道 */
    virtual void CloseAudioChannel() = 0;

    /** @brief 查询音频通道是否可用 */
    virtual bool IsAudioChannelOpened() const = 0;

    /** @brief 接管并发送音频包，发送失败时返回 false */
    virtual bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) = 0;

    /** @brief 通知服务端按指定模式开始接收语音 */
    virtual void SendStartListening(ListeningMode mode);

    /** @brief 通知服务端结束当前语音输入 */
    virtual void SendStopListening();

    /** @brief 提交单张照片问题，录音问题附在 stop 消息中 */
    bool SendImageQuestion(const std::string& image_id, bool recorded);

    /** @brief 请求中止当前语音播报 */
    virtual void SendAbortSpeaking();

    /** @brief 发送已序列化的 MCP 消息载荷 */
    virtual void SendMcpMessage(const std::string& message);

protected:
    std::function<void(const cJSON* root)> on_incoming_json_;
    std::function<void(std::unique_ptr<AudioStreamPacket> packet)> on_incoming_audio_;
    std::function<void()> on_audio_channel_opened_;
    std::function<void()> on_audio_channel_closed_;
    std::function<void(const std::string& message)> on_network_error_;
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;

    int server_sample_rate_ = 24000;
    int server_frame_duration_ = 60;
    bool error_occurred_ = false;
    std::string session_id_;
    std::chrono::time_point<std::chrono::steady_clock> last_incoming_time_;

    /** @brief 构造客户端 hello，失败时清空输出并返回 false */
    bool BuildHelloMessage(int version, const char* transport, std::string& message) const;

    /** @brief 校验服务端可选音频参数，失败时保留原参数 */
    static bool ParseServerAudioParameters(const cJSON* root, int& sample_rate, int& frame_duration);

    /** @brief 通过具体传输发送文本消息 */
    virtual bool SendText(const std::string& text) = 0;

    /** @brief 标记会话出错并通知网络错误接收方 */
    virtual void SetError(const std::string& message);

    /** @brief 查询是否超过音频通道接收超时时间 */
    virtual bool IsTimeout() const;
};

#endif  // PROTOCOL_H
