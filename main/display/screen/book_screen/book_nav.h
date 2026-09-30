#pragma once

#include <lvgl.h>

/** @brief 从 LVGL 事件读取触点坐标 */
bool ReadEventPoint(lv_event_t* e, lv_point_t* out);
/** @brief 应用已排队的正文翻页增量 */
void ApplyReaderPageDelta();
/** @brief 盖板键/长按定时器：只累加 delta，真正翻页在 RenderReaderPage（LVGL） */
bool QueueReaderPageTurn(int dir);
/** @brief 请求退回阅读应用首页 */
void RequestBackHome();
/** @brief 完成离开正文后的延迟清理 */
void FinishDeferredCleanup();
/** @brief 打开/排版/插图 worker 是否忙碌 */
bool ReaderWorkersBusy();
/** @brief 阅读相关 worker 占线时打一条，便于串口对照「点了继续阅读却无 StartOpenWorker」 */
void WarnReaderWorkersBusy(const char* where);
/** @brief 与进百问同源：open_token 失效 worker 回调；opening 时 deferred_cleanup，避免 UAF/泄漏 */
void ReleaseReaderSessionForLeave();
/** @brief 从目录/设置等返回已有正文屏 */
lv_obj_t* ResumeReaderScreen();
/** @brief 从正文请求打开百问 */
void RequestOpenAssistantFromReader();
/** @brief 与 vk_home 同路：勿在 CLICKED 栈里同步拆 session */
bool BookLvAsync(void (*cb)(void*), void* user_data = nullptr);
/** @brief 注册书库相关异步刷屏入口 */
void EnsureBookPaintFns();
/** @brief 页码已同步改完后调用：序号合并，积压回调跳过已画过的序号。 */
void RequestRenderBookshelfPage();
/** @brief 上屏经 ScreenPaintCoalesce：有 pending 则等交完，连点跳到最终页再画 */
void RequestRenderReaderPage();
/** @brief 请求重绘目录列表 */
void RequestRenderTocList();
/** @brief 与 ▲▼/± 同路：合并上屏，勿同步再刷一帧 */
void RequestSettingsSheetPaint();
/** @brief 目录页不挂正文触摸（不退出/不翻页）；章行自带点击 */
void DetachReadBodyInput(lv_obj_t* obj);
/** @brief 挂上正文触控（九宫格） */
void AttachReadBodyInput(lv_obj_t* obj);
/** @brief 异步退回系统首页 */
void GoHomeFromBookAsync(void* user_data);
/** @brief 异步打开百问（LVGL 任务） */
void OpenAssistantFromReaderAsync(void* user_data);
/** @brief 清空详情封面控件指针 */
void ClearDetailCoverUiPtrs();
/** @brief 详情 vk_home / vk_prev → 书架或阅读首页。 */
void CancelDetailCoverLoad();
/** @brief 作废列表/目录封面补全；须在清槽/离页前调用。 */
void CancelListCoverFill();
/** @brief 作废正文插图后台解码；翻页 clean 前调用。 */
void CancelPageImageLoad();
