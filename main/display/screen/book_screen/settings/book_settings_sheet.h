#pragma once

#include <lvgl.h>

struct BookUiState;

/** @brief 漫画下提示文字设置已保存，返回是否应跳过即时应用 */
bool DeferComicTextSetting();

/** @brief 重建排版设置卡控件树 */
void RebuildSettingsSheet();
/** @brief 按当前内容及屏幕方向调整阅读卡片尺寸。 */
void ResizeReadingBottomSheet();
/** @brief 切换阅读卡片页签并关闭当前子面板。 */
void SelectReadingSheetTab(int tab);
/** @brief 取消书签多选，返回是否处理了取消操作。 */
bool CancelBookmarkMultiSelection();
/** @brief 清空书签多选状态。 */
void ResetBookmarkSelection();
/** @brief 书签提示弹窗是否打开。 */
bool BookmarkMessageVisible();
/** @brief 关闭书签提示及遮罩，返回是否关闭了弹窗；须在 UI 任务调用。 */
bool CloseBookmarkMessage();
/** @brief 设置卡控件已缓存时原地刷；不再因按下推迟整卡 clean（±/翻页不拆树） */
void ClearSettingsSheetWidgetRefs();
/** @brief 刷新设置卡内容 */
void RefreshSettingsSheet();
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

/** @brief 统一设置阅读卡片外框：顶部及两侧 2px、无底边，导航分隔线只绘制一次。 */
void ApplyReadingPanelRules(lv_obj_t* panel, int bottom_gap = 0);
/** @brief 底部四项导航占用的高度，触摸分区浮层需让出这块区域。 */
int ReadingSheetNavReserve();

/** @brief 原地更新阅读卡片；结构变化需要重建时返回 false。 */
bool RefreshReadingSheetInPlace();
/** @brief 提交未完成的数值调节并清理阅读卡片控件引用。 */
void ResetReadingSheetLiveRefs();

/** @brief 获取阅读卡片统一标题栏高度。 */
int ReadingSheetHeaderHeight();
/** @brief 创建字体、阅读设置与触摸分区共用的恢复默认按钮。 */
lv_obj_t* CreateReadingResetButton(lv_obj_t* header, lv_event_cb_t event);

/** @brief 构建共用的阅读卡片标题与返回按钮，统一位置和箭头尺寸。 */
void BuildReadingSheetHeader(lv_obj_t* header, const char* title, lv_event_cb_t back_event, bool back_arrow);
