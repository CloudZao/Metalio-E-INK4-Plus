#pragma once

#include <vector>

#include <lvgl.h>

#include "cloud_screen/push/push_resources_library.h"

/** @brief 关闭通用对话框 */
void CloseDialog();
/** @brief 关闭动作确认框 */
void CloseActionDialog();
/** @brief 显示动作确认框 */
void ShowActionDialog(int cloud_index);
/** @brief 调度删除选中云资源 */
void ScheduleDeleteItems(std::vector<reader::CloudPushResource> items);
/** @brief 云页删除按钮 */
void Cloud_OnDeleteClicked(lv_event_t* e);
