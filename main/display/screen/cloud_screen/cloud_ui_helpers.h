#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <lvgl.h>

#include "cloud_screen/push/push_resources_library.h"

/** @brief 云页标题字体 */
const lv_font_t* Cloud_UiFont();
/** @brief 云页列表项字体 */
const lv_font_t* Cloud_ItemFont();
/** @brief 云页内容宽度 */
lv_coord_t Cloud_ContentWidth();
/** @brief 禁用滚动 */
void Cloud_DisableScroll(lv_obj_t* obj);
/** @brief 测量文本宽度 */
lv_coord_t Cloud_MeasureTextWidth(const lv_font_t* font, const char* text);
/** @brief 单行省略书名/文件名，避免 WRAP 顶掉底部按钮 */
std::string EllipsizeText(const std::string& text, const lv_font_t* font, lv_coord_t max_w);
/** @brief 按像素逐字排满最多两行（末行可省略）。 绕开 LVGL 在 LONG_WRAP 时「非行首英文整词不拆」导致的首行留白。 */
std::string Cloud_LayoutTitleTwoLines(const char* text, const lv_font_t* font, lv_coord_t max_w);
/** @brief 云页居中提示 */
void Cloud_ShowMessage(lv_obj_t* parent, const char* msg);
/** @brief 取消状态提示清除定时器 */
void CancelStatusClearTimer();
/** @brief 把状态提示应用到 UI */
void ApplyStatusTipUi();
/** @brief 设置状态提示文案 */
void SetStatusTip(const char* text, bool auto_clear);
/** @brief 资源是否属于当前 Tab */
bool ItemMatchesTab(const reader::CloudPushResource& item, int tab);
/** @brief 按 Tab 重建过滤列表 */
void RebuildFiltered();
/** @brief 刷新分区标题 */
void RefreshSectionTitle();
/** @brief Tab 按钮样式 */
void StyleTabBtn(lv_obj_t* btn, lv_obj_t* lbl, bool on);
/** @brief 刷新 Tab 选中态 */
void RefreshTabUi();
/** @brief 同步多选集合容量 */
void SyncSelectedSize();
/** @brief 已选数量 */
int SelectedCount();
/** @brief 项是否已选 */
bool ItemSelected(int idx);
/** @brief 切换项选中 */
void ToggleItemSelected(int idx);
/** @brief 刷新底栏模式 */
void RefreshFooterMode();
/** @brief 退出多选（扩展参数） */
void ExitMultiModeEx(bool rebuild, bool clear_status);
/** @brief 退出多选 */
void ExitMultiMode(bool rebuild);
/** @brief 进入多选 */
void EnterMultiModeSelect(int idx);
/** @brief 填充预览元信息 */
void FillPreviewMeta(const reader::CloudPushResource* item);
/** @brief 云页 UI 是否仍存活 */
bool ScreenAlive();
/** @brief 当前操作代数是否仍有效 */
bool OpStillValid(uint32_t op_gen);
/** @brief 同步是否忙碌 */
bool SyncBusy();
/** @brief 释放 UI 但保留等网任务 */
void ReleaseUiKeepNet();
/** @brief 把 URL 下载到内存缓冲 */
bool Cloud_DownloadUrlToBuffer(const char* url, std::vector<uint8_t>& buf, size_t max_bytes);
/** @brief 列表总页数 */
int PageCount();
/** @brief 钳制页码 */
void ClampPage();
/** @brief 设置页脚文案 */
void SetPageFooterText(const char* text);
