#pragma once
#include <cstdint>
#include <lvgl.h>

/** @brief 创建固定方形 SD 壁纸预览区，图片与提示均居中 */
lv_obj_t* SettingsTestAgingSd_Build(lv_obj_t* parent, lv_coord_t side);
/** @brief 重新读取目录中第一张壁纸，结果回收后调用本轮完成钩子 */
void SettingsTestAgingSd_ReadCycle(void (*done)(uint32_t), uint32_t session);
/** @brief 停止接收新读取并取消在途任务，已释放 SD 句柄时返回 true */
bool SettingsTestAgingSd_StopReading();
/** @brief 作废在途读取并释放预览资源，须在界面线程调用 */
void SettingsTestAgingSd_Reset();
