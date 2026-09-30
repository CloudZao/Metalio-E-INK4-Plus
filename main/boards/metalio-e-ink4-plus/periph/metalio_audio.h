#pragma once

#include <stdint.h>

class AudioCodec;

/**
 * @brief BH1098 + NS4150 板级音频：UART 模式1、I2S duplex、PA_EN
 */

/**
 * @brief 返回板级 AudioCodec（转发 Board::GetAudioCodec）
 */
AudioCodec* MetalioAudio_GetCodec();

/**
 * @brief 初始化 BT UART 并下发模式1；PA 默认关（I2S 由 GetAudioCodec 惰性创建）
 */
void MetalioAudio_Init();

/**
 * @brief 等待 AT 模式1 就绪
 * @param timeout_ms 最长等待毫秒
 * @return true 已就绪
 */
bool MetalioAudio_WaitModeReady(int timeout_ms = 2000);

/**
 * @brief 等待模式1就绪并启动 codec
 * @return true 可读写麦/喇叭
 */
bool MetalioAudio_EnsureStarted();

/**
 * @brief UART/codec 已初始化且模式1任务已完成（不保证芯片有回包）
 */
bool MetalioAudio_ModeReady();

/**
 * @brief 初始化后是否收到过 BT UART 回包
 */
bool MetalioAudio_BtRxSeen();

/**
 * @brief 功放使能（IO_P4=PA_EN）
 */
void MetalioAudio_SetPa(bool on);

/**
 * @brief 输入采样率 Hz
 */
int MetalioAudio_InputRate();

/**
 * @brief 读麦克风 PCM（单声道 int16）
 * @return 实际采样点数；超时/失败 ≤0
 */
int MetalioAudio_ReadMic(int16_t* dest, int samples);

/**
 * @brief 写喇叭 PCM（单声道 int16）；内部开 PA
 * @return 写入采样点数；失败 ≤0
 */
int MetalioAudio_WriteSpk(const int16_t* data, int samples);
