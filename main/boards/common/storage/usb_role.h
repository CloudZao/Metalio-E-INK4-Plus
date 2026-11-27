#pragma once

#include <atomic>

enum class UsbRole { None, CameraHost, DiskDevice };

/** @brief 获取同一USB控制器的角色所有者，仅供USB后端协调 */
inline std::atomic<UsbRole>& UsbRoleOwner() {
    static std::atomic<UsbRole> owner{UsbRole::None};
    return owner;
}

/** @brief 非阻塞申请USB角色，已占用时拒绝 */
inline bool UsbRoleAcquire(UsbRole role) {
    auto expected = UsbRole::None;
    return UsbRoleOwner().compare_exchange_strong(expected, role, std::memory_order_acq_rel);
}

/** @brief 实际控制器释放后归还角色，不清除其他所有者 */
inline void UsbRoleRelease(UsbRole role) {
    UsbRoleOwner().compare_exchange_strong(role, UsbRole::None, std::memory_order_acq_rel);
}
