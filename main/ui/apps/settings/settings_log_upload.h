#pragma once

/** @brief 查询日志上传页面是否处于活动状态。 */
bool SettingsLogUpload_IsActive();

/** @brief 在 LVGL 线程发起日志上传并显示进度页面。 */
void SettingsLogUpload_Start();

/** @brief 在 LVGL 线程取消日志上传并关闭进度页面。 */
void SettingsLogUpload_Teardown();
