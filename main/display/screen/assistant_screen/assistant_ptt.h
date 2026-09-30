#pragma once

#include "assistant_screen_priv.h"

/** @brief 翻页；dir=-1 上一页，+1 下一页 */
bool Assistant_TurnPage(int dir);
/** @brief 复位屏触 PTT 臂听状态 */
void Assistant_ResetTouchPttState();
/** @brief 创建全屏触控 PTT 命中层 */
lv_obj_t* Assistant_CreateTouchPttHitLayer(lv_obj_t* scr);
/** @brief 停止状态栏 PTT 波形动画 */
void Assistant_StopPttWaveAnim();
/** @brief BOOT 或屏触任一源仍按住 */
bool Assistant_IsAnyPttHeld();
/** @brief 请求同步状态栏 PTT/波形 */
void Assistant_RequestSyncPttOverlay();
/** @brief 若已无 PTT 按住则停止 Listening */
void Assistant_StopListeningIfNoPttHeld(const char* why);
/** @brief 显示或隐藏状态栏波形 */
void Assistant_ApplyPttWave(bool show);
/** @brief 创建状态栏波形宿主控件 */
lv_obj_t* Assistant_CreatePttWaveHost(lv_obj_t* parent, lv_coord_t bar_h);
/** @brief 取消屏触 PTT 长按臂听定时器 */
void Assistant_CancelTouchPttArmTimer();
/** @brief 是否应显示 PTT 波形（Listening 且仍按住） */
bool Assistant_IsPttWaveWanted();
