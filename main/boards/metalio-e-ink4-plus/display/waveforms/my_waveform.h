#pragma once
#include "epd_internals.h"
#ifdef __cplusplus
extern "C" {
#endif

extern const EpdWaveform MY_WAVEFORM; // 清晰档完整刷新波形

/** @brief 在高层驱动初始化前准备刷新波形 */
void my_waveform_init(void);

/** @brief 获取均衡及快速档的灰阶波形，不可用时返回 NULL */
const EpdWaveform* my_waveform_fast(void);

/** @brief 获取阅读抗锯齿使用的八级灰 GL 波形 */
const EpdWaveform* my_waveform_gray8(void);

/** @brief 获取短 DU 局刷波形 */
const EpdWaveform* my_waveform_short_du(void);

/** @brief 获取快速档普通局刷使用的十四相 DU 波形 */
const EpdWaveform* my_waveform_fast_du(void);

/** @brief 获取黑白切页的条带擦白与重绘波形 */
const EpdWaveform* my_waveform_wipe_reveal(void);

/** @brief 获取黑白切页使用的短条带擦白与重绘波形 */
const EpdWaveform* my_waveform_wipe_reveal_short(void);

/**
 * @brief 获取指定刷新档位的 DU 擦白与 GL 灰阶重绘波形
 * @note 空间条带延迟由渲染器添加，不占用灰阶索引
 */
const EpdWaveform* my_waveform_wipe_gl(int profile);

#ifdef __cplusplus
}
#endif
