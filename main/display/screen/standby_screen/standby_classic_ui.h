#pragma once

#include "standby_classic_priv.h"

/** @brief 将天气快照应用到经典待机 UI */
void StandbyClassic_ApplyWeatherUi();
/** @brief 启动 Overlay 运行时（日期/天气刷新） */
void StandbyClassic_StartOverlayRuntime();
/** @brief 构建经典待机控件树 */
void StandbyClassic_BuildUi(lv_obj_t* root);
/** @brief 刷新一次日期显示 */
void StandbyClassic_RefreshDateOnce();
