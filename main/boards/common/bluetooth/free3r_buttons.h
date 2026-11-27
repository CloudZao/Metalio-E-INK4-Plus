#pragma once

#include <cstddef>
#include <cstdint>

constexpr uint8_t kFree3RButtonsStandbyKey = 3;  // 固定待机键，不参与翻页按键学习
constexpr uint8_t kFree3RButtonsInvalid = 0xff;  // 不支持或暂未映射的报告

/** @brief 判断已采集的 Free3-R 按键输入报告 */
inline bool Free3RButtonsIsButtonReport(uint8_t id, uint8_t type) {
    return id == 3 && type == 1;
}

/** @brief 解码 Free3-R 短按状态；1/2 为上一页和下一页，3 为待机，0 为松开 */
inline uint8_t Free3RButtonsDecode(const uint8_t* data, size_t size) {
    if (!data || size != 3 || data[1] || data[2]) {
        return kFree3RButtonsInvalid;
    }
    switch (data[0]) {
        case 0:
            return 0;
        case 2:
            return 1;
        case 1:
            return 2;
        case 8:
            return kFree3RButtonsStandbyKey;
        default:
            return kFree3RButtonsInvalid;
    }
}
