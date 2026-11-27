#pragma once

#include "config.h"

struct UiResourceProfile {
    int ui_font_size;              // 界面常规字号
    int ui_large_font_size;        // 大号内容字号
    int ui_caption_font_size;      // 辅助说明字号
    int emphasis_native_bpp;       // 强调资源精度，0 表示不请求独立资源
    int emphasis_expand_px;        // 缺少强调资源时横向扩展像素数
    int reader_font_size;          // 阅读默认字号
    int reader_font_min;           // 矢量阅读字号下限
    int reader_font_max;           // 矢量阅读字号上限
    const char* reader_font_file;  // 默认阅读位图字体资源
};

inline constexpr UiResourceProfile kUiResources{BOARD_UI_FONT_SIZE,
                                                BOARD_UI_LARGE_FONT_SIZE,
                                                BOARD_UI_CAPTION_FONT_SIZE,
                                                BOARD_UI_EMPHASIS_NATIVE_BPP,
                                                BOARD_UI_EMPHASIS_EXPAND_PX,
                                                BOARD_READER_FONT_SIZE,
                                                BOARD_READER_FONT_MIN,
                                                BOARD_READER_FONT_MAX,
                                                BOARD_READER_FONT_FILE};

/** @brief 判断资源包中完整系统界面字号；数学专用规格不用于普通文本。 */
static constexpr bool UiTextFontSizeSupported(int size) {
    return size == 25 || size == 30;
}
static_assert(kUiResources.emphasis_expand_px >= 0 && kUiResources.emphasis_expand_px <= 2);
static_assert(kUiResources.emphasis_native_bpp == 0 || kUiResources.emphasis_native_bpp == 4);
static_assert(UiTextFontSizeSupported(kUiResources.ui_font_size));
static_assert(UiTextFontSizeSupported(kUiResources.ui_large_font_size));
static_assert(UiTextFontSizeSupported(kUiResources.ui_caption_font_size));

/** @brief 获取构建时选定的产品字体与资源规格，与显示控制器无关。 */
static constexpr const UiResourceProfile& GetUiResourceProfile() {
    return kUiResources;
}
