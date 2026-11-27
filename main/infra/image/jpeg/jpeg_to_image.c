#include <string.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "esp_jpeg_common.h"
#include "esp_jpeg_dec.h"
#include "jpeg_to_image.h"

#define TAG "jpeg_to_image"

static uint16_t AlignDown8(uint16_t v) {
    return (uint16_t)(v & ~0x7);
}

static uint16_t AlignUp8(uint16_t v) {
    return (uint16_t)((v + 7) & ~0x7);
}

// 像素仅使用 PSRAM，避免分配失败后挤占内部 RAM。
static uint8_t* AllocJpegOutBuf(size_t size) {
    return (uint8_t*)heap_caps_aligned_calloc(16, 1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static esp_err_t JpegErrorToEsp(jpeg_error_t error) {
    switch (error) {
        case JPEG_ERR_OK:
            return ESP_OK;
        case JPEG_ERR_NO_MEM:
            return ESP_ERR_NO_MEM;
        case JPEG_ERR_UNSUPPORT_FMT:
        case JPEG_ERR_UNSUPPORT_STD:
            return ESP_ERR_NOT_SUPPORTED;
        case JPEG_ERR_INVALID_PARAM:
            return ESP_ERR_INVALID_ARG;
        case JPEG_ERR_NO_MORE_DATA:
        case JPEG_ERR_BAD_DATA:
            return ESP_ERR_INVALID_RESPONSE;
        default:
            return ESP_FAIL;
    }
}

static void FreeJpegOutBuf(uint8_t* p) {
    if (p == NULL) {
        return;
    }
    heap_caps_free(p);
}

/** 缩到不超过 max，且边长为 8 的倍数；库限制最多缩到约 1/8 */
static void ComputeFitScale(uint16_t src_w, uint16_t src_h, int max_w, int max_h, uint16_t* out_w, uint16_t* out_h) {
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
    const char* stage = "probe_open";

    // 先探尺寸（不 scale）
    {
        jpeg_dec_config_t probe = DEFAULT_JPEG_DEC_CONFIG();
        probe.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
        probe.rotate = JPEG_ROTATE_0D;
        jpeg_ret = jpeg_dec_open(&probe, &jpeg_dec);
        if (jpeg_ret != JPEG_ERR_OK) {
            ret = JpegErrorToEsp(jpeg_ret);
            goto jpeg_dec_failed;
        }
        jpeg_io.inbuf = (uint8_t*)src;
        jpeg_io.inbuf_len = (int)src_len;
        stage = "probe_header";
        jpeg_ret = jpeg_dec_parse_header(jpeg_dec, &jpeg_io, &out_info);
        if (jpeg_ret != JPEG_ERR_OK) {
            ret = JpegErrorToEsp(jpeg_ret);
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
            ESP_LOGD(TAG, "JPEG scale %ux%u -> %ux%u", out_info.width, out_info.height, scale_w, scale_h);
        }

        stage = "open";
        jpeg_ret = jpeg_dec_open(&config, &jpeg_dec);
        if (jpeg_ret != JPEG_ERR_OK) {
            ret = JpegErrorToEsp(jpeg_ret);
            goto jpeg_dec_failed;
        }

        jpeg_io.inbuf = (uint8_t*)src;
        jpeg_io.inbuf_len = (int)src_len;
        jpeg_io.inbuf_remain = 0;
        jpeg_io.outbuf = NULL;
        jpeg_io.out_size = 0;

        stage = "header";
        jpeg_ret = jpeg_dec_parse_header(jpeg_dec, &jpeg_io, &out_info);
        if (jpeg_ret != JPEG_ERR_OK) {
            ret = JpegErrorToEsp(jpeg_ret);
            goto jpeg_dec_failed;
        }

        stage = "output_size";
        jpeg_ret = jpeg_dec_get_outbuf_len(jpeg_dec, &outbuf_len);
        if (jpeg_ret != JPEG_ERR_OK || outbuf_len <= 0) {
            ret = jpeg_ret != JPEG_ERR_OK ? JpegErrorToEsp(jpeg_ret) : ESP_ERR_INVALID_RESPONSE;
            goto jpeg_dec_failed;
        }

        stage = "allocate";
        out_buf = AllocJpegOutBuf((size_t)outbuf_len);
        if (out_buf == NULL) {
            ret = ESP_ERR_NO_MEM;
            goto jpeg_dec_failed;
        }

        jpeg_io.outbuf = out_buf;
        stage = "process";
        jpeg_ret = jpeg_dec_process(jpeg_dec, &jpeg_io);
        if (jpeg_ret != JPEG_ERR_OK) {
            ret = JpegErrorToEsp(jpeg_ret);
            goto jpeg_dec_failed;
        }

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
    ESP_LOGE(TAG, "operation=decode stage=%s result=%d code=%d", stage, (int)ret, (int)jpeg_ret);
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

esp_err_t jpeg_to_image_fit(const uint8_t* src, size_t src_len, int max_w, int max_h, uint8_t** out, size_t* out_len,
                            size_t* width, size_t* height, size_t* stride) {
    if (out) {
        *out = NULL;
    }
    if (out_len) {
        *out_len = 0;
    }
    if (width) {
        *width = 0;
    }
    if (height) {
        *height = 0;
    }
    if (stride) {
        *stride = 0;
    }
    if (src == NULL || src_len == 0 || out == NULL || out_len == NULL || width == NULL || height == NULL ||
        stride == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    // 封面/正文图：直接走软件 scale，避免硬解先撑满幅 RGB565
    return decode_with_new_jpeg(src, src_len, max_w, max_h, out, out_len, width, height, stride);
}
