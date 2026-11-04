#pragma once

enum class ScreenWifiSwitchState {
    kIdle, // 当前没有开关任务
    kSwitching, // 开关任务执行中
    kFailed, // 上次开关操作失败
};

/** @brief 查询设置页与快捷面板共用的 Wi-Fi 开关任务状态 */
ScreenWifiSwitchState ScreenWifiGetSwitchState();
/** @brief 在 Wi-Fi 模式下异步切换并保存开关状态；重复点击在任务完成前忽略 */
void ScreenWifiToggle();
