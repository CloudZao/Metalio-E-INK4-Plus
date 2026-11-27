#pragma once
#include <atomic>
#include <lvgl.h>

struct TtfRuntime;

enum class TtfRuntimeUse {
    Reader, // 阅读正文，保留分页步进及大容量位图缓存
    SystemUi, // 系统界面，紧凑缓存且不长期占用 SD 文件句柄
};

/** @brief 打开按需渲染的 TTF/OTF 字体；失败返回空指针。 */
TtfRuntime* TtfRuntimeOpen(const char* path, int size_px, TtfRuntimeUse use = TtfRuntimeUse::Reader);

/** @brief 关闭字体文件并释放字形缓存和后台任务。 */
void TtfRuntimeClose(TtfRuntime* font);

/** @brief 获取生命周期与运行时字体一致的 LVGL 字体对象。 */
const lv_font_t* TtfRuntimeFont(const TtfRuntime* font);

/** @brief 查询字体是否使用完整且有容量上限的 PSRAM 快照。 */
bool TtfRuntimeIsMemoryBacked(const TtfRuntime* font);

/** @brief 在后台核对内存快照与当前 SD 文件，调用期间须持有字体所有权。 */
bool TtfRuntimeSnapshotMatchesFile(const TtfRuntime* font);

/** @brief 获取仅供分页测宽的字体视图；步进缓存不可用时回退正文字体。 */
const lv_font_t* TtfRuntimeLayoutFont(const TtfRuntime* font);

/** @brief 批量预热当前页的码点，复用有容量上限的字形缓存；缺字不视为错误。 */
bool TtfRuntimePrewarm(TtfRuntime* font, const uint32_t* codepoints, size_t count,
                       const std::atomic<bool>* cancelled = nullptr);
