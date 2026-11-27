#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <esp_err.h>
#include "boards/common/camera/camera_capture.h"

// DVP 采集入口；产测预览和 Camera 适配共用硬件互斥与电源生命周期。

struct DvpCameraRequest {
    uint32_t timeout_ms = 8000;          // 采集超时上限
    bool (*cancelled)(void*) = nullptr;  // 可选取消回调
    void* context = nullptr;             // 取消回调上下文
    bool keep_awake = true; // 静拍保持活动，连续取景允许系统自动待机
};

struct DvpCameraDetectInfo {
    uint16_t pid = 0;  // 传感器型号 ID
};

struct DvpCameraGrayImage {
    uint16_t width = 0;           // 图像宽度
    uint16_t height = 0;          // 图像高度
    std::vector<uint8_t> pixels;  // 灰度像素
};

/** @brief 检测摄像头型号 */
esp_err_t DvpCameraDetect(DvpCameraDetectInfo& info, const DvpCameraRequest& request);

/** @brief 采集原生 VGA 分辨率的灰度预览 */
esp_err_t DvpCameraCapture(DvpCameraGrayImage& image, const DvpCameraRequest& request);

/**
 * @brief 采集并编码彩色 JPEG，返回前释放摄像头硬件
 * @param out_data 接收图像所有权，调用方使用 free 释放
 * @param out_len 接收 JPEG 字节数，失败时为零
 */
esp_err_t DvpCameraCaptureJpeg(uint8_t** out_data, size_t* out_len, const DvpCameraRequest& request);

/** @brief 复用摄像头逐帧采集灰度预览，在帧间归还显示 DMA 门闩 */
esp_err_t DvpCameraStreamPreview(const CameraCaptureRequest& request, CameraPreviewFrameHandler handler, void* context);
