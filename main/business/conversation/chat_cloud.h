#pragma once

#include <atomic>
#include <new>
#include <esp_log.h>
#include "sdkconfig.h"
#include "infra/network/cloud_configuration.h"
#include "infra/network/endpoints/api_endpoints.h"
#include "infra/storage/settings.h"
#include "config.h"

enum class ChatCloudProvider { Baiwen = 0, Xiaozhi = 1, Yunxin = 2 };
inline constexpr int kChatCloudProviderCount = 3;
inline constexpr ChatCloudProvider kChatCloudDefaultProvider =
    static_cast<ChatCloudProvider>(BOARD_CHAT_PROVIDER_DEFAULT);
static_assert(BOARD_CHAT_PROVIDER_DEFAULT >= 0 && BOARD_CHAT_PROVIDER_DEFAULT < kChatCloudProviderCount);

inline std::atomic<ChatCloudProvider> ChatCloudDetailsaved{kChatCloudDefaultProvider};
inline ChatCloudProvider ChatCloudDetailactive = kChatCloudDefaultProvider;
inline std::atomic<bool> chat_cloud_detail_local_tts{true};

/** @brief 在内部 RAM 启动任务中加载云服务配置，须先于界面和网络任务执行 */
inline void ChatCloudInitializeFromNvs() {
    Settings settings("chat_cloud");
    const int stored = settings.GetInt("provider", static_cast<int>(kChatCloudDefaultProvider));
    const auto provider = stored >= 0 && stored < kChatCloudProviderCount ? static_cast<ChatCloudProvider>(stored)
                                                                          : kChatCloudDefaultProvider;
    ChatCloudDetailactive = provider;
    ChatCloudDetailsaved.store(provider, std::memory_order_release);
    chat_cloud_detail_local_tts.store(
        settings.GetBool(provider == ChatCloudProvider::Yunxin ? "yx_tts" : "xz_tts", true), std::memory_order_release);
}

/** @brief 获取已成功保存的云服务选择 */
inline ChatCloudProvider ChatCloudSavedProvider() {
    return ChatCloudDetailsaved.load(std::memory_order_acquire);
}

/** @brief 获取本次启动正在使用的云服务 */
inline ChatCloudProvider ChatCloudActiveProvider() {
    return ChatCloudDetailactive;
}

/**
 * @brief 在内部 RAM 任务中保存云服务选择，成功提交后发布内存快照
 * @return 成功返回 true；失败保留原选择，本次启动的服务直到重启前保持不变。
 */
inline bool ChatCloudSaveProvider(ChatCloudProvider provider) try {
    if (static_cast<int>(provider) < 0 || static_cast<int>(provider) >= kChatCloudProviderCount) {
        return false;
    }
    Settings settings("chat_cloud", true);
    if (!settings.SetInt("provider", static_cast<int>(provider)) || !settings.Commit()) {
        return false;
    }
    ChatCloudDetailsaved.store(provider, std::memory_order_release);
    return true;
} catch (const std::bad_alloc&) {
    ESP_LOGE("ChatCloud", "operation=save_provider result=no_memory");
    return false;
}

/** @brief 查询当前服务是否为小智 */
inline bool ChatCloudIsXiaozhi() {
    return ChatCloudActiveProvider() == ChatCloudProvider::Xiaozhi;
}

/** @brief 查询当前服务是否为网易云信 */
inline bool ChatCloudIsYunxin() {
    return ChatCloudActiveProvider() == ChatCloudProvider::Yunxin;
}

/** @brief 查询当前服务是否使用小智兼容协议 */
inline bool ChatCloudUsesXiaozhiProtocol() {
    return ChatCloudIsXiaozhi() || ChatCloudIsYunxin();
}

/** @brief 读取当前服务已成功保存的本地播报开关 */
inline bool ChatCloudLocalTtsEnabled() {
    return chat_cloud_detail_local_tts.load(std::memory_order_acquire);
}

/** @brief 查询当前服务是否允许语音播报 */
inline bool ChatCloudSpeechPlaybackEnabled() {
    return !ChatCloudUsesXiaozhiProtocol() || ChatCloudLocalTtsEnabled();
}

/**
 * @brief 在内部 RAM 任务中保存本地播报开关，成功提交后才更新内存状态
 * @return 成功返回 true；失败保留原开关，百问服务的播报设置由远端管理。
 */
inline bool ChatCloudSaveLocalTtsEnabled(bool enabled) try {
    Settings settings("chat_cloud", true);
    if (!settings.SetBool(ChatCloudIsYunxin() ? "yx_tts" : "xz_tts", enabled) || !settings.Commit()) {
        return false;
    }
    chat_cloud_detail_local_tts.store(enabled, std::memory_order_release);
    return true;
} catch (const std::bad_alloc&) {
    ESP_LOGE("ChatCloud", "operation=save_local_tts result=no_memory");
    return false;
}

/** @brief 查询当前服务是否具备建立会话所需的配置 */
inline bool ChatCloudAvailable() {
    return ChatCloudUsesXiaozhiProtocol() || ApiHasHost();
}

/** @brief 获取当前服务的 WebSocket 配置域 */
inline const char* ChatCloudWebsocketNamespace() {
    return ChatCloudIsYunxin() ? "yx_websocket" : ChatCloudIsXiaozhi() ? "xz_websocket" : "websocket";
}

/** @brief 获取当前服务的 MQTT 配置域 */
inline const char* ChatCloudMqttNamespace() {
    return ChatCloudIsYunxin() ? "yx_mqtt" : ChatCloudIsXiaozhi() ? "xz_mqtt" : "mqtt";
}

/** @brief 根据当前设备标识生成云信实时服务入口，仅用于网络请求 */
inline std::string ChatCloudYunxinWebsocketUrl(const std::string& mac) {
    return std::string(yunxin_websocket_url) + "?device_id=" + mac + "&app_key=" + yunxin_app_key;
}

/** @brief 获取云信请求所需凭证，调用方不得记录其内容 */
inline const char* ChatCloudYunxinToken() {
    return yunxin_token;
}

/** @brief 获取当前兼容协议的配置更新入口 */
inline const char* ChatCloudChatOtaUrl() {
    return ChatCloudIsYunxin() ? yunxin_ota_url : xiaozhi_ota_url;
}

/** @brief 为基础设施提供本次会话的产品配置快照，不记录凭据。 */
inline CloudConfiguration ChatCloudConfiguration(const std::string& device_id) {
    CloudConfiguration config;
    config.available = ChatCloudAvailable();
    config.separate_chat_configuration = ChatCloudUsesXiaozhiProtocol();
    config.websocket_namespace = ChatCloudWebsocketNamespace();
    config.mqtt_namespace = ChatCloudMqttNamespace();
    config.chat_configuration_url = ChatCloudChatOtaUrl();
    if (ChatCloudIsYunxin()) {
        config.websocket_url = ChatCloudYunxinWebsocketUrl(device_id);
        config.websocket_token = ChatCloudYunxinToken();
        config.websocket_version = 1;
    }
    return config;
}
