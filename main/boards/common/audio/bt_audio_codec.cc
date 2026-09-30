#include "bt_audio_codec.h"

#include "metalio_audio.h"

#include <cmath>
#include <cstring>
#include <esp_log.h>
#include <vector>

#define TAG "BTAudioCodec"

BTAudioCodec::~BTAudioCodec() {
    if (rx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
    }
    if (tx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
    }
}

BTAudioCodecDuplex::BTAudioCodecDuplex(int input_sample_rate, int output_sample_rate,
                                       gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout,
                                       gpio_num_t din) {
    duplex_ = true;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    // 与 397 一致：参考输入开 → 双声道（麦 + 回采）
    input_reference_ = true;
    input_channels_ = input_reference_ ? 2 : 1;

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_SLAVE,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .allow_pd = false,
        .intr_priority = 0,
        .tx_destination = I2S_DESTINATION_DMA,
        .rx_destination = I2S_DESTINATION_DMA,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    // 对齐 397：Slave + 立体声 32bit；勿再设 bclk_div（由主机 BH 提供 BCLK）
    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg.sample_rate_hz = static_cast<uint32_t>(output_sample_rate_);
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
#if SOC_I2S_HW_VERSION_2
    std_cfg.clk_cfg.ext_clk_freq_hz = 0;
#endif
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    std_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_32BIT;
    std_cfg.slot_cfg.ws_pol = false;
    std_cfg.slot_cfg.bit_shift = true;
#if SOC_I2S_HW_VERSION_2
    std_cfg.slot_cfg.left_align = true;
    std_cfg.slot_cfg.big_endian = false;
    std_cfg.slot_cfg.bit_order_lsb = false;
#endif
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = bclk;
    std_cfg.gpio_cfg.ws = ws;
    std_cfg.gpio_cfg.dout = dout;
    std_cfg.gpio_cfg.din = din;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "Duplex channels created");
}

int BTAudioCodec::Write(const int16_t* data, int samples) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    // 双声道：每个采样点输出左右两个声道（与 397 一致）
    std::vector<int32_t> buffer(static_cast<size_t>(samples) * 2);
    int32_t volume_factor =
        static_cast<int32_t>(pow(double(output_volume_) / 100.0, 2) * 65536);

    for (int i = 0; i < samples; i++) {
        int64_t temp = int64_t(data[i]) * volume_factor;
        int32_t processed = temp > INT32_MAX   ? INT32_MAX
                            : temp < INT32_MIN ? INT32_MIN
                                               : static_cast<int32_t>(temp);
        buffer[static_cast<size_t>(i) * 2] = processed;
        buffer[static_cast<size_t>(i) * 2 + 1] = processed;
    }

    // 播放路径不经 MetalioAudio_WriteSpk，这里补拉 PA（PowerPolicy 亦会按状态开）
    MetalioAudio_SetPa(true);

    size_t bytes_written = 0;
    ESP_ERROR_CHECK(i2s_channel_write(tx_handle_, buffer.data(),
                                      static_cast<size_t>(samples) * 2 * sizeof(int32_t),
                                      &bytes_written, portMAX_DELAY));
    return static_cast<int>(bytes_written / (2 * sizeof(int32_t)));
}

int BTAudioCodec::Read(int16_t* dest, int samples) {
    // 与 397 一致：samples 已含 input_channels_，按 slot 1:1 读，勿再 *2
    size_t bytes_read = 0;
    std::vector<int32_t> bit32_buffer(static_cast<size_t>(samples));
    if (i2s_channel_read(rx_handle_, bit32_buffer.data(),
                         static_cast<size_t>(samples) * sizeof(int32_t), &bytes_read,
                         portMAX_DELAY) != ESP_OK) {
        ESP_LOGE(TAG, "Read Failed!");
        return 0;
    }

    samples = static_cast<int>(bytes_read / sizeof(int32_t));
    for (int i = 0; i < samples; i++) {
        int32_t value = bit32_buffer[static_cast<size_t>(i)] >> 12;
        dest[i] = (value > INT16_MAX)    ? INT16_MAX
                  : (value < -INT16_MAX) ? -INT16_MAX
                                         : static_cast<int16_t>(value);
    }
    return samples;
}
