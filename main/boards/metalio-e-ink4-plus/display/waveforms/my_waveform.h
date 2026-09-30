#pragma once

#include "epd_internals.h"

/** 默认表：GC16/GL16 完整 48 相（可选 MY_WAVEFORM_TRIM=1 裁成 36/37）+ 完备 DU */
extern const EpdWaveform MY_WAVEFORM;

/**
 * @brief 开机装配 MY_WAVEFORM（DU 完备；GC/GL 按 MY_WAVEFORM_TRIM 裁或拷全表）
 * @note 须在 epd_hl_init(&MY_WAVEFORM) 之前调用一次
 */
void my_waveform_init(void);
