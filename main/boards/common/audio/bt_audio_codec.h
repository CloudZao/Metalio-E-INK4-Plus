#pragma once

#include "audio_codec.h"

#include <driver/gpio.h>
#include <mutex>

/**
 * @brief 外置蓝牙音频芯片 I2S duplex（ESP 为 Slave）
 */
class BTAudioCodec : public AudioCodec {
protected:
    std::mutex data_if_mutex_;

    int Write(const int16_t* data, int samples) override;
    int Read(int16_t* dest, int samples) override;

public:
    ~BTAudioCodec() override;
};

/**
 * @brief 全双工 I2S 通道
 */
class BTAudioCodecDuplex : public BTAudioCodec {
public:
    BTAudioCodecDuplex(int input_sample_rate, int output_sample_rate, gpio_num_t bclk,
                       gpio_num_t ws, gpio_num_t dout, gpio_num_t din);
};
