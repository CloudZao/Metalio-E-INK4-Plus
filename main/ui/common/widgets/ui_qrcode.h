#pragma once
#include <lvgl.h>

constexpr lv_coord_t kUiQrFrameInset = 5;  // 二维码单侧外框与留白总宽度

/** @brief 创建统一黑框、静区和黑白配色的二维码，返回框内二维码控件。 */
lv_obj_t* UiCreateQrCode(lv_obj_t* parent, lv_coord_t side);

/** @brief 调整二维码及外框尺寸，调用后须重新编码内容。 */
void UiResizeQrCode(lv_obj_t* qr, lv_coord_t side);

/** @brief 更新传输二维码，编码失败时保留可定位日志。 */
void ApQrPaint(lv_obj_t* qr, const char* text);

/** @brief 按传输页面可用空间创建二维码。 */
lv_obj_t* ApQrCreate(lv_obj_t* parent);
