#pragma once

/** @brief 恢复 RTC 到系统 UTC，并处理旧版当地时间格式兼容 */
bool TimeServiceRestoreRtc();

/** @brief 将系统 UTC 写入 RTC，并保存时间格式标记 */
bool TimeServiceStoreRtc();
