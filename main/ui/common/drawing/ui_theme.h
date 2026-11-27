#pragma once
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 读取持久化的界面明暗模式，首次读取须在内部RAM栈初始化阶段完成。 */
bool UiThemeIsDark(void);

/** @brief 请求保存界面明暗模式，返回是否接纳；后台写入失败保留 RAM 状态及待保存快照。 */
bool UiThemeSetDark(bool dark);

/** @brief 在持UI锁时应用原生单色主题和必需的本机系统字体。 */
void UiThemeApply(lv_display_t* display);

#ifdef __cplusplus
}
#endif

/** @brief 获取界面正文、边框及选中底色。 */
static inline lv_color_t UiColorForeground(void) {
    return UiThemeIsDark() ? lv_color_white() : lv_color_black();
}

/** @brief 获取界面背景及选中状态的反色文字。 */
static inline lv_color_t UiColorBackground(void) {
    return UiThemeIsDark() ? lv_color_black() : lv_color_white();
}

/** @brief 转换界面装饰色；照片、壁纸及二维码使用原始颜色。 */
static inline lv_color_t UiColorSurface(uint32_t rgb) {
    const lv_color_t color = lv_color_hex(rgb);
    if (!UiThemeIsDark()) {
        return color;
    }
    // 普通UI按黑白阈值绘制，浅灰反相不能仍落入白色区间。
    return lv_color_luminance(color) > 127 ? lv_color_black() : lv_color_white();
}
