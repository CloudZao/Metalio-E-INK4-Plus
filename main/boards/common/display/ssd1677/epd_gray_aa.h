/**
 * @file epd_gray_aa.h
 * @brief 阅读真 4 灰 AA：L8 sidecar + Cross Overlay 掩码打包
 *
 * L8 全屏约 384KB，走 SPIRAM；灰步另复用 flush 侧 plane scratch。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 按面板尺寸分配/复用 L8 sidecar
 * @return true 可用；宽非 8 对齐或分配失败为 false
 */
bool epd_gray_aa_ensure(int width, int height);

/** @brief 开始一帧并绑定逻辑坐标到面板坐标的转换，回调仅在该帧内使用 */
void epd_gray_aa_begin_frame(void (*to_panel)(int x, int y, int* panel_x, int* panel_y));

/**
 * @brief 将 LVGL 逻辑坐标覆盖度写入面板 HTILED 物理坐标（随 DisplayUi 方向）
 * @param lx,ly LVGL 逻辑坐标
 * @param mask_val A2 覆盖度（0 / 85 / 170 / 255）
 */
void epd_gray_aa_plot_logical(int32_t lx, int32_t ly, uint8_t mask_val);

/**
 * @brief 打包 Overlay 单平面到 out（可复用同一 scratch）
 * @param out 输出位图，长度 = 面板像素/8
 * @param plane_msb 0→LSB(0x24)：浅灰=0；1→MSB(0x26)：浅灰=1
 * @note 弱抗锯齿：仅 ~85 低覆盖度边缘转浅灰，~170/255 保留 BW 黑色，0 保持原底
 */
void epd_gray_aa_pack_overlay_plane(uint8_t* out, int plane_msb);

#ifdef __cplusplus
}
#endif
