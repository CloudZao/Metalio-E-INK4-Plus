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

/** @brief 创建设置卡图标按钮 */
lv_obj_t* MakeSheetIconBtn(lv_obj_t* parent, const char* txt, lv_event_cb_t cb, bool enabled);

/** @brief 创建设置卡文字链 */
lv_obj_t* MakeSheetTextLink(lv_obj_t* parent, const char* text, lv_event_cb_t cb);

/** @brief 设置卡按钮使能 */
void SetSheetBtnEnabled(lv_obj_t* btn, bool enabled);

/** @brief 切换底栏信息位 */
void OnSheetFooterBitToggle(lv_event_t* e);

/** @brief 切换下划线模式 */
void OnSheetUnderlineMode(lv_event_t* e);

/** @brief 切换正文方向 */
void OnSheetOrientMode(lv_event_t* e);

/** @brief 底部四项导航占用的高度。 */
int ReadingSheetNavReserve();

/** @brief 原地更新阅读卡片；结构变化需要重建时返回 false。 */
bool RefreshReadingSheetInPlace();

/** @brief 提交未完成的数值调节并清理阅读卡片控件引用。 */
void ResetReadingSheetLiveRefs();

/** @brief 获取阅读卡片统一标题栏高度。 */
int ReadingSheetHeaderHeight();

/** @brief 构建阅读卡片标题、返回及可选恢复按钮，统一分配可用宽度。 */
void BuildReadingSheetHeader(lv_obj_t* header, const char* title, lv_event_cb_t back_event, bool back_arrow,
                             lv_event_cb_t reset_event);
