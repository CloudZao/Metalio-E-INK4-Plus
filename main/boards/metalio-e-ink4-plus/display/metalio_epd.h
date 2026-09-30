#pragma once

#include "epd_highlevel.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化并行墨水屏（EPDiy + TPS/TCA），竖屏 inverted-portrait
 * @return true TPS 就绪
 */
bool MetalioEpd_Init(void);

/**
 * @brief highlevel 状态（framebuffer / update）
 */
EpdiyHighlevelState* MetalioEpd_Hl(void);

/**
 * @brief TPS 是否就绪
 */
bool MetalioEpd_Ready(void);

#ifdef __cplusplus
}
#endif
