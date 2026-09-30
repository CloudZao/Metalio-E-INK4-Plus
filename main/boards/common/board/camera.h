#ifndef CAMERA_H
#define CAMERA_H

#include <cstddef>
#include <cstdint>
#include <string>

class Camera {
public:
    virtual ~Camera() = default;
    virtual void SetExplainUrl(const std::string& url, const std::string& token) = 0;
    virtual bool Capture() = 0;
    virtual bool SetHMirror(bool enabled) = 0;
    virtual bool SetVFlip(bool enabled) = 0;
    virtual std::string Explain(const std::string& question) = 0;

    /**
     * 静拍一帧并把 JPEG 所有权交给调用方（*out_data 须 free）。
     * 默认不支持；UVC 板实现：内部走 Capture 的 OTG/MUX 生命周期。
     */
    virtual bool CaptureStillJpeg(uint8_t** out_data, size_t* out_len) {
        if (out_data) {
            *out_data = nullptr;
        }
        if (out_len) {
            *out_len = 0;
        }
        return false;
    }
};

#endif // CAMERA_H
