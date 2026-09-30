#pragma once

#include <cstddef>
#include <string>

#include <lvgl.h>

/** @brief 预扫 SD 字库列表，打开设置卡时不必再等「加载中」 */
void ScanReadFonts();
/** @brief ---------- TTF 导入转换面板（覆盖设置卡；转换 worker 独立，UI 只轮询） ---------- */
void EnsureFontListPageShowsSelection();
/** @brief 当前选中字体在列表中的下标 */
int CurrentFontIndex();
/** @brief 字体列表总页数 */
int FontListPageCount();
/** @brief 钳制字体列表页码 */
void ClampFontListPage();
/** @brief 列表主文案：去掉 .ef；过长截断 */
void FormatFontListName(const std::string& file, char* out, size_t out_len);
/** @brief 退出字体多选模式 */
void ExitFontMultiMode(bool paint);
/** @brief 进入字体多选 */
void EnterFontMultiModeSelect(int idx);
/** @brief 刷新字体多选底栏 */
void RefreshFontMultiFooter();
/** @brief 同步已选字体集合容量 */
void SyncFontSelectedSize();
/** @brief 字体项是否已选 */
bool FontItemSelected(int idx);
/** @brief 已选字体数量 */
int FontSelectedCount();
/** @brief 切换字体项选中 */
void ToggleFontItemSelected(int idx);
/** @brief 字体多选取消 */
void OnFontMultiCancel(lv_event_t* e);
/** @brief 字体多选全选/反选 */
void OnFontMultiSelectAll(lv_event_t* e);
/** @brief 字体多选批量移除 */
void OnFontMultiRemove(lv_event_t* e);
/** @brief 设置卡字体项长按 */
void OnSheetFontLongPressed(lv_event_t* e);
/** @brief 设置卡点选字体 */
void OnSheetFontSelect(lv_event_t* e);
