#ifndef CAMERA_H
#define CAMERA_H

#include <cstddef>
#include <cstdint>

class Camera {
public:
    virtual ~Camera() = default;

    /** @brief 设置硬件水平镜像，不支持时返回false。 */
    virtual bool SetHMirror(bool enabled) = 0;

    /** @brief 设置硬件垂直翻转，不支持时返回false。 */
    virtual bool SetVFlip(bool enabled) = 0;

    /**
     * @brief 静拍JPEG并移交所有权，调用方使用free释放。
     * @note 默认不支持；实现须在返回前完成硬件会话清理，失败时清空输出。
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

#endif  // CAMERA_H
