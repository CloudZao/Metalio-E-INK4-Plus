#pragma once

#include "boards/common/bluetooth/ble_page_turner.h"

/** @brief 将 BLE 按键事件投递到有界队列，不访问 UI */
void PagerKey_Receive(BlePageTurnerAction action, BlePageTurnerKeyPhase phase);

/** @brief 在 UI 任务复用盖板键短按及电源键待机处理 */
void PagerKey_Drain();
