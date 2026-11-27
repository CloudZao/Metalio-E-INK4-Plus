#pragma once
#include "business/diagnostics/log_upload_session.h"

/** @brief 启动日志上传，已有任务运行时返回该任务；不访问界面 */
LogUploadSessionPtr LogUploadStart();

/** @brief 请求取消上传，不阻塞、不销毁在途网络对象 */
void LogUploadCancel();
