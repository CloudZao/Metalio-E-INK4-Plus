#pragma once

#include <cstddef>
#include <cstdint>

namespace Free3RButtons {
constexpr uint8_t kStandbyKey = 3; // 固定待机键，不参与翻页按键学习
constexpr uint8_t kInvalid = 0xff; // 不支持或暂未映射的报告

/** @brief 判断已采集的 Free3-R 按键输入报告 */
inline bool IsButtonReport(uint8_t id, uint8_t type) {
    return id == 3 && type == 1;
}

/** @brief 解码 Free3-R 短按状态；1/2 为上一页和下一页，3 为待机，0 为松开 */
inline uint8_t Decode(const uint8_t* data, size_t size) {
    if (!data || size != 3 || data[1] || data[2]) return kInvalid;
    switch (data[0]) {
        case 0: return 0;
        case 2: return 1;
        case 1: return 2;
        case 8: return kStandbyKey;
        default: return kInvalid;
    }
}
} // namespace Free3RButtons
