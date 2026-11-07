#pragma once

#include "settings.h"
#include "api_endpoints.h"
#include "sdkconfig.h"
#include <atomic>

namespace chat_cloud {
enum class Provider { Baiwen = 0, Xiaozhi = 1, Yunxin = 2 };
inline constexpr int kProviderCount = 3;
#if CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS_GLOBAL
inline constexpr Provider kDefaultProvider = Provider::Yunxin;
#else
inline constexpr Provider kDefaultProvider = Provider::Baiwen;
#endif

namespace detail {
inline std::atomic<Provider> saved{kDefaultProvider};
inline Provider active = kDefaultProvider;
inline std::atomic<bool> local_tts{true};
}  // namespace detail

// Call once on the internal-RAM startup task, before UI/network workers exist.
// NVS reads also disable Flash caches; never lazily load from a UI accessor.
inline void InitializeFromNvs() {
    Settings settings("chat_cloud");
    const int stored = settings.GetInt("provider", static_cast<int>(kDefaultProvider));
    const auto provider = stored >= 0 && stored < kProviderCount
        ? static_cast<Provider>(stored) : kDefaultProvider;
    detail::active = provider;
    detail::saved.store(provider, std::memory_order_release);
    detail::local_tts.store(settings.GetBool(provider == Provider::Yunxin ? "yx_tts" : "xz_tts", true), std::memory_order_release);
}

inline Provider SavedProvider() { return detail::saved.load(std::memory_order_acquire); }
inline Provider ActiveProvider() { return detail::active; }

// Application's internal-RAM task only. Publish the cached selection after
// Settings has committed it. The active provider stays fixed until reboot.
inline void SaveProvider(Provider provider) {
    if (static_cast<int>(provider) < 0 || static_cast<int>(provider) >= kProviderCount) return;
    {
        Settings settings("chat_cloud", true);
        settings.SetInt("provider", static_cast<int>(provider));
    }
    detail::saved.store(provider, std::memory_order_release);
}

inline bool IsXiaozhi() { return ActiveProvider() == Provider::Xiaozhi; }
inline bool IsYunxin() { return ActiveProvider() == Provider::Yunxin; }
inline bool UsesXiaozhiProtocol() { return IsXiaozhi() || IsYunxin(); }
inline bool LocalTtsEnabled() { return detail::local_tts.load(std::memory_order_acquire); }
inline bool SpeechPlaybackEnabled() { return !UsesXiaozhiProtocol() || LocalTtsEnabled(); }

// Internal-RAM task only, just like SaveProvider. Baiwen's preference stays remote.
inline void SaveLocalTtsEnabled(bool enabled) {
    {
        Settings settings("chat_cloud", true);
        settings.SetBool(IsYunxin() ? "yx_tts" : "xz_tts", enabled);
    }
    detail::local_tts.store(enabled, std::memory_order_release);
}

inline bool Available() { return UsesXiaozhiProtocol() || api::HasHost(); }
inline const char* WebsocketNamespace() { return IsYunxin() ? "yx_websocket" : IsXiaozhi() ? "xz_websocket" : "websocket"; }
inline const char* MqttNamespace() { return IsYunxin() ? "yx_mqtt" : IsXiaozhi() ? "xz_mqtt" : "mqtt"; }
inline std::string YunxinWebsocketUrl(const std::string& mac) {
    return std::string(yunxin_websocket_url) + "?device_id=" + mac +
           "&app_key=" + yunxin_app_key;
}
inline const char* YunxinToken() { return yunxin_token; }
inline const char* ChatOtaUrl() {
    return IsYunxin()
        ? yunxin_ota_url
        : xiaozhi_ota_url;
}

// Append the activation path before the query, preserving Yunxin's appkey.
inline std::string ActivationUrl(const std::string& ota_url) {
    if (ota_url.empty()) { return {}; }
    const auto query_pos = ota_url.find('?');
    std::string path = ota_url.substr(0, query_pos);
    const std::string query = query_pos == std::string::npos ? "" : ota_url.substr(query_pos);
    if (path.empty()) return {};
    if (path.back() != '/') { path += '/'; }
    return path + "activate" + query;
}
}  // namespace chat_cloud
