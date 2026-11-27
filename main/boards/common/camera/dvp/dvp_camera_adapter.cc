#include <esp_log.h>
#include "dvp_camera_adapter.h"
#include "dvp_camera.h"

bool DvpCamera::CaptureStillJpeg(uint8_t** out_data, size_t* out_len) {
    const esp_err_t err = DvpCameraCaptureJpeg(out_data, out_len, {20000});
    if (err != ESP_OK) {
        ESP_LOGW("DvpCamera", "capture failed: code=%d", static_cast<int>(err));
    }
    return err == ESP_OK;
}
