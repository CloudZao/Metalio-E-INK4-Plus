#pragma once

#include <string>

#include "camera.h"
#include "usb_uvc_still.h"

/**
 * USB UVC 静拍 → Camera 抽象，供 MCP self.camera.take_photo / 百问 BOOT 双击。
 * Capture：OTG+MUX → 会话 → 抓 JPEG → 立刻安全释放 USB（JPEG 留内存）。
 * Explain：仅 vision HTTP（字段与 Esp32Camera 一致），不再触碰 Host/MUX。
 * CaptureStillJpeg：Capture 后移交 JPEG 所有权（调用方 free），供本地 A2UI 插图。
 */
class UvcStillCamera : public Camera {
public:
    UvcStillCamera() = default;
    ~UvcStillCamera() override;

    void SetExplainUrl(const std::string& url, const std::string& token) override;
    bool Capture() override;
    bool SetHMirror(bool enabled) override;
    bool SetVFlip(bool enabled) override;
    std::string Explain(const std::string& question) override;
    bool CaptureStillJpeg(uint8_t** out_data, size_t* out_len) override;

private:
    void ReleaseJpeg();
    void ReleaseHardware();

    std::string explain_url_;
    std::string explain_token_;
    UsbUvcStill::JpegBuffer jpeg_;
    bool mux_held_ = false;
    bool otg_held_ = false;
};
