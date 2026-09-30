#pragma once

#include <vector>

#include "cloud_screen/push/push_resources_library.h"

struct CloudDownloadWork;

/** @brief 排空下载队列 */
void DrainDlQueue();
/** @brief 停止下载 worker */
void StopDlWorker();
/** @brief 入队下载任务 */
bool EnqueueDlWork(CloudDownloadWork* work);
/** @brief 确保下载进度页已构建 */
void EnsureDlProgressPageBuilt();
/** @brief 刷新下载进度页 */
void RefreshDlProgressPage(int file_percent);
/** @brief 显示下载进度页 */
void ShowDlProgressPage();
/** @brief 隐藏下载进度页 */
void HideDlProgressPage();
/** @brief 从 UI 中止当前下载作业 */
void AbortDownloadJobFromUi();
/** @brief 行内显示下载进度 */
void ShowRowDownloadProgress(int percent);
/** @brief 尽快继续批量下载 */
void ScheduleBatchContinueSoon(const char* why);
/** @brief 是否有活动下载任务 */
bool CloudDlTasksActive();
/** @brief 预览等待下载提示 */
void ShowPreviewWaitDlTip();
/** @brief 与下载相同：打断封面 HTTP，避免删行时仍回写悬空缩略图 */
void AbortCloudSideHttpForDownload();
/** @brief 下载结束后恢复云侧 HTTP */
void ResumeCloudSideHttpAfterDl();
/** @brief 下载进度回调 */
void OnDownloadProgress(int percent, void* user);
/** @brief 云下载 worker 入口 */
void CloudDownloadWorker(void* arg);
/** @brief 开始单项资源下载 */
void StartResourceDownload(int index);
/** @brief 下载完成异步回调入口 */
void AsyncDownloadDone(void* user_data);
/** @brief 若批量保存未完则继续 */
void ContinueBatchSaveIfNeeded();
/** @brief 按 taskId 找列表下标 */
int FindItemIndexByTaskId(const char* key);
/** @brief 删除列表指定下标项 */
void EraseItemAt(int idx);
