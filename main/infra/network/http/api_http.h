#pragma once

#include <memory>
#include <string>
#include "http.h"
#include "infra/platform/system_info.h"

// 统一 HTTP 请求头：业务 API 请求先走 ApplyCommonHeaders / ApplyJsonHeaders。
// 自动带上 Device-Id（设备 MAC）。

inline const std::string& ApiDeviceId() {
    static const std::string kId = SystemInfo::GetMacAddress();
    return kId;
}

// 所有业务请求的公共头（对应 curl --header 'Device-Id: ...'）。
inline void ApiApplyCommonHeaders(Http* http) {
    if (http == nullptr) {
        return;
    }
    http->SetHeader("Device-Id", ApiDeviceId());
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Connection", "close");
}

inline void ApiApplyCommonHeaders(const std::unique_ptr<Http>& http) {
    ApiApplyCommonHeaders(http.get());
}

// JSON 请求额外 Content-Type。
inline void ApiApplyJsonHeaders(Http* http) {
    ApiApplyCommonHeaders(http);
    if (http != nullptr) {
        http->SetHeader("Content-Type", "application/json");
    }
}

inline void ApiApplyJsonHeaders(const std::unique_ptr<Http>& http) {
    ApiApplyJsonHeaders(http.get());
}
