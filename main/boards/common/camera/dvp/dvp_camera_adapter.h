#pragma once

#include "boards/common/camera/camera.h"

/** @brief 将DVP静拍接入公共摄像头硬件接口。 */
class DvpCamera : public Camera {
public:
    /** @brief 当前DVP实现不支持硬件水平镜像。 */
    bool SetHMirror(bool) override {
        return false;
    }

    /** @brief 当前DVP实现不支持硬件垂直翻转。 */
    bool SetVFlip(bool) override {
        return false;
    }

    /** @brief 采集JPEG并移交所有权，调用方使用free释放。 */
    bool CaptureStillJpeg(uint8_t** out_data, size_t* out_len) override;
};
