#pragma once

/**
 * @file a2ui_img_cache.h
 * @brief SD 卡上的 A2I1/A2L8 会话图片缓存
 *
 * 仅负责目录、启动清理和按键读写；显示与 HTTP 下载由 a2ui_image.cc 处理。
 * 目录由 SD_PATH_A2UI_CACHE 定义；无 SD 卡时禁用缓存，有卡时开机清空。
 * 缓存键为解析后图片 URL 的 FNV-1a，与下载时使用的地址一致。
 * 达到容量或文件数上限时清空缓存后再写入，失败不阻止启动和界面运行。
 * 访问通过互斥锁保护，通常由图片下载任务调用。
 */

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sd_paths.h"

#ifdef __cplusplus
extern "C" {
#endif

#define A2UI_IMG_CACHE_BASE_PATH SD_PATH_A2UI_CACHE // 图片会话缓存目录

/**
 * @brief 初始化缓存目录并清空上次启动的会话文件
 * @return 无 SD 卡时返回 ESP_ERR_NOT_FOUND；重复调用不重复初始化
 */
esp_err_t a2ui_img_cache_init(void);

/** @brief 查询图片缓存是否可用 */
bool a2ui_img_cache_is_ready(void);

/**
 * @brief 按图片地址读取缓存，优先使用 SPIRAM 分配输出缓冲区
 * @note 成功时由调用方通过 heap_caps_free 或 free 释放输出缓冲区
 * @return 未命中时返回 ESP_ERR_NOT_FOUND
 */
esp_err_t a2ui_img_cache_load(const char *url, uint8_t **out_buf, size_t *out_len);

/**
 * @brief 保存已校验的 A2I1/A2L8 图片数据
 * @note 超出缓存预算时先清空缓存，写入失败不影响界面运行
 */
esp_err_t a2ui_img_cache_store(const char *url, const uint8_t *data, size_t len);

/** @brief 移除指定图片的缓存，供缓存解析失败时重新下载 */
void a2ui_img_cache_invalidate(const char *url);

#ifdef __cplusplus
}
#endif
