#pragma once

#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 使用软件JPEG解码器缩放为RGB565，失败时清空输出
 * @param src JPEG输入数据
 * @param src_len 输入字节数
 * @param max_w 目标最大宽度，非正值不限制宽度
 * @param max_h 目标最大高度，非正值不限制高度
 * @param out 接收像素缓冲，成功后由调用方使用heap_caps_free释放
 * @param out_len 接收输出字节数
 * @param width 接收像素宽度
 * @param height 接收像素高度
 * @param stride 接收每行字节数
 * @return ESP_OK表示成功，否则返回校验、分配或解码错误
 * @note 解码库缩放能力有限，调用方仍须按返回尺寸检查目标边界。
 */
esp_err_t jpeg_to_image_fit(const uint8_t* src, size_t src_len, int max_w, int max_h, uint8_t** out, size_t* out_len,
                            size_t* width, size_t* height, size_t* stride);

#ifdef __cplusplus
}
#endif
