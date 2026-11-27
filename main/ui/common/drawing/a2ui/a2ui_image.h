#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define A2UI_L8_MAGIC 0x384C3241u  // A2L8 图像封装标识，按小端存储

#define A2UI_IMG_MAX_SURFACES 3  // 同时显示的图像表面上限，超出时保留占位

/** @brief 绑定图像显示对象，供解码完成后请求重绘。 */
void a2ui_image_set_disp(lv_display_t* disp);

/** @brief 设置图像地址解析所用的来源基址，空值清除配置。 */
void a2ui_image_set_http_origin(const char* origin);

/**
 * @brief 为图像控件加载图片，每个控件的显示缓冲区由本模块持有。
 * @note 调用方须持 LVGL 锁；内存图像同步显示，文件或网络图像异步加载。
 * @note max_w 和 max_h 当前仅保留接口兼容，不作为解码尺寸限制。
 * @note 页面销毁时先取消加载，再销毁图像控件，最后清理显示缓冲区。
 */
esp_err_t a2ui_image_load_async(lv_obj_t* img_obj, const char* url, int max_w, int max_h);

/**
 * @brief 复制并登记本地图像数据，相同标识替换已有内容。
 * @note 优先使用 PSRAM；取消加载或清理显示缓冲区不会删除本地登记。
 */
esp_err_t a2ui_image_put_local(const char* url, const uint8_t* a2i1, size_t len);

/**
 * @brief 复制并登记 L8 像素为本地图像，像素值零为黑、255 为白。
 * @note 本地登记独立于显示缓冲区生命周期，可供翻页重新加载。
 */
esp_err_t a2ui_image_put_local_l8(const char* url, uint16_t w, uint16_t h, const uint8_t* pixels, size_t pixels_len);

/**
 * @brief 使在途加载结果失效并清空等待队列，保留已显示的像素缓冲区。
 * @note 须在持有 LVGL 锁时调用。
 */
void a2ui_image_abort_loads(void);

/**
 * @brief 取消加载并释放全部显示缓冲区，保留本地图像登记。
 * @note 须在持有 LVGL 锁且相关图像控件已销毁后调用。
 */
void a2ui_image_clear(void);

#ifdef __cplusplus
}
#endif
