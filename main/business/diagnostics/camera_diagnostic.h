#pragma once
#include <atomic>
#include <memory>
#include <esp_err.h>
#include "infra/image/raster_image.h"

struct CameraDiagnosticResult {
    std::atomic<bool> cancelled{false};
    std::atomic<bool> completed{false};
    bool capture = false;
    esp_err_t error = ESP_FAIL;
    RasterImage raster;
};
using CameraDiagnosticRequest = std::shared_ptr<CameraDiagnosticResult>;

/** @brief 提交相机检测或采集，返回前释放硬件，忙碌或内存不足时未接纳。 */
CameraDiagnosticRequest CameraDiagnosticStart(bool capture, int max_width, int max_height);

/** @brief 请求取消采集，后台持有硬件会话直到安全退出。 */
void CameraDiagnosticCancel(const CameraDiagnosticRequest& request);
