#pragma once

#include "lv_adapter_epdiy.h"

/** @brief 屏内手指是否仍按着（盖板键不算；对齐 397） */
bool TouchUiFingerIsDown();

/**
 * @brief 是否还有未交付的 down/up 边沿（含刷屏期间短按的 press→release 回放）
 */
bool TouchUiHasPendingEdges();

/** @brief 兼容 397 头名 */
using LVAdapterDisplay = LvAdapterEpdiy;
