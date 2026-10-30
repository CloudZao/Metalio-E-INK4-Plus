#pragma once

#include <lvgl.h>
#include "reader/book_session.h"
struct ReaderPageResult;

/** @brief 从 LVGL 事件读取触点坐标 */
bool ReadEventPoint(lv_event_t* e, lv_point_t* out);
/** @brief 盖板键与长按提交翻页增量，后台任务准备目标页 */
bool QueueReaderPageTurn(int dir);
/** @brief 请求退回阅读应用首页 */
void RequestBackHome();
/** @brief 完成离开正文后的延迟清理 */
void FinishDeferredCleanup();
/** @brief 提交最终进度并将会话析构交给回收任务；队列满时保留会话供重试。 */
bool RetireReaderSession();
/** @brief 预留 SD 事务并后台销毁会话；成功后取得指针所有权。 */
bool ScheduleReaderSessionCleanup(reader::BookSession* session);
/** @brief 打开/排版/正文/插图 worker 是否忙碌 */
bool ReaderWorkersBusy();
/** @brief 阅读相关 worker 占线时打一条，便于串口对照「点了继续阅读却无 StartOpenWorker」 */
void WarnReaderWorkersBusy(const char* where);
/** @brief 与进百问同源：open_token 失效 worker 回调；opening 时 deferred_cleanup，避免 UAF/泄漏 */
void ReleaseReaderSessionForLeave();
/** @brief 系统回首页前清理仍存活的阅读屏（LVGL 任务） */
void ReleaseReaderForSystemHome();
/** @brief 从目录/设置等返回已有正文屏 */
lv_obj_t* ResumeReaderScreen();
/** @brief 从正文请求打开百问 */
void RequestOpenAssistantFromReader();
/** @brief 与 vk_home 同路：勿在 CLICKED 栈里同步拆 session */
bool BookLvAsync(void (*cb)(void*), void* user_data = nullptr);
/** @brief 注册书库相关异步刷屏入口 */
void EnsureBookPaintFns();
/** @brief 页码已同步改完后调用：序号合并，积压回调跳过已画过的序号。 */
void RequestRenderBookshelfPage(bool screen_change = false);
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

/** @brief 在 UI 提交导航；相对翻页累加，绝对跳转保留最新目标。 */
bool RequestReaderNavigation(reader::BookSession::NavigationKind kind, int value = 0);
/** @brief 在 UI 轮询页面结果，并调度前台导航与后台扫描。 */
void PollReaderPageWork();
/** @brief 是否存在待执行或正在准备的正文请求。 */
bool ReaderPageWorkPending();
/** @brief 取走已发布的正文结果，由正文绘制函数消费。 */
std::unique_ptr<ReaderPageResult> TakeReaderPageResult();
/** @brief 离开或换参时作废结果并清空积压输入。 */
void CancelReaderPageWork();
