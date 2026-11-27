#pragma once
#include <memory>
#include <lvgl.h>

enum class UiFontRole {
    Body,             // 正文、操作文字及页内提示，随产品默认字号
    Title,            // 页面及内容标题，与正文同字号
    Emphasis,         // 与正文同字号的强调文字
    TitleEmphasis,    // 导航卡片等标题的同字号强调文字
    Large,            // 大号内容、数值和需要较大字形的控件
    LargeEmphasis,    // 与大号内容同字号的强调文字
    CaptionEmphasis,  // 紧凑区域强调文字，缩小时保留字重
    Caption,          // 辅助说明及紧凑区域文字
};

/** @brief 按界面用途取得字体；字号、字重映射及资源回退由公共层维护。 */
const lv_font_t* UiFont(UiFontRole role = UiFontRole::Body);

/** @brief 获取紧凑字体，保留首选字体的强调样式。 */
const lv_font_t* UiFontCompact(const lv_font_t* preferred);

struct UiFontResource;
using UiFontResourcePtr = std::shared_ptr<UiFontResource>;

/** @brief 在后台加载 fonts_ttf 内的 TTF/OTF 字体及本机字号；空文件名表示默认字体。 */
UiFontResourcePtr UiFontLoadFile(const char* file);

/** @brief 取得预览字体，调用方须保留资源所有权；介质代次变化后返回空。 */
const lv_font_t* UiFontPreview(const UiFontResourcePtr& resource);

/** @brief 将已加载资源应用到系统界面；内部取得 UI 锁，保留旧控件的字体引用。 */
bool UiFontSetFile(const UiFontResourcePtr& resource);

/** @brief 获取当前系统字体文件名，空字符串表示内置字体；须在 UI 线程或持锁时读取。 */
const char* UiFontFile();

/** @brief 启动时读取已保存的系统字体选择，无卡或资源不可用时保留内置字体。 */
void UiFontRestore();

/** @brief 在 UI 线程处理启动恢复与 SD 介质变化，加载工作交给现有存储任务。 */
void UiFontPoll();
