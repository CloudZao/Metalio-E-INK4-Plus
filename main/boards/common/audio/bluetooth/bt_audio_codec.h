#pragma once

#include <mutex>
#include <atomic>
#include <driver/gpio.h>
#include "boards/common/audio/audio_codec.h"

/**
 * @brief 外置蓝牙音频芯片 I2S duplex（ESP 为 Slave）
 */
class BTAudioCodec : public AudioCodec {
protected:
    std::mutex data_if_mutex_;
    std::mutex read_mutex_;                    // 保护接收操作与资源释放
    std::mutex lifecycle_mutex_;               // 串行化初始化和释放
    std::atomic<bool> hardware_ready_{false};  // 双工通道均已启用
    bool tx_enabled_ = false;                  // 发送通道已启用
    bool rx_enabled_ = false;                  // 接收通道已启用
    void ReleaseChannelsLocked();

    int Write(const int16_t* data, int samples) override;
    int Read(int16_t* dest, int samples) override;

public:
    ~BTAudioCodec() override;

    /** @brief 查询双工硬件是否已初始化并启用 */
    bool HardwareReady() const override {
        return hardware_ready_.load();
    }

    /** @brief 等待读写完成后释放 I2S 通道与 DMA 资源 */
    void ReleaseHardware() override;
};

/**
 * @brief 全双工 I2S 通道
 */
class BTAudioCodecDuplex : public BTAudioCodec {
public:
    /** @brief 记录双工 I2S 采样率和引脚配置，实际通道由 Start 按需申请 */
    BTAudioCodecDuplex(int input_sample_rate, int output_sample_rate, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout,
                       gpio_num_t din);

    /** @brief 按需初始化双工 I2S，重复调用保持幂等 */
    void Start() override;

private:
    gpio_num_t bclk_, ws_, dout_, din_;
};
