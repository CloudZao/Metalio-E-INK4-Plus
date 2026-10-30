#pragma once

#include <lvgl.h>

struct TtfRuntime;

/** @brief 打开按需渲染的 TTF/OTF 字体；失败返回空指针。 */
TtfRuntime* TtfRuntimeOpen(const char* path, int size_px);
/** @brief 关闭字体文件并释放字形缓存和后台任务。 */
void TtfRuntimeClose(TtfRuntime* font);
/** @brief 获取生命周期与运行时字体一致的 LVGL 字体对象。 */
const lv_font_t* TtfRuntimeFont(const TtfRuntime* font);
/** @brief 获取仅供分页测宽的字体视图；步进缓存不可用时回退正文字体。 */
const lv_font_t* TtfRuntimeLayoutFont(const TtfRuntime* font);
/** @brief 批量预热当前页的码点，复用有容量上限的字形缓存；缺字不视为错误。 */
bool TtfRuntimePrewarm(TtfRuntime* font, const uint32_t* codepoints, size_t count);
