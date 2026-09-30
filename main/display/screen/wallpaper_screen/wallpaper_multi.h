#pragma once

#include <lvgl.h>

/** @brief 同步壁纸多选集合容量 */
void SyncWpSelectedSize();
/** @brief 壁纸已选数量 */
int WpSelectedCount();
/** @brief 壁纸项是否已选 */
bool WpItemSelected(int idx);
/** @brief 切换壁纸项选中 */
void ToggleWpItemSelected(int idx);
/** @brief 刷新壁纸底栏模式 */
void RefreshWpFooterMode();
/** @brief 退出壁纸多选 */
void ExitWpMultiMode(bool rebuild);
/** @brief 进入壁纸多选 */
void EnterWpMultiModeSelect(int idx);
/** @brief 刷新可见勾选 */
void PatchWpVisibleCheckMarks();
/** @brief 请求刷勾选 */
void RequestWpCheckMarksPaint();
/** @brief 壁纸多选取消 */
void OnWpMultiCancel(lv_event_t* e);
/** @brief 壁纸多选全选 */
void OnWpMultiSelectAll(lv_event_t* e);
/** @brief 壁纸多选删除 */
void OnWpMultiRemove(lv_event_t* e);
/** @brief 壁纸行长按 */
void Wallpaper_OnRowLongPressed(lv_event_t* e);
/** @brief 壁纸行点击 */
void Wallpaper_OnRowClicked(lv_event_t* e);
/** @brief 构建底栏 */
void BuildFooter(lv_obj_t* scr);
