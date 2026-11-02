#pragma once

#include <lvgl.h>
#include <vector>
#include <cstdint>

namespace reader { struct RasterImage; }

/** @brief 保存当前界面的原始帧及坐标映射；已有底图时复用，须持 UI 锁。 */
bool WallpaperFrame_Capture();
/** @brief 当前是否持有干净底图。 */
bool WallpaperFrame_HasBase();
/** @brief 获取底图可绘区尺寸，供后台缩放解码。 */
lv_point_t WallpaperFrame_Size();
/** @brief 恢复干净底图，须持 UI 锁。 */
bool WallpaperFrame_Restore();
/** @brief 居中叠加适配屏幕后的图片；透明度为空时直接覆盖，每次从干净底图合成。 */
bool WallpaperFrame_Draw(const reader::RasterImage& image, const std::vector<uint8_t>& alpha);
/** @brief 退出待机后释放底图。 */
void WallpaperFrame_Clear();
