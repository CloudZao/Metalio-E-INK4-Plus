#ifndef _OTA_H
#define _OTA_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <esp_err.h>
#include "board.h"

/** SD 卡根目录本地固件；开机检测到则优先于网络 OTA */
inline constexpr const char kOtaSdFirmwarePath[] = "/sdcard/Metalio-E-Ink4-Plus.bin";

class Ota {
public:
    Ota();
    ~Ota();

    esp_err_t CheckVersion();
    esp_err_t Activate();
    bool HasActivationChallenge() { return has_activation_challenge_; }
    bool HasNewVersion() { return has_new_version_; }
    bool HasMqttConfig() { return has_mqtt_config_; }
    bool HasWebsocketConfig() { return has_websocket_config_; }
    bool HasActivationCode() { return has_activation_code_; }
    bool HasServerTime() { return has_server_time_; }
    bool StartUpgrade(std::function<void(int progress, size_t speed)> callback);
    bool StartUpgradeFromUrl(const std::string& url, std::function<void(int progress, size_t speed)> callback);
    /** @brief 从本地文件刷写 app 分区（路径通常为 kOtaSdFirmwarePath） */
    bool StartUpgradeFromFile(const std::string& path,
                              std::function<void(int progress, size_t speed)> callback);
    void MarkCurrentVersionValid();

    /**
     * @brief 从固件镜像头读取 app 版本号（仅用于确认框展示，不参与是否升级判断）
     * @return true 读取成功
     */
    static bool PeekFirmwareVersionFromFile(const std::string& path, std::string& out_version);

    const std::string& GetFirmwareVersion() const { return firmware_version_; }
    const std::string& GetCurrentVersion() const { return current_version_; }
    const std::string& GetFirmwareUrl() const { return firmware_url_; }
    const std::string& GetActivationMessage() const { return activation_message_; }
    const std::string& GetActivationCode() const { return activation_code_; }
    std::string GetCheckVersionUrl();

private:
    std::string activation_message_;
    std::string activation_code_;
    bool has_new_version_ = false;
    bool has_mqtt_config_ = false;
    bool has_websocket_config_ = false;
    bool has_server_time_ = false;
    bool has_activation_code_ = false;
    bool has_serial_number_ = false;
    bool has_activation_challenge_ = false;
    std::string current_version_;
    std::string firmware_version_;
    std::string firmware_url_;
    std::string activation_challenge_;
    std::string serial_number_;
    int activation_timeout_ms_ = 30000;

    bool Upgrade(const std::string& firmware_url);
    bool UpgradeFromFile(const std::string& path);
    std::function<void(int progress, size_t speed)> upgrade_callback_;
    std::vector<int> ParseVersion(const std::string& version);
    bool IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion);
    std::string GetActivationPayload();
    std::unique_ptr<Http> SetupHttp();
};

#endif // _OTA_H
