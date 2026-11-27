#pragma once
#include <lvgl.h>

// 内容用途独立于文件格式；仅显式标记的可见内容参与灰度判定。
enum class EpdContentRole {
    Monochrome,          // 二值内容
    FastMonochrome,      // 整个子树使用快速二值显示
    Shelf,               // 书架区域
    Cover,               // 书籍封面
    Illustration,        // 阅读插图
    WallpaperThumbnail,  // 壁纸缩略图
    WallpaperPreview,    // 壁纸预览
    Photo,               // 照片
    ReaderAntialias,     // 阅读文字抗锯齿
    AssistantImage,      // 助手图片
    ApplicationImage     // 应用已完成量化的图片，不再施加系统图片压暗
};

/** @brief 设置对象的内容用途；快速二值模式作用于子树，不改写图像数据 */
void EpdSetContentRole(lv_obj_t* object, EpdContentRole role);

/** @brief 判断显示器当前可见内容是否需要灰度显示 */
bool EpdVisibleContentNeedsGray(lv_display_t* display);

/** @brief 初始化按内容用途选择显示模式的混合钩子 */
bool EpdContentBlendInit();
