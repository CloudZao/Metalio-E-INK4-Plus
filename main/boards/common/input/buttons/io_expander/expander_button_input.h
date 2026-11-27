#pragma once

#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 BOOT GPIO 与 TCA 音量/电源键采样
 * @note 所有按键空闲为高、按下为低；启动及恢复时按住的普通键屏蔽至松开
 */
void ExpanderButtonInput_Init(void);

/**
 * @brief 睡前：先停扫描，再把 TCA 口改输出以屏蔽 INT；按键脚驱到空闲高，P17 随后改输入唤醒
 * @return 成功返回 ESP_OK；失败时保持停止扫描，调用方须恢复后退出休眠流程
 */
esp_err_t ExpanderButtonInput_MaskForSleep(void);

/**
 * @brief 浅睡醒后：恢复 IO 口方向与按键扫描状态
 * @return 配置及采样成功返回 ESP_OK；失败时保持扫描不可用
 */
esp_err_t ExpanderButtonInput_ResumeAfterSleep(void);

/**
 * @brief 按键当前是否按下
 * @param index 0=BOOT 1=音量+ 2=音量- 3=电源
 */
bool ExpanderButtonInput_IsDown(int index);

/** @brief 查询是否存在待领取的电源短按事件，不清除状态。 */
bool ExpanderButtonInput_HasPowerClick(void);

#ifdef __cplusplus
}
#endif
