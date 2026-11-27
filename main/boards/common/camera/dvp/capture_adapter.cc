#include <utility>
#include "boards/common/camera/dvp/dvp_camera.h"
#include "boards/common/camera/camera_capture.h"

esp_err_t CameraDetect(const CameraCaptureRequest& request) {
    DvpCameraDetectInfo info;
    return DvpCameraDetect(info, {request.timeout_ms, request.cancelled, request.context});
}

esp_err_t CameraCapturePreview(RasterImage& image, const CameraCaptureRequest& request) {
    DvpCameraGrayImage gray;
    const auto result = DvpCameraCapture(gray, {request.timeout_ms, request.cancelled, request.context});
    if (result == ESP_OK) {
        image.width = gray.width;
        image.height = gray.height;
        image.pixels = std::move(gray.pixels);
    }
    return result;
}

esp_err_t CameraStreamPreview(const CameraCaptureRequest& request, CameraPreviewFrameHandler handler, void* context) {
    return DvpCameraStreamPreview(request, handler, context);
}
