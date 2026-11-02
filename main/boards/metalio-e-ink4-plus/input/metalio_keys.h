#pragma once

#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 BOOT GPIO 与 TCA 音量/电源键采样
 * @note TCA 键空闲上拉为高、按下为低；BOOT 空闲极性上电采样
 */
void MetalioKeys_Init(void);

/**
 * @brief 睡前：先停扫描，再把 TCA 口改输出以屏蔽 INT；按键脚驱到空闲高，P17 随后改输入唤醒
 * @return 成功返回 ESP_OK；失败时保持停止扫描，调用方须恢复后退出休眠流程
 */
esp_err_t MetalioKeys_MaskForSleep(void);

/**
 * @brief 浅睡醒后：恢复 IO 口方向与按键扫描状态
 * @return 配置及采样成功返回 ESP_OK；失败时保持扫描不可用
 */
esp_err_t MetalioKeys_ResumeAfterSleep(void);

/**
 * @brief 按键扫描是否可用（睡中 mask 期间为 false）
 */
bool MetalioKeys_IsReady(void);

/**
 * @brief 取走一次按键边沿（按下或松开）
 * @return true 有边沿
 */
bool MetalioKeys_TakeEdge(void);

/**
 * @brief 按键当前是否按下
 * @param index 0=BOOT 1=音量+ 2=音量- 3=电源
 */
bool MetalioKeys_IsDown(int index);

/**
 * @brief 取走电源长按触发的关机脉冲请求（一次性）
 * @return true 需执行 IO_P13 脉冲序列
 */
bool MetalioKeys_TakeShutdownPulse(void);

/** @brief 采样任务锁存的电源短按；全刷阻塞时保留，单一消费者取走。 */
bool MetalioKeys_TakePowerClick(void);
bool MetalioKeys_HasPowerClick(void);

#ifdef __cplusplus
}
#endif
