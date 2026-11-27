#pragma once

#include <cstddef>
#include <cstdint>
#include <esp_err.h>
#include "infra/image/raster_image.h"

struct CameraCaptureRequest {
    int max_width = 0, max_height = 0; // 可选的预览解码边界，由呈现入口提供
    uint32_t timeout_ms = 8000;          // 本次检测或采集的等待上限
    bool (*cancelled)(void*) = nullptr;  // 查询操作是否作废，仅后台任务调用
    void* context = nullptr;             // 取消回调上下文，由调用方保持有效
};

/** @brief 在后台探测摄像头，不发布UI结果 */
esp_err_t CameraDetect(const CameraCaptureRequest& request);

/** @brief 在后台采集预览，返回前释放硬件会话，图像归调用方所有 */
esp_err_t CameraCapturePreview(RasterImage& image, const CameraCaptureRequest& request);

typedef bool (*CameraJpegFrameHandler)(const uint8_t* jpeg, size_t length, void* context); // 借用单帧；返回 false 正常停止

typedef bool (*CameraPreviewFrameHandler)(RasterImage&& image, void* context); // 移交单帧灰度图；返回 false 正常停止

/** @brief 在后台复用采集会话逐帧交付灰度图，回调期间不持有显示 DMA 门闩 */
esp_err_t CameraStreamPreview(const CameraCaptureRequest& request, CameraPreviewFrameHandler handler, void* context);
