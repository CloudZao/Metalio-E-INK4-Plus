#include <esp_check.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <sys/param.h>

#include "esp_jpeg_common.h"
#include "esp_jpeg_dec.h"

#include "jpeg_to_image.h"

#ifdef CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL MAX(CONFIG_LOG_DEFAULT_LEVEL, ESP_LOG_DEBUG)
#endif  // CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
#include <esp_log.h>

#ifdef CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER
#include "driver/jpeg_decode.h"
#endif

#define TAG "jpeg_to_image"

static uint16_t AlignDown8(uint16_t v) {
    return (uint16_t)(v & ~0x7);
}

static uint16_t AlignUp8(uint16_t v) {
    return (uint16_t)((v + 7) & ~0x7);
}

/** 大图优先 PSRAM；内部 DRAM 仅作兜底（封面全尺寸 RGB565 常 >1MB） */
static uint8_t* AllocJpegOutBuf(size_t size) {
    uint8_t* p = (uint8_t*)heap_caps_aligned_calloc(16, 1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p != NULL) {
        return p;
    }
    p = (uint8_t*)jpeg_calloc_align(size, 16);
    if (p != NULL) {
        return p;
    }
    return (uint8_t*)heap_caps_aligned_calloc(16, 1, size, MALLOC_CAP_8BIT);
}

static void FreeJpegOutBuf(uint8_t* p) {
    if (p == NULL) {
        return;
    }
    // jpeg_calloc_align 与 heap_caps_aligned_* 均可 heap_caps_free（现有调用方亦如此）
    heap_caps_free(p);
}

/** 缩到不超过 max，且边长为 8 的倍数；库限制最多缩到约 1/8 */
static void ComputeFitScale(uint16_t src_w, uint16_t src_h, int max_w, int max_h, uint16_t* out_w,
                            uint16_t* out_h) {
    *out_w = src_w;
    *out_h = src_h;
    if (max_w <= 0 || max_h <= 0 || src_w == 0 || src_h == 0) {
        return;
    }
    if (src_w <= (uint16_t)max_w && src_h <= (uint16_t)max_h) {
        return;
    }

    const float sx = (float)max_w / (float)src_w;
    const float sy = (float)max_h / (float)src_h;
    const float s = sx < sy ? sx : sy;
    uint16_t tw = AlignDown8((uint16_t)(src_w * s));
    uint16_t th = AlignDown8((uint16_t)(src_h * s));
    if (tw < 8) {
        tw = 8;
    }
    if (th < 8) {
        th = 8;
    }

    // scale 目标 ≥ ceil(src/8)，再向上对齐到 8
    uint16_t min_w = AlignUp8((uint16_t)((src_w + 7) / 8));
    uint16_t min_h = AlignUp8((uint16_t)((src_h + 7) / 8));
    if (min_w > src_w) {
        min_w = AlignDown8(src_w);
        if (min_w < 8) {
            min_w = src_w;
        }
    }
    if (min_h > src_h) {
        min_h = AlignDown8(src_h);
        if (min_h < 8) {
            min_h = src_h;
        }
    }
    if (tw < min_w) {
        tw = min_w;
    }
    if (th < min_h) {
        th = min_h;
    }

    *out_w = tw;
    *out_h = th;
}

static esp_err_t decode_with_new_jpeg(const uint8_t* src, size_t src_len, int max_w, int max_h, uint8_t** out,
                                      size_t* out_len, size_t* width, size_t* height, size_t* stride) {
    ESP_LOGD(TAG, "Decoding JPEG with software decoder (fit %dx%d)", max_w, max_h);
    esp_err_t ret = ESP_OK;
    jpeg_error_t jpeg_ret = JPEG_ERR_OK;
    uint8_t* out_buf = NULL;
    jpeg_dec_handle_t jpeg_dec = NULL;
    jpeg_dec_io_t jpeg_io = {0};
    jpeg_dec_header_info_t out_info = {0};
    uint16_t scale_w = 0;
    uint16_t scale_h = 0;
    int outbuf_len = 0;

    // 先探尺寸（不 scale）
    {
        jpeg_dec_config_t probe = DEFAULT_JPEG_DEC_CONFIG();
        probe.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
        probe.rotate = JPEG_ROTATE_0D;
        jpeg_ret = jpeg_dec_open(&probe, &jpeg_dec);
        if (jpeg_ret != JPEG_ERR_OK) {
            ESP_LOGE(TAG, "Failed to open JPEG decoder");
            ret = ESP_FAIL;
            goto jpeg_dec_failed;
        }
        jpeg_io.inbuf = (uint8_t*)src;
        jpeg_io.inbuf_len = (int)src_len;
        jpeg_ret = jpeg_dec_parse_header(jpeg_dec, &jpeg_io, &out_info);
        if (jpeg_ret != JPEG_ERR_OK) {
            ESP_LOGE(TAG, "Failed to parse JPEG header");
            ret = ESP_ERR_INVALID_ARG;
            goto jpeg_dec_failed;
        }
        ComputeFitScale(out_info.width, out_info.height, max_w, max_h, &scale_w, &scale_h);
        jpeg_dec_close(jpeg_dec);
        jpeg_dec = NULL;
    }

    {
        jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
        config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
        config.rotate = JPEG_ROTATE_0D;
        if (scale_w != out_info.width || scale_h != out_info.height) {
            config.scale.width = scale_w;
            config.scale.height = scale_h;
            ESP_LOGI(TAG, "JPEG scale %ux%u -> %ux%u", out_info.width, out_info.height, scale_w, scale_h);
        }

        jpeg_ret = jpeg_dec_open(&config, &jpeg_dec);
        if (jpeg_ret != JPEG_ERR_OK) {
            ESP_LOGE(TAG, "Failed to open JPEG decoder (scaled)");
            ret = ESP_FAIL;
            goto jpeg_dec_failed;
        }

        jpeg_io.inbuf = (uint8_t*)src;
        jpeg_io.inbuf_len = (int)src_len;
        jpeg_io.inbuf_remain = 0;
        jpeg_io.outbuf = NULL;
        jpeg_io.out_size = 0;

        jpeg_ret = jpeg_dec_parse_header(jpeg_dec, &jpeg_io, &out_info);
        if (jpeg_ret != JPEG_ERR_OK) {
            ESP_LOGE(TAG, "Failed to parse JPEG header (scaled)");
            ret = ESP_ERR_INVALID_ARG;
            goto jpeg_dec_failed;
        }

        jpeg_ret = jpeg_dec_get_outbuf_len(jpeg_dec, &outbuf_len);
        if (jpeg_ret != JPEG_ERR_OK || outbuf_len <= 0) {
            // 旧路径兜底：按头寸估算
            outbuf_len = (int)out_info.width * (int)out_info.height * 2;
        }

        out_buf = AllocJpegOutBuf((size_t)outbuf_len);
        if (out_buf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for JPEG output buffer (%d bytes)", outbuf_len);
            ret = ESP_ERR_NO_MEM;
            goto jpeg_dec_failed;
        }

        jpeg_io.outbuf = out_buf;
        jpeg_ret = jpeg_dec_process(jpeg_dec, &jpeg_io);
        if (jpeg_ret != JPEG_ERR_OK) {
            ESP_LOGE(TAG, "Failed to decode JPEG");
            ret = ESP_FAIL;
            goto jpeg_dec_failed;
        }

        ESP_LOG_BUFFER_HEXDUMP(TAG, out_buf, MIN(out_info.width * out_info.height * 2, 256), ESP_LOG_DEBUG);

        *out = out_buf;
        out_buf = NULL;
        *out_len = (size_t)out_info.width * out_info.height * 2;
        *width = (size_t)out_info.width;
        *height = (size_t)out_info.height;
        *stride = (size_t)out_info.width * 2;
        jpeg_dec_close(jpeg_dec);
        jpeg_dec = NULL;
        return ESP_OK;
    }

jpeg_dec_failed:
    if (jpeg_dec) {
        jpeg_dec_close(jpeg_dec);
        jpeg_dec = NULL;
    }
    FreeJpegOutBuf(out_buf);
    out_buf = NULL;

    *out = NULL;
    *out_len = 0;
    *width = 0;
    *height = 0;
    *stride = 0;
    return ret;
}

#ifdef CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER
static esp_err_t decode_with_hardware_jpeg(const uint8_t* src, size_t src_len, uint8_t** out, size_t* out_len,
                                           size_t* width, size_t* height, size_t* stride) {
    ESP_LOGD(TAG, "Decoding JPEG with hardware decoder");
    esp_err_t ret = ESP_OK;

    jpeg_decoder_handle_t jpeg_dec = NULL;
    uint8_t* bit_stream = NULL;
    uint8_t* out_buf = NULL;
    size_t out_buf_len = 0;
    size_t tx_buffer_size = 0;
    size_t rx_buffer_size = 0;

    jpeg_decode_engine_cfg_t eng_cfg = {
        .intr_priority = 1,
        .timeout_ms = 1000,
    };

    jpeg_decode_cfg_t decode_cfg_rgb = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };

    ret = jpeg_new_decoder_engine(&eng_cfg, &jpeg_dec);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create JPEG decoder engine");
        goto jpeg_hw_dec_failed;
    }

    jpeg_decode_memory_alloc_cfg_t tx_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };

    jpeg_decode_memory_alloc_cfg_t rx_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };

    bit_stream = (uint8_t*)jpeg_alloc_decoder_mem(src_len, &tx_mem_cfg, &tx_buffer_size);
    if (bit_stream == NULL || tx_buffer_size < src_len) {
        ESP_LOGE(TAG, "Failed to allocate memory for JPEG bit stream");
        ret = ESP_ERR_NO_MEM;
        goto jpeg_hw_dec_failed;
    }

    memcpy(bit_stream, src, src_len);

    jpeg_decode_picture_info_t header_info;
    ESP_GOTO_ON_ERROR(jpeg_decoder_get_info(bit_stream, src_len, &header_info), jpeg_hw_dec_failed, TAG,
                      "Failed to get JPEG header info");

    ESP_LOGD(TAG, "JPEG header info: width=%d, height=%d, sample_method=%d", header_info.width, header_info.height,
             (int)header_info.sample_method);

    switch (header_info.sample_method) {
        case JPEG_DOWN_SAMPLING_GRAY:
        case JPEG_DOWN_SAMPLING_YUV444:
            out_buf_len = header_info.width * header_info.height * 2;
            *stride = header_info.width * 2;
            break;
        case JPEG_DOWN_SAMPLING_YUV422:
        case JPEG_DOWN_SAMPLING_YUV420:
            out_buf_len = ((header_info.width + 15) & ~15) * ((header_info.height + 15) & ~15) * 2;
            *stride = ((header_info.width + 15) & ~15) * 2;
            break;
        default:
            ESP_LOGE(TAG, "Unsupported JPEG sample method");
            ret = ESP_ERR_NOT_SUPPORTED;
            goto jpeg_hw_dec_failed;
    }

    out_buf = (uint8_t*)jpeg_alloc_decoder_mem(out_buf_len, &rx_mem_cfg, &rx_buffer_size);
    if (out_buf == NULL || rx_buffer_size < out_buf_len) {
        ESP_LOGE(TAG, "Failed to allocate memory for JPEG output buffer");
        ret = ESP_ERR_NO_MEM;
        goto jpeg_hw_dec_failed;
    }

    uint32_t out_size = 0;

    ESP_GOTO_ON_ERROR(
        jpeg_decoder_process(jpeg_dec, &decode_cfg_rgb, bit_stream, src_len, out_buf, out_buf_len, &out_size),
        jpeg_hw_dec_failed, TAG, "Failed to decode JPEG");

    ESP_LOGD(TAG, "Expected %d bytes, got %" PRIu32 " bytes", out_buf_len, out_size);

    if (out_size != out_buf_len) {
        ESP_LOGE(TAG, "Decoded image size mismatch: Expected %zu bytes, got %" PRIu32 " bytes", out_buf_len, out_size);
        ret = ESP_ERR_INVALID_SIZE;
        goto jpeg_hw_dec_failed;
    }

    if (header_info.sample_method == JPEG_DOWN_SAMPLING_GRAY) {
        // convert GRAY8 to RGB565
        uint32_t i = header_info.width * header_info.height;
        do {
            --i;
            uint8_t r = (out_buf[i] >> 3) & 0x1F;
            uint8_t g = (out_buf[i] >> 2) & 0x3F;
            // b is same as r
            uint16_t rgb565 = (r << 11) | (g << 5) | r;
            out_buf[2 * i + 1] = (rgb565 >> 8) & 0xFF;
            out_buf[2 * i] = rgb565 & 0xFF;
        } while (i != 0);
        out_size = header_info.width * header_info.height * 2;
        ESP_LOGD(TAG, "Converted GRAY8 to RGB565, new size: %zu", out_size);
    }

    ESP_LOG_BUFFER_HEXDUMP(TAG, out_buf, MIN(out_size, 256), ESP_LOG_DEBUG);

    *out = out_buf;
    out_buf = NULL;
    *out_len = (size_t)out_size;
    jpeg_del_decoder_engine(jpeg_dec);
    jpeg_dec = NULL;
    heap_caps_free(bit_stream);
    bit_stream = NULL;
    *width = header_info.width;
    *height = header_info.height;

    return ret;

jpeg_hw_dec_failed:
    if (out_buf) {
        heap_caps_free(out_buf);
        out_buf = NULL;
    }
    if (bit_stream) {
        heap_caps_free(bit_stream);
        bit_stream = NULL;
    }
    if (jpeg_dec) {
        jpeg_del_decoder_engine(jpeg_dec);
        jpeg_dec = NULL;
    }
    *out = NULL;
    *out_len = 0;
    *width = 0;
    *height = 0;
    *stride = 0;
    return ret;
}
#endif  // CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER

esp_err_t jpeg_to_image(const uint8_t* src, size_t src_len, uint8_t** out, size_t* out_len, size_t* width,
                        size_t* height, size_t* stride) {
#ifdef CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif  // CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
    if (src == NULL || src_len == 0 || out == NULL || out_len == NULL || width == NULL || height == NULL ||
        stride == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }
#ifdef CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER
    esp_err_t ret = decode_with_hardware_jpeg(src, src_len, out, out_len, width, height, stride);
    if (ret == ESP_OK) {
        return ret;
    }
    ESP_LOGW(TAG, "Failed to decode with hardware JPEG, fallback to software decoder");
#endif
    return decode_with_new_jpeg(src, src_len, 0, 0, out, out_len, width, height, stride);
}

esp_err_t jpeg_to_image_fit(const uint8_t* src, size_t src_len, int max_w, int max_h, uint8_t** out, size_t* out_len,
                            size_t* width, size_t* height, size_t* stride) {
    if (src == NULL || src_len == 0 || out == NULL || out_len == NULL || width == NULL || height == NULL ||
        stride == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }
    // 封面/正文图：直接走软件 scale，避免硬解先撑满幅 RGB565
    return decode_with_new_jpeg(src, src_len, max_w, max_h, out, out_len, width, height, stride);
}
