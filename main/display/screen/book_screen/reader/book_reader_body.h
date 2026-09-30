#pragma once

#include <cstdint>

#include <lvgl.h>

#include "reader/reader.h"
#include "ui_scale.h"

// 源值对齐 397 book_screen；经 UiSx/UiSy 同比例到 470
constexpr lv_coord_t kOpenBarW = UiSx(320);
constexpr lv_coord_t kOpenBarH = UiSy(28);
constexpr uint32_t kOpenWorkerStack = 16 * 1024;
constexpr uint32_t kPageImageWorkerStack = 16 * 1024;
constexpr uint32_t kListCoverWorkerStack = 16 * 1024;
// 阅读时长周期落盘：30–60s 中取 45s，掉电最多丢约半分钟
constexpr uint32_t kReadTimeCheckpointMs = 45000;

/** @brief 创建正文阅读页 */
lv_obj_t* CreateReaderScreen(const reader::BookInfo& info);
/** @brief 从目录/设置等返回已有正文屏 */
lv_obj_t* ResumeReaderScreen();
/** @brief 渲染当前正文页 */
void RenderReaderPage();
/** @brief 上屏经 ScreenPaintCoalesce：有 pending 则等交完，连点跳到最终页再画 */
void RequestRenderReaderPage();
/** @brief 挂上正文触控（九宫格） */
void AttachReadBodyInput(lv_obj_t* obj);
/** @brief 目录页不挂正文触摸（不退出/不翻页）；章行自带点击 */
void DetachReadBodyInput(lv_obj_t* obj);
/** @brief 正文内容区点击（九宫格） */
void OnReadContentClicked(lv_event_t* e);
/** @brief 与进百问同源：open_token 失效 worker 回调；opening 时 deferred_cleanup，避免 UAF/泄漏 */
void ReleaseReaderSessionForLeave();
/** @brief 完成离开正文后的延迟清理 */
void FinishDeferredCleanup();
/** @brief 打开/排版/插图 worker 是否忙碌 */
bool ReaderWorkersBusy();
/** @brief 阅读相关 worker 占线时打一条，便于串口对照「点了继续阅读却无 StartOpenWorker」 */
void WarnReaderWorkersBusy(const char* where);
