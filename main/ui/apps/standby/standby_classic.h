#pragma once

#include <cstddef>
#include "lvgl.h"

// 经典待机 UI（日期 / 天气 / 待办）。仅由 StandbyScreen 入口在判定无待机壁纸时调用。

/** @brief 在 root 上构建经典待机控件；as_overlay=true 时隐藏回首页按钮、待办不可点。 */
void StandbyClassic_Build(lv_obj_t* root, bool as_overlay);

/** @brief 启动日期填充 + 天气/清单刷新（须在 Build 之后）。 */
void StandbyClassic_StartRuntime();

/** @brief 停运行时刷新；可重复调用。 */
void StandbyClassic_StopRuntime();

/** @brief 清 UI 指针与 todos（Overlay 删除或页卸载后）。 */
void StandbyClassic_Teardown();
