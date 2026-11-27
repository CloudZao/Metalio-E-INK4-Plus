#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>
#include <driver/uart.h>
#include <esp_err.h>

struct BluetoothAudioConfig {
    uart_port_t uart_port;                                        // 板级 UART 端口
    bool reset_on_boot;                                           // 启动时是否执行独立音频电源复位
    esp_err_t (*set_pa)(bool);                                    // 直接执行功放硬件控制，禁止回调本模块
    void (*on_uart_data)(const std::vector<uint8_t>&) = nullptr;  // 共享串口的附加接收处理
};

class AudioCodec;

/**
 * @brief 返回板级 AudioCodec（转发 Board::GetAudioCodec）
 */
AudioCodec* BluetoothAudio_GetCodec();

/**
 * @brief 初始化 UART，按板级配置执行启动复位并在后台确认模式
 * @param config UART、复位策略及板级硬件操作
 * @param on_boot_reset_done 启动复位结束、无需复位时 UART 就绪或初始化失败时调用一次，释放板级等待方
 */
void BluetoothAudio_Init(const BluetoothAudioConfig& config, void (*on_boot_reset_done)());

/**
 * @brief 请求后台初始化；同一电源周期重试耗尽后不重复启动
 */
void BluetoothAudio_RequestModeInit();
constexpr int kBluetoothAudioInitWaitMs = 20000;  // 后台调用方等待初始化的最长时间

/**
 * @brief 电源关闭时使已确认的音频模式失效
 */
void BluetoothAudio_OnPowerOff();

/**
 * @brief 记录恢复供电时间，供后台初始化等待电源稳定
 */
void BluetoothAudio_OnPowerRestored();

/**
 * @brief 等待 AT 模式1 就绪
 * @param timeout_ms 最长等待毫秒；等待操作仅供后台任务使用
 * @param still_needed 可选继续等待条件，失效时立即结束等待
 * @return true 已就绪且继续等待条件仍成立
 */
bool BluetoothAudio_WaitModeReady(int timeout_ms = 2000, std::function<bool()> still_needed = {});

/**
 * @brief 模式已就绪时启动 codec，否则请求后台初始化并立即返回
 * @return true 已启动；false 尚未就绪或启动失败
 */
bool BluetoothAudio_EnsureStarted();

/**
 * @brief 当前电源周期已收到明确的 SET MODE 1 成功应答
 */
bool BluetoothAudio_ModeReady();

enum class BluetoothAudioState {
    NotReady,      // 未开始初始化或硬件尚未供电
    Initializing,  // 正在复位或等待模式应答
    Ready,         // 已确认当前电源周期的模式
    Failed         // 当前电源周期初始化已失败
};

/** @brief 查询当前电源周期的音频初始化状态，不触发初始化或复位。 */
BluetoothAudioState BluetoothAudio_GetState();

/** @brief 非阻塞下发蓝牙版本查询；串口事务忙时返回 ESP_ERR_TIMEOUT。 */
esp_err_t BluetoothAudio_QueryVersion();

/** @brief 复制最近收到的有效蓝牙版本；新查询会清除旧值，尚未收到或缓冲区不足时返回 false。 */
bool BluetoothAudio_GetVersion(char* version, size_t capacity);

/**
 * @brief 设置板级功放使能状态
 * @return 成功返回 ESP_OK，失败返回板级驱动错误
 */
esp_err_t BluetoothAudio_SetPa(bool on);

/**
 * @brief 输入采样率 Hz
 */
int BluetoothAudio_InputRate();

/**
 * @brief 读取音频输入 PCM，声道布局由板级 codec 决定
 * @return 实际采样点数；超时/失败 ≤0
 */
int BluetoothAudio_ReadMic(int16_t* dest, int samples);

/**
 * @brief 写喇叭 PCM（单声道 int16）；内部开 PA
 * @return 写入采样点数；失败 ≤0
 */
int BluetoothAudio_WriteSpk(const int16_t* data, int samples);
