#pragma once

#include <stdbool.h>
#include "epd_highlevel.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化并行墨水屏及其电源外设，逻辑方向由显示层映射
 * @return true TPS 就绪
 */
bool EpdPanel_Init(void);

/**
 * @brief 借用高层绘制状态，须在初始化成功后使用
 */
EpdiyHighlevelState* EpdPanel_GetHighlevelState(void);

/**
 * @brief 查询最近一次初始化的屏幕电源就绪结果
 */
bool EpdPanel_IsReady(void);

#ifdef __cplusplus
}
#endif
