#pragma once

#include "lvgl.h"

/** @brief 启动蜂窝网络自动测试的等待定时器，替换已有启动及重试等待。 */
void SettingsTestCell_StartAutoSequence();

/** @brief 停止蜂窝网络自动测试的启动等待定时器。 */
void SettingsTestCell_StopStartTimer();

/** @brief 停止蜂窝网络测试的重试定时器。 */
void SettingsTestCell_StopRetryTimer();
