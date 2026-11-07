#pragma once

#include <lvgl.h>

struct HomeIconCompressed {
    lv_image_dsc_t image; // Flash 中的压缩像素描述，必须是首成员
    const uint32_t* tile_offsets; // A8/ARGB8888 每 16 行、AL88 整图 zlib 数据的起止偏移
    lv_color_format_t color_format; // 解压后的 ARGB8888、A8 或 AL88 格式
};

/** @brief 优先使用 Flash 中的无损图标，未收录时保留资源路径 */
const void* HomeIconSource(const char* path);
/** @brief 确保内嵌无损图像解码器已注册 */
bool HomeIconDecoderReady();

/** @brief 将黑色透明图标完整解码为 A8 后绑定；占用控件 user_data，销毁时释放缓存 */
bool HomeIconSetFullSource(lv_obj_t* image, const char* path);
