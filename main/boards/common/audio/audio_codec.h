#ifndef _AUDIO_CODEC_H
#define _AUDIO_CODEC_H

#include <atomic>
#include <vector>
#include <string>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <driver/i2s_std.h>

#define AUDIO_CODEC_DMA_DESC_NUM 6
#define AUDIO_CODEC_DMA_FRAME_NUM 240

class AudioCodec {
public:
    /** @brief 创建音频编解码器的共享状态 */
    AudioCodec();

    /** @brief 通过基类释放具体音频编解码器 */
    virtual ~AudioCodec();

    /** @brief 发布输出音量并保存配置，派生类负责应用到硬件 */
    virtual void SetOutputVolume(int volume);

    /** @brief 发布输入增益，具体单位与应用方式由派生类约定 */
    virtual void SetInputGain(float gain);

    /** @brief 更新输入逻辑开关，派生类负责硬件切换 */
    virtual void EnableInput(bool enable);

    /** @brief 更新输出逻辑开关，派生类负责硬件切换 */
    virtual void EnableOutput(bool enable);

    /** @brief 输出 PCM 数据，完整写入时返回 true */
    virtual bool OutputData(std::vector<int16_t>& data);

    /** @brief 读取完整 PCM 采样，短读或读取失败时返回 false */
    virtual bool InputData(std::vector<int16_t>& data);

    /** @brief 读取音量配置并启动音频通道 */
    virtual void Start();

    /** @brief 查询硬件是否就绪，支持派生类按需管理 DMA 生命周期 */
    virtual bool HardwareReady() const {
        return true;
    }

    /** @brief 释放按需申请的硬件资源，常驻 codec 默认无需处理 */
    virtual void ReleaseHardware() {}

    /** @brief 查询是否支持同时输入和输出 */
    inline bool duplex() const {
        return duplex_;
    }

    /** @brief 查询输入是否包含播放参考通道 */
    inline bool input_reference() const {
        return input_reference_;
    }

    /** @brief 获取输入采样率，单位为 Hz */
    inline int input_sample_rate() const {
        return input_sample_rate_;
    }

    /** @brief 获取输出采样率，单位为 Hz */
    inline int output_sample_rate() const {
        return output_sample_rate_;
    }

    /** @brief 获取输入通道数 */
    inline int input_channels() const {
        return input_channels_;
    }

    /** @brief 获取输出通道数 */
    inline int output_channels() const {
        return output_channels_;
    }

    /** @brief 获取最近发布的输出音量 */
    inline int output_volume() const {
        return output_volume_.load();
    }

    /** @brief 获取最近发布的输入增益 */
    inline float input_gain() const {
        return input_gain_.load();
    }

    /** @brief 查询输入逻辑开关 */
    inline bool input_enabled() const {
        return input_enabled_.load();
    }

    /** @brief 查询输出逻辑开关 */
    inline bool output_enabled() const {
        return output_enabled_.load();
    }

protected:
    i2s_chan_handle_t tx_handle_ = nullptr;  // I2S 发送通道，由具体驱动管理生命周期
    i2s_chan_handle_t rx_handle_ = nullptr;  // I2S 接收通道，由具体驱动管理生命周期

    bool duplex_ = false;                      // 是否支持同时输入和输出，初始化后只读
    bool input_reference_ = false;             // 输入是否包含播放参考通道，初始化后只读
    std::atomic<bool> input_enabled_{false};   // 输入任务与音频电源定时器共享的逻辑开关
    std::atomic<bool> output_enabled_{false};  // 输出任务与音频电源定时器共享的逻辑开关
    int input_sample_rate_ = 0;                // 输入采样率，单位为 Hz，初始化后只读
    int output_sample_rate_ = 0;               // 输出采样率，单位为 Hz，初始化后只读
    int input_channels_ = 1;                   // 输入通道数，初始化后只读
    int output_channels_ = 1;                  // 输出通道数，初始化后只读
    std::atomic<int> output_volume_{70};       // 控制任务发布，输出任务按帧读取
    std::atomic<float> input_gain_{0.0f};      // 控制任务发布，输入任务按帧读取

    /** @brief 从具体驱动读取 PCM，返回实际采样数 */
    virtual int Read(int16_t* dest, int samples) = 0;

    /** @brief 向具体驱动写入 PCM，返回实际采样数 */
    virtual int Write(const int16_t* data, int samples) = 0;
};

#endif  // _AUDIO_CODEC_H
