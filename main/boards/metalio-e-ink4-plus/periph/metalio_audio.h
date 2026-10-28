#pragma once

#include <stdint.h>
#include <functional>

class AudioCodec;

/**
 * @brief 返回板级 AudioCodec（转发 Board::GetAudioCodec）
 */
AudioCodec* MetalioAudio_GetCodec();

/**
 * @brief 初始化 UART，后台先复位 BH，再配置并确认模式1；不会等待复位完成
 * @param on_boot_reset_done 启动复位结束时调用一次；初始化失败时同样调用以释放等待方
 */
void MetalioAudio_Init(void (*on_boot_reset_done)());
/**
 * @brief 请求后台初始化；同一电源周期重试耗尽后不重复启动
 */
void MetalioAudio_RequestModeInit();
constexpr int kMetalioAudioInitWaitMs = 20000; // 后台调用方等待初始化的最长时间

/**
 * @brief 电源关闭时使已确认的音频模式失效
 */
void MetalioAudio_OnPowerOff();
/**
 * @brief 记录恢复供电时间，供后台初始化等待电源稳定
 */
void MetalioAudio_OnPowerRestored();

/**
 * @brief 等待 AT 模式1 就绪
 * @param timeout_ms 最长等待毫秒；等待操作仅供后台任务使用
 * @param still_needed 可选继续等待条件，失效时立即结束等待
 * @return true 已就绪且继续等待条件仍成立
 */
bool MetalioAudio_WaitModeReady(int timeout_ms = 2000, std::function<bool()> still_needed = {});

/**
 * @brief 模式已就绪时启动 codec，否则请求后台初始化并立即返回
 * @return true 已启动；false 尚未就绪或启动失败
 */
bool MetalioAudio_EnsureStarted();

/**
 * @brief 当前电源周期已收到明确的 SET MODE 1 成功应答
 */
bool MetalioAudio_ModeReady();

enum class MetalioAudioState {
    NotReady, // 未开始初始化或硬件尚未供电
    Initializing, // 正在复位或等待模式应答
    Ready, // 已确认当前电源周期的模式
    Failed // 当前电源周期初始化已失败
};

/** @brief 查询当前电源周期的音频初始化状态，不触发初始化或复位。 */
MetalioAudioState MetalioAudio_GetState();

/**
 * @brief 设置板级功放使能状态
 */
void MetalioAudio_SetPa(bool on);

/**
 * @brief 输入采样率 Hz
 */
int MetalioAudio_InputRate();

/**
 * @brief 读取音频输入 PCM，声道布局由板级 codec 决定
 * @return 实际采样点数；超时/失败 ≤0
 */
int MetalioAudio_ReadMic(int16_t* dest, int samples);

/**
 * @brief 写喇叭 PCM（单声道 int16）；内部开 PA
 * @return 写入采样点数；失败 ≤0
 */
int MetalioAudio_WriteSpk(const int16_t* data, int samples);
