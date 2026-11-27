#pragma once

/** @brief 查询当前阅读文字抗锯齿偏好，供提交刷新时选择波形。 */
bool EpdReaderAaEnabled();

/** @brief 更新阅读文字抗锯齿偏好，并同步需要该开关的显示后端。 */
void EpdSetReaderAaEnabled(bool enabled);
