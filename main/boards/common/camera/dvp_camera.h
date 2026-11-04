#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <esp_err.h>

// DVP 采集入口；产测预览和 Camera 适配共用硬件互斥与电源生命周期。
namespace dvp_camera {
struct Request {
    uint32_t timeout_ms = 8000; // 采集超时上限
    bool (*cancelled)(void*) = nullptr; // 可选取消回调
    void* context = nullptr; // 取消回调上下文
};
struct DetectInfo {
    uint16_t pid = 0; // 传感器型号 ID
};
struct GrayImage {
    uint16_t width = 0; // 图像宽度
    uint16_t height = 0; // 图像高度
    std::vector<uint8_t> pixels; // 灰度像素
};
/** @brief 检测摄像头型号 */
esp_err_t Detect(DetectInfo& info, const Request& request);
/** @brief 采集原生 VGA 分辨率的灰度预览 */
esp_err_t Capture(GrayImage& image, const Request& request);
/**
 * @brief 采集并编码彩色 JPEG，返回前释放摄像头硬件
 * @param out_data 接收图像所有权，调用方使用 free 释放
 * @param out_len 接收 JPEG 字节数，失败时为零
 */
esp_err_t CaptureJpeg(uint8_t** out_data, size_t* out_len, const Request& request);
}
