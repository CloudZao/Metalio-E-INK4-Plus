#ifndef MQTT_PROTOCOL_H
#define MQTT_PROTOCOL_H

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <mbedtls/build_info.h>
#include <cJSON.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <mqtt.h>
#include <psa/crypto.h>
#include <udp.h>
#include "protocol.h"
#include "infra/network/cloud_configuration.h"

#define MQTT_RECONNECT_INTERVAL_MS 60000

#define MQTT_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class MqttProtocol : public Protocol {
public:
    /** @brief 创建 MQTT 会话，调度与重连许可由系统提供 */
    MqttProtocol(std::function<bool(std::function<void()>)> schedule, std::function<bool()> can_reconnect,
                 CloudConfiguration configuration);

    /** @brief 关闭重连入口，等待网络及定时器回调退出后释放会话 */
    ~MqttProtocol();

    /** @brief 建立 MQTT 控制连接 */
    bool Start() override;

    /** @brief 加密并发送一个 Opus 音频包 */
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;

    /** @brief 完成 hello 协商并建立 UDP 音频通道 */
    bool OpenAudioChannel() override;

    /** @brief 释放 UDP 通道并通知服务端结束当前会话 */
    void CloseAudioChannel() override;

    /** @brief 查询音频通道是否存在且未报错或超时 */
    bool IsAudioChannelOpened() const override;

private:
    CloudConfiguration configuration_;                     // 当前会话的产品配置
    std::function<bool(std::function<void()>)> schedule_;  // 系统任务投递
    std::function<bool()> can_reconnect_;                  // 系统允许重连时返回 true
    EventGroupHandle_t event_group_handle_ = nullptr;

    std::string publish_topic_;

    std::mutex channel_mutex_;
    std::unique_ptr<Mqtt> mqtt_;
    std::unique_ptr<Udp> udp_;
    psa_key_id_t aes_key_ = PSA_KEY_ID_NULL;
    std::string aes_nonce_;
    std::string udp_server_;
    int udp_port_;
    uint32_t local_sequence_;
    uint32_t remote_sequence_;
    uint32_t rejected_audio_packets_ = 0;  // 异常帧日志限频计数
    uint32_t rejected_json_packets_ = 0;   // 异常消息日志限频计数
    esp_timer_handle_t reconnect_timer_ = nullptr;
    std::mutex reconnect_mutex_;  // 串行化关闭标记与重连定时器启动
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

    bool StartMqttClient(bool report_error = false);
    void ScheduleReconnect();
    void ParseServerHello(const cJSON* root);
    std::string DecodeHexString(const std::string& hex_string);
    bool CryptCtr(const uint8_t* nonce, const uint8_t* input, size_t length, uint8_t* output);

    bool SendText(const std::string& text) override;
};

#endif  // MQTT_PROTOCOL_H
