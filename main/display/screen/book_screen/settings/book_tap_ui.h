#pragma once

#include <lvgl.h>

#include "book_screen/settings/book_tap_zones.h"

/** @brief 触摸分区动作文案 */
const char* TapZoneActionLabel(BookTapZoneAction action);
/** @brief 与触摸分区浮层互斥 */
void TapZonesPanelClose();
/** @brief 重建触摸分区面板内容 */
void TapZonesRebuildContent();
/** @brief 确保触摸分区面板已创建 */
void TapZonesEnsurePanel();
/** @brief 设置卡「触摸分区」 */
void OnSheetTapZonesClicked(lv_event_t* e);
/** @brief 分区格点击 */
void OnTapZoneCellClicked(lv_event_t* e);
/** @brief 选中分区动作 */
void OnTapZoneActionPicked(lv_event_t* e);
/** @brief 复位分区默认 */
void OnTapZoneReset(lv_event_t* e);
/** @brief 关闭分区面板 */
void OnTapZoneClose(lv_event_t* e);
/** @brief 分区动作选择返回 */
void OnTapZonePickBack(lv_event_t* e);
/** @brief 盖板 vk_prev：选动作页退回九宫格（须 LVGL 任务，勿在 touch_feed 同步 clean）。 */
void AsyncTapZonePickBack(void* user_data);
/** @brief 盖板 vk_prev：关闭触摸分区浮层。 */
void AsyncTapZonesPanelClose(void* user_data);
