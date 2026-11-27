#pragma once

#include <cstddef>
#include <cstdint>

constexpr uint8_t kSmart1EButtonsClickReportId = 1;  // 每条单字节通知表示一次短按
constexpr uint8_t kSmart1EButtonsStandbyKey = 3;     // 固定待机键，不参与翻页学习
constexpr uint8_t kSmart1EButtonsInvalid = 0xff;     // 未映射的报告

/** @brief 判断 Smart 1-E 的按键输入报告 */
inline bool Smart1EButtonsIsButtonReport(uint8_t id, uint8_t type) {
    return id == kSmart1EButtonsClickReportId && type == 1;
}

/** @brief 解码 Smart 1-E 的方向和待机指令，返回规范化按键编号 */
inline uint8_t Smart1EButtonsDecode(const uint8_t* data, size_t size) {
    if (!data || size != 1) {
        return kSmart1EButtonsInvalid;
    }
    switch (data[0]) {
        case 1:
            return 1;
        case 2:
            return 2;
        case 6:
            return kSmart1EButtonsStandbyKey;
        default:
            return kSmart1EButtonsInvalid;
    }
}
