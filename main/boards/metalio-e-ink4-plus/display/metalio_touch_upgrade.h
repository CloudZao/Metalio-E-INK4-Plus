#pragma once
#include <stdbool.h>

/**
 * @brief 在 EPD/I2C 初始化后、触摸采样及后台任务启动前检查并升级触摸固件
 * @return 未启用升级、版本无需更新或升级成功时返回 true，失败时返回 false
 */
bool MetalioTouch_UpgradeOnBoot();
