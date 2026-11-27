#pragma once
#include <memory>
#include "app_api.h"
#include "infra/image/raster_image.h"

struct AppCameraPreview;

/** @brief 创建前台摄像头取景状态，不立即申请硬件 */
std::shared_ptr<AppCameraPreview> AppCameraPreviewCreate();

/** @brief 设置取景需求和有界输出尺寸；重复开启不重建正在运行的任务 */
int AppCameraPreviewEnable(const std::shared_ptr<AppCameraPreview>& state, bool enabled, int width, int height);

/** @brief 同步前台状态，待机与离页取消采集，恢复前台后按需求重启 */
void AppCameraPreviewForeground(const std::shared_ptr<AppCameraPreview>& state, bool foreground);

/** @brief 在 UI 线程取走最新一帧，不累计历史帧 */
std::shared_ptr<RasterImage> AppCameraPreviewTake(const std::shared_ptr<AppCameraPreview>& state);

/** @brief 查询摄像头采集状态 */
int AppCameraPreviewStatus(const std::shared_ptr<AppCameraPreview>& state, xz_app_camera_status_t* result);

/** @brief 取消取景并释放后台持有的结果，不等待 UI 或 ELF 回调 */
void AppCameraPreviewClose(const std::shared_ptr<AppCameraPreview>& state);

/** @brief 查询是否仍有摄像头工作任务或硬件收尾，供待机与关机等待 */
bool AppCameraPreviewBusy();
