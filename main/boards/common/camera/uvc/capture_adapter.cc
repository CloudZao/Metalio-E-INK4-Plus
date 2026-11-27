#include <utility>
#include "boards/common/camera/uvc/usb_uvc_still.h"
#include "infra/image/image_decode.h"
#include "boards/common/camera/camera_capture.h"

static esp_err_t DecodePreviewFrame(const uint8_t* bytes, size_t size, const CameraCaptureRequest& request,
                                    RasterImage& image) {
    ImageDecodeError error = ImageDecodeError::kNone;
    if (DecodeImageToL8(bytes, size, request.max_width, request.max_height, image,
                        request.cancelled, request.context, &error)) {
        return ESP_OK;
    }
    switch (error) {
        case ImageDecodeError::kNoMemory:
            return ESP_ERR_NO_MEM;
        case ImageDecodeError::kUnsupported:
            return ESP_ERR_NOT_SUPPORTED;
        case ImageDecodeError::kCancelled:
            return ESP_ERR_INVALID_STATE;
        case ImageDecodeError::kInvalidArgument:
            return ESP_ERR_INVALID_ARG;
        case ImageDecodeError::kInvalidData:
            return ESP_ERR_INVALID_RESPONSE;
        default:
            return ESP_FAIL;
    }
}

esp_err_t CameraDetect(const CameraCaptureRequest& request) {
    const UsbUvcStill::Request cancel{request.cancelled, request.context};
    return UsbUvcStill::CaptureManaged(nullptr, request.timeout_ms, &cancel);
}

esp_err_t CameraCapturePreview(RasterImage& image, const CameraCaptureRequest& request) {
    const UsbUvcStill::Request cancel{request.cancelled, request.context};
    UsbUvcStill::JpegBuffer jpeg;
    struct JpegRelease {
        UsbUvcStill::JpegBuffer& jpeg;
        ~JpegRelease() { jpeg.Release(); }
    } release{jpeg};
    auto result = UsbUvcStill::CaptureManaged(&jpeg, request.timeout_ms, &cancel);
    if (result == ESP_OK) {
        result = DecodePreviewFrame(jpeg.data, jpeg.len, request, image);
    }
    return result;
}

esp_err_t CameraStreamPreview(const CameraCaptureRequest& request, CameraPreviewFrameHandler handler, void* context) {
    if (!handler) {
        return ESP_ERR_INVALID_ARG;
    }
    const UsbUvcStill::Request cancel{request.cancelled, request.context};
    UsbUvcStill::JpegBuffer jpeg;
    struct BufferRelease {
        UsbUvcStill::JpegBuffer& jpeg;
        ~BufferRelease() { jpeg.Release(); }
    } release{jpeg};
    struct Delivery {
        const CameraCaptureRequest& request;
        CameraPreviewFrameHandler handler;
        void* context;
        esp_err_t result = ESP_OK;
    } delivery{request, handler, context};
    const auto result = UsbUvcStill::CaptureManaged(
        &jpeg, request.timeout_ms, &cancel, 8000,
        [](const uint8_t* bytes, size_t size, void* arg) {
            auto& state = *static_cast<Delivery*>(arg);
            RasterImage image;
            state.result = DecodePreviewFrame(bytes, size, state.request, image);
            if (state.result != ESP_OK) {
                return false;
            }
            return state.handler(std::move(image), state.context);
        }, &delivery);
    return result == ESP_OK ? delivery.result : result;
}
