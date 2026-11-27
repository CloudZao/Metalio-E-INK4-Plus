#pragma once

#include <string>

struct CloudConfiguration {
    bool available = false;                    // 当前产品是否允许建立云会话
    bool separate_chat_configuration = false;  // 对话配置使用独立入口并清理过时凭据
    std::string websocket_namespace;           // WebSocket配置存储域
    std::string mqtt_namespace;                // MQTT配置存储域
    std::string chat_configuration_url;        // 对话配置入口，不得输出日志
    std::string websocket_url;                 // 可选固定会话地址，不得输出日志
    std::string websocket_token;               // 可选认证信息，不得输出日志
    int websocket_version = 0;                 // 零表示读取存储的协议版本
};

/** @brief 在查询参数之前追加激活路径，空地址返回空字符串。 */
inline std::string CloudActivationUrl(const std::string& url) {
    if (url.empty())
        return {};
    const auto query_pos = url.find('?');
    std::string path = url.substr(0, query_pos);
    if (path.empty())
        return {};
    if (path.back() != '/') {
        path += '/';
    }
    return path + "activate" + (query_pos == std::string::npos ? "" : url.substr(query_pos));
}
