#pragma once

#include <lvgl.h>

#include "cloud_screen/push/push_resources_library.h"

/** @brief 云列表行长按 */
void Cloud_OnRowLongPressed(lv_event_t* e);
/** @brief 云列表行点击 */
void Cloud_OnRowClicked(lv_event_t* e);
/** @brief 云多选取消 */
void OnMultiCancel(lv_event_t* e);
/** @brief 云多选全选 */
void OnMultiSelectAll(lv_event_t* e);
/** @brief 云多选保存/下载 */
void OnMultiSave(lv_event_t* e);
/** @brief 云多选删除 */
void OnMultiDelete(lv_event_t* e);
/** @brief 按索引找列表行控件 */
lv_obj_t* FindListRow(int index);
/** @brief 按尺寸找行内子控件 */
lv_obj_t* FindRowChildBySize(lv_obj_t* row, lv_coord_t w, lv_coord_t h);
/** @brief 刷新单行勾选 */
void PatchRowCheckMark(int index);
/** @brief 刷新可见行勾选 */
void PatchVisibleRowCheckMarks();
/** @brief 刷新单行缩略图 */
void PatchRowThumb(int index);
/** @brief 创建资源列表行 */
lv_obj_t* CreateResourceRow(lv_obj_t* parent, const reader::CloudPushResource& item, int index);
/** @brief 渲染列表页（内部） */
void RenderListPageInternal(bool schedule_thumbs);
/** @brief 渲染云列表当前页 */
void RenderListPage();
/** @brief 请求云列表重绘 */
void RequestCloudRender();
/** @brief 请求勾选标记重绘 */
void RequestCheckMarksPaint();
/** @brief 调度封面缩略填充 */
void ScheduleCoverThumbFill();
