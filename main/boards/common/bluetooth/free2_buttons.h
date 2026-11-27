#pragma once

#include <cstddef>
#include <cstdint>

constexpr uint8_t kFree2ButtonsInputReport = 1;    // HID Report Reference 的输入报告类型
constexpr uint8_t kFree2ButtonsClickReportId = 1;  // 单字节方向指令，每条通知表示一次短按
constexpr uint8_t kFree2ButtonsInvalid = 0xff;     // 不支持的报告

/** @brief 判断已验证的 Free 2 按键输入报告，兼容不同设备的报告编号 */
inline bool Free2ButtonsIsButtonReport(uint8_t id, uint8_t type) {
    return type == kFree2ButtonsInputReport && (id == kFree2ButtonsClickReportId || id == 3 || id == 4);
}

/** @brief 解码方向指令或三字节状态；0 为松开，1/2 为按键，其他报告返回 kInvalid */
inline uint8_t Free2ButtonsDecode(uint8_t report_id, const uint8_t* data, size_t size) {
    if (!data) {
        return kFree2ButtonsInvalid;
    }
    if (report_id == kFree2ButtonsClickReportId) {
        return size == 1 && (data[0] == 1 || data[0] == 2) ? data[0] : kFree2ButtonsInvalid;
    }
    if (!Free2ButtonsIsButtonReport(report_id, kFree2ButtonsInputReport) || size != 3 || data[1] || data[2] ||
        data[0] > 2) {
        return kFree2ButtonsInvalid;
    }
    return data[0];
}
