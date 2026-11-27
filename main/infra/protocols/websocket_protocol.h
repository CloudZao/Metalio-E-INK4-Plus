#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <web_socket.h>
#include "protocol.h"
#include "infra/network/cloud_configuration.h"

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class WebsocketProtocol : public Protocol {
public:
    /** @brief 使用产品配置快照创建WebSocket协议。 */
    explicit WebsocketProtocol(CloudConfiguration configuration);

    /** @brief 关闭连接并释放会话同步资源 */
    ~WebsocketProtocol();

    /** @brief 检查会话资源，连接在打开音频通道时建立 */
    bool Start() override;

    /** @brief 按协商版本封装并发送一个 Opus 音频包 */
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;

    /** @brief 建立 WebSocket 连接并完成 hello 协商 */
    bool OpenAudioChannel() override;

    /** @brief 关闭当前 WebSocket 音频连接 */
    void CloseAudioChannel() override;

    /** @brief 查询连接是否有效且未报错或超时 */
    bool IsAudioChannelOpened() const override;

private:
    CloudConfiguration configuration_;  // 当前会话的产品配置
    EventGroupHandle_t event_group_handle_;
    std::unique_ptr<WebSocket> websocket_;
    int version_ = 1;
    uint32_t rejected_json_messages_ = 0;  // JSON异常日志限频计数
    uint32_t rejected_audio_packets_ = 0;  // 异常帧限频计数

    void ParseServerHello(const cJSON* root);
    bool SendText(const std::string& text) override;
};

#endif
