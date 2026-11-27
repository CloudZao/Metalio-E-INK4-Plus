#include <esp_log.h>
#include "uvc_still_camera.h"

#define TAG "UvcStillCamera"

static constexpr uint32_t kSessionEnumMs = 5000;
static constexpr uint32_t kCaptureTimeoutMs = 20000;
static constexpr size_t kMaxJpegBytes = 5 * 1024 * 1024;

UvcStillCamera::~UvcStillCamera() {
    jpeg_.Release();
}

bool UvcStillCamera::SetHMirror(bool /*enabled*/) {
    return false;
}

bool UvcStillCamera::SetVFlip(bool /*enabled*/) {
    return false;
}

bool UvcStillCamera::CaptureStillJpeg(uint8_t** out_data, size_t* out_len) {
    if (out_data == nullptr || out_len == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(capture_mutex_);
    *out_data = nullptr;
    *out_len = 0;
    if (!CaptureFrame()) {
        return false;
    }
    *out_data = jpeg_.data;
    *out_len = jpeg_.len;
    jpeg_.data = nullptr;
    jpeg_.len = 0;
    return true;
}

bool UvcStillCamera::CaptureFrame() {
    jpeg_.Release();
    const esp_err_t result = UsbUvcStill::CaptureManaged(&jpeg_, kCaptureTimeoutMs, nullptr, kSessionEnumMs);
    if (result != ESP_OK || jpeg_.empty() || jpeg_.len > kMaxJpegBytes) {
        if (result == ESP_OK) {
            ESP_LOGE(TAG, "operation=capture result=invalid_size bytes=%u", unsigned(jpeg_.len));
        }
        jpeg_.Release();
        return false;
    }
    ESP_LOGI(TAG, "Captured JPEG bytes=%u", unsigned(jpeg_.len));
    return true;
}
