#pragma once

#include <lvgl.h>

struct BookUiState;

/** @brief 重建排版设置卡控件树 */
void RebuildSettingsSheet();
/** @brief 设置卡控件已缓存时原地刷；不再因按下推迟整卡 clean（±/翻页不拆树） */
void ClearSettingsSheetWidgetRefs();
/** @brief 刷新设置卡内容 */
void RefreshSettingsSheet();
/** @brief 刷新设置卡字体列表 */
void RefreshSettingsSheetFontList();
/** @brief 刷新设置卡边距/行距等调节行 */
void RefreshSettingsSheetAdj();
/** @brief 排版后保卡可见；控件已由 coalesce/Rebuild 就绪则不再叠刷 */
void EnsureSettingsSheetAfterLayout();
/** @brief 设置卡关键控件是否已建好 */
bool SettingsSheetWidgetsReady();
/** @brief 填充设置卡字体区 */
void SettingsSheetPopulateFont(BookUiState& st, lv_obj_t* parent);
/** @brief 填充设置卡选项区 */
void SettingsSheetPopulateOptions(BookUiState& st, lv_obj_t* parent);
/** @brief 创建设置卡图标按钮 */
lv_obj_t* MakeSheetIconBtn(lv_obj_t* parent, const char* txt, lv_event_cb_t cb, bool enabled);
/** @brief 创建设置卡文字链 */
lv_obj_t* MakeSheetTextLink(lv_obj_t* parent, const char* text, lv_event_cb_t cb);
/** @brief 设置卡按钮使能 */
void SetSheetBtnEnabled(lv_obj_t* btn, bool enabled);
/** @brief 更新设置卡绑定提示 */
void UpdateSheetBoundTip(lv_obj_t* tip, bool can_dec, bool can_inc);

/** @brief 设置卡字体上一页 */
void OnSheetFontPagePrev(lv_event_t* e);
/** @brief 设置卡字体下一页 */
void OnSheetFontPageNext(lv_event_t* e);
/** @brief 减小边距档 */
void OnSheetMarginDec(lv_event_t* e);
/** @brief 增大边距档 */
void OnSheetMarginInc(lv_event_t* e);
/** @brief 减小行距档 */
void OnSheetGapDec(lv_event_t* e);
/** @brief 增大行距档 */
void OnSheetGapInc(lv_event_t* e);
/** @brief 切换底栏信息位 */
void OnSheetFooterBitToggle(lv_event_t* e);
/** @brief 切换下划线模式 */
void OnSheetUnderlineMode(lv_event_t* e);
/** @brief 切换前光色温 */
void OnSheetFlCct(lv_event_t* e);
/** @brief 前光亮度 − */
void OnSheetFlBrightDec(lv_event_t* e);
/** @brief 前光亮度 + */
void OnSheetFlBrightInc(lv_event_t* e);
/** @brief 切换正文方向 */
void OnSheetOrientMode(lv_event_t* e);
/** @brief 设置卡「目录」 */
void OnSheetTocClicked(lv_event_t* e);
