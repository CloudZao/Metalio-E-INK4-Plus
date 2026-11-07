#pragma once

#include <cstddef>
#include <cstdint>

namespace Smart1EButtons {
constexpr uint8_t kClickReportId = 1; // 每条单字节通知表示一次短按
constexpr uint8_t kStandbyKey = 3; // 固定待机键，不参与翻页学习
constexpr uint8_t kInvalid = 0xff; // 未映射的报告

/** @brief 判断 Smart 1-E 的按键输入报告 */
inline bool IsButtonReport(uint8_t id, uint8_t type) {
    return id == kClickReportId && type == 1;
}

/** @brief 解码 Smart 1-E 的方向和待机指令，返回规范化按键编号 */
inline uint8_t Decode(const uint8_t* data, size_t size) {
    if (!data || size != 1) return kInvalid;
    switch (data[0]) {
        case 1: return 1;
        case 2: return 2;
        case 6: return kStandbyKey;
        default: return kInvalid;
    }
}
} // namespace Smart1EButtons
