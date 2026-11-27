#pragma once

#include "lvgl.h"

// 首页上半区域的时钟与天气 Hero，独立于待机 Overlay。

/** @brief 在 parent 上挂载经典 Hero；可重复调用，内部会先 teardown 再重建。 */
lv_obj_t* HomeHero_Mount(lv_obj_t* parent, lv_coord_t y_offset);

/** @brief 挂载斜切风格 Hero；可重复调用，内部会先 teardown 再重建。 */
lv_obj_t* HomeHero_MountSlash(lv_obj_t* parent, lv_coord_t y_offset);

/** @brief 启动时钟/天气更新和 listener，需在 Mount 后调用。 */
void HomeHero_Start();

/** @brief 停止 timer 和 listener，可重复调用。 */
void HomeHero_Stop();

/** @brief 释放 UI 资源，适用于首页删除或重建前。 */
void HomeHero_Teardown();
