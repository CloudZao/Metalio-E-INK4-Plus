#pragma once

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 获取 Flash 主字库字体，缺字时回退至 SD 扩展字库。
 * 需先烧录 fonts.fontpack 到 font_data 分区（含 25/30@2 等）。
 * SD 文件路径由 SD_PATH_FONT_EXT 定义，扩展字形须匹配字号和灰度。
 * Flash 主字库或所需规格不可用时返回 nullptr。
 */
const lv_font_t* fontpack_lv_font_get(uint16_t size, uint16_t bpp);

/**
 * @brief 按配置获取同字号强调字体，优先选择指定资源，缺失时可合成加粗。
 * @param native_bpp 强调资源精度，0 表示跳过独立资源。
 * @param expand_px 无匹配资源时的横向加粗宽度，0 关闭，支持 1、2px。
 * @note 合成字形保持普通字体的步进、行高和基线；不修改源位图，不创建额外字形缓存。
 * 资源或规格槽不可用时回退同字号普通字体。
 */
const lv_font_t* fontpack_lv_font_emphasis(uint16_t size, uint16_t native_bpp, uint8_t expand_px);

/**
 * @brief 获取指定规格的 SD 扩展字体，供阅读字体缺字回退
 * @note 不依赖 Flash 包包含同一规格；无文件或缺字时由字形回调返回不可用。
 */
const lv_font_t* fontpack_lv_font_sd(uint16_t size, uint16_t bpp);

/**
 * @brief 获取当前产品资源规格指定的界面字体。
 * 可重复调用；缓存实际结果。界面字体是必需资源；规格缺失时记录错误并终止初始化，不使用 LVGL 内置字体。
 */
const lv_font_t* fontpack_lv_font_ui(void);

/** @brief 查询 fontpack 字体的源位图 bpp（不代表字重），其他字体返回 0。 */
uint16_t fontpack_lv_font_bpp(const lv_font_t* font);

/** @brief 尚未映射主字库时尝试初始化 Flash 后端，重复调用复用已有结果。 */
bool fontpack_lv_ensure_ready(void);

#ifdef __cplusplus
}
#endif
