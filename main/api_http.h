#pragma once

#include "http.h"
#include "system_info.h"

#include <memory>
#include <string>

// 统一 HTTP 请求头：业务 API 请求先走 ApplyCommonHeaders / ApplyJsonHeaders。
// 自动带上 Device-Id（设备 MAC）。
namespace api {

inline const std::string& DeviceId() {
    static const std::string kId = SystemInfo::GetMacAddress();
    return kId;
}

// 所有业务请求的公共头（对应 curl --header 'Device-Id: ...'）。
inline void ApplyCommonHeaders(Http* http) {
    if (http == nullptr) {
        return;
    }
    http->SetHeader("Device-Id", DeviceId());
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Connection", "close");
}

inline void ApplyCommonHeaders(const std::unique_ptr<Http>& http) {
    ApplyCommonHeaders(http.get());
}

// JSON 请求额外 Content-Type。
inline void ApplyJsonHeaders(Http* http) {
    ApplyCommonHeaders(http);
    if (http != nullptr) {
        http->SetHeader("Content-Type", "application/json");
    }
}

inline void ApplyJsonHeaders(const std::unique_ptr<Http>& http) {
    ApplyJsonHeaders(http.get());
}

}  // namespace api
