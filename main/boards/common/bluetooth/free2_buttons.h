#pragma once

#include <cstddef>
#include <cstdint>

namespace Free2Buttons {
constexpr uint8_t kInputReport = 1; // HID Report Reference 的输入报告类型
constexpr uint8_t kClickReportId = 1; // 单字节方向指令，每条通知表示一次短按
constexpr uint8_t kInvalid = 0xff; // 不支持的报告

/** @brief 判断已验证的 Free 2 按键输入报告，兼容不同设备的报告编号 */
inline bool IsButtonReport(uint8_t id, uint8_t type) {
    return type == kInputReport && (id == kClickReportId || id == 3 || id == 4);
}

/** @brief 解码方向指令或三字节状态；0 为松开，1/2 为按键，其他报告返回 kInvalid */
inline uint8_t Decode(uint8_t report_id, const uint8_t* data, size_t size) {
    if (!data) return kInvalid;
    if (report_id == kClickReportId)
        return size == 1 && (data[0] == 1 || data[0] == 2) ? data[0] : kInvalid;
    if (!IsButtonReport(report_id, kInputReport) || size != 3 || data[1] || data[2] || data[0] > 2) return kInvalid;
    return data[0];
}

} // namespace Free2Buttons
