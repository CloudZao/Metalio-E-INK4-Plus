#pragma once

#include <lvgl.h>
#include <atomic>

#include "reader/reader_types.h"

/** @brief 书库/详情等界面标题：固件 fontpack 30@2 */
const lv_font_t* Book_ListFont();
/** @brief 书库列表 item：固件 fontpack UI 默认字（30@2） */
const lv_font_t* Book_ItemFont();
/** @brief 获取正文 TTF/OTF 或 .ef 字体，加载失败时回退默认字体。 */
const lv_font_t* BookFont();
/** @brief 获取正文分页测宽视图，字宽与绘制字体一致，仅供布局计算。 */
const lv_font_t* BookLayoutFont();
/** @brief 按 NVS 字体与字号偏好加载正文字体。 */
void EnsureBookFont();
/** @brief 记录待应用的源字体与字号，由安全重载验证并接入。 */
bool PrepareBookTtfFont(const char* filename, int size_px);
/** @brief 在编排任务退出后重载字体；待应用字体失败时保留原字体和偏好。 */
bool ReloadBookFont();
enum class BookFontReloadResult {
    kPending, // 无已完成请求
    kApplied, // 新字体已接入，需更新当前页
    kFailed, // 加载失败，保留原字体
    kDiscarded, // 请求已过期或阅读已暂停
};
/** @brief 在旧阅读任务退出后后台打开字体，UI 不等待字体文件加载。 */
bool StartBookFontReload();
/** @brief 接收后台字体结果；apply 为 false 时丢弃结果并保留待应用选择。 */
BookFontReloadResult PollBookFontReload(bool apply);
/** @brief 查询字体加载请求是否尚未收尾，期间须保持原阅读资源存活。 */
bool BookFontReloadBusy();
/** @brief 新正文控件接入后释放被替换字体，避免旧标签仍引用已释放字形回调。 */
void ReleaseRetiredBookFont();
/** @brief 释放正文动态字体资源；待机时可保留尚未执行的字体选择。 */
void ReleaseBookFont(bool cancel_prepared = true);
/** @brief 预热当前页用到的字形 */
bool PrewarmPageFont(const reader::Page* page, const std::atomic<bool>* cancelled = nullptr);
