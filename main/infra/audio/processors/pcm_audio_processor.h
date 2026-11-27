#pragma once

#include <functional>
#include <vector>
#include "boards/common/audio/audio_codec.h"

class PcmAudioProcessor {
public:
    /** @brief 绑定音频硬件并设置输入帧时长 */
    void Initialize(AudioCodec* codec, int frame_duration_ms);

    /** @brief 停止处理并释放硬件引用 */
    void Deinitialize();

    /** @brief 选择麦克风通道并交付完整 PCM 帧 */
    void Feed(std::vector<int16_t>&& data);

    /** @brief 开始接收输入帧 */
    void Start();

    /** @brief 停止接收输入帧 */
    void Stop();

    /** @brief 查询处理是否运行 */
    bool IsRunning();

    /** @brief 设置 PCM 帧接收函数 */
    void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback);

    /** @brief 获取所需输入采样数 */
    size_t GetFeedSize();

private:
    AudioCodec* codec_ = nullptr;                                  // 借用板级音频硬件
    bool is_running_ = false;                                      // 输入处理状态
    size_t frame_samples_ = 0;                                     // 单帧采样数
    std::function<void(std::vector<int16_t>&&)> output_callback_;  // PCM 帧接收函数
};
