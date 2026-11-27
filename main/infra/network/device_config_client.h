#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <esp_err.h>
#include <http.h>
#include "infra/network/cloud_configuration.h"

class DeviceConfigClient {
public:
    /** @brief 创建云配置客户端；取消回调在联网、重试及响应提交前检查 */
    DeviceConfigClient(std::function<bool()> cancelled, CloudConfiguration configuration = {});

    /** @brief 查询产品固件元数据，不执行版本比较或镜像写入 */
    esp_err_t QueryFirmware();

    /** @brief 查询并保存当前对话提供方配置 */
    esp_err_t QueryChat();

    /** @brief 应答当前激活挑战；未完成绑定返回超时状态 */
    esp_err_t Activate();

    /** @brief 查询当前响应是否要求挑战应答 */
    bool HasActivationChallenge() const {
        return has_activation_challenge_;
    }

    /** @brief 查询当前响应是否包含 MQTT 配置 */
    bool HasMqttConfig() const {
        return has_mqtt_config_;
    }

    /** @brief 查询当前响应是否包含 WebSocket 配置 */
    bool HasWebsocketConfig() const {
        return has_websocket_config_;
    }

    /** @brief 查询当前响应是否包含待绑定验证码 */
    bool HasActivationCode() const {
        return has_activation_code_;
    }

    /** @brief 查询本次请求是否成功应用服务器时间 */
    bool HasServerTime() const {
        return has_server_time_;
    }

    /** @brief 获取固件版本元数据，不判断是否较新 */
    const std::string& GetFirmwareVersion() const {
        return firmware_version_;
    }

    /** @brief 获取镜像地址，调用方不得记录地址内容 */
    const std::string& GetFirmwareUrl() const {
        return firmware_url_;
    }

    /** @brief 获取仅供界面显示的待绑定验证码 */
    const std::string& GetActivationCode() const {
        return activation_code_;
    }

private:
    esp_err_t CheckResponse(bool chat_only);
    esp_err_t CheckResponseOnce(bool chat_only, int64_t deadline_us);
    std::unique_ptr<Http> SetupHttp();
    std::string GetActivationPayload();
    std::function<bool()> cancelled_;        // 联网工作是否已被业务取消或系统暂停
    CloudConfiguration configuration_;       // 对话提供方配置快照
    bool chat_only_ = false;                 // 当前激活挑战的配置来源
    bool has_mqtt_config_ = false;           // 响应含 MQTT 配置
    bool has_websocket_config_ = false;      // 响应含 WebSocket 配置
    bool has_server_time_ = false;           // 本次响应已成功设置 UTC 时间
    bool has_activation_code_ = false;       // 本次响应含待绑定验证码
    bool has_serial_number_ = false;         // eFuse 含设备序列号
    bool has_activation_challenge_ = false;  // 本次响应含激活挑战
    std::string activation_code_;            // 仅供显示的验证码
    std::string firmware_version_;           // 产品固件版本元数据
    std::string firmware_url_;               // 镜像地址，不得输出日志
    std::string activation_challenge_;       // 激活挑战，不得输出日志
    std::string serial_number_;              // eFuse 序列号，不得输出日志
};
