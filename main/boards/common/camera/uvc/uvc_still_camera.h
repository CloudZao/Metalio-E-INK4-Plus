#pragma once

#include <mutex>
#include "boards/common/camera/camera.h"
#include "usb_uvc_still.h"

/** @brief USB UVC静拍硬件，负责OTG、MUX和会话回收，不保存视觉服务配置。 */
class UvcStillCamera : public Camera {
public:
    UvcStillCamera() = default;
    ~UvcStillCamera() override;

    /** @brief 当前UVC实现不支持硬件水平镜像。 */
    bool SetHMirror(bool enabled) override;

    /** @brief 当前UVC实现不支持硬件垂直翻转。 */
    bool SetVFlip(bool enabled) override;

    /** @brief 完成会话清理后移交JPEG，调用方使用free释放。 */
    bool CaptureStillJpeg(uint8_t** out_data, size_t* out_len) override;

private:
    bool CaptureFrame();

    std::mutex capture_mutex_;  // 保护完整采集及照片所有权移交
    UsbUvcStill::JpegBuffer jpeg_;
};
