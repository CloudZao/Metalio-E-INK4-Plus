#include "uvc_still_camera.h"

#include <stdexcept>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board.h"
#include "cx25601n.h"
#include "system_info.h"

#define TAG "UvcStillCamera"

namespace {
constexpr uint32_t kSessionEnumMs = 5000;
constexpr uint32_t kCaptureTimeoutMs = 20000;
constexpr size_t kMaxJpegBytes = 5 * 1024 * 1024;
constexpr uint32_t kOtgOffSettleMs = 50;
}  // namespace

UvcStillCamera::~UvcStillCamera() {
    ReleaseJpeg();
    ReleaseHardware();
}

void UvcStillCamera::SetExplainUrl(const std::string& url, const std::string& token) {
    explain_url_ = url;
    explain_token_ = token;
    ESP_LOGI(TAG, "Explain URL set (len=%u, token=%s)", (unsigned)explain_url_.size(),
             explain_token_.empty() ? "no" : "yes");
}

void UvcStillCamera::ReleaseJpeg() {
    jpeg_.Release();
}

void UvcStillCamera::ReleaseHardware() {
    // StopSession(=ParkSession) 内部已：关流 → MUX=flash → 等 disconnect → 卸 UVC → ALL_FREE
    if (UsbUvcStill::IsSessionActive()) {
        UsbUvcStill::StopSession();
        mux_held_ = false;
    } else if (mux_held_) {
        UsbUvcStill::SelectUsbMux(false);
        mux_held_ = false;
    }

    if (otg_held_) {
        vTaskDelay(pdMS_TO_TICKS(kOtgOffSettleMs));
        if (cx25601n_is_ready()) {
            const esp_err_t otg = cx25601n_enable_otg(false);
            if (otg != ESP_OK) {
                ESP_LOGW(TAG, "OTG disable failed: %s", esp_err_to_name(otg));
            }
        }
        otg_held_ = false;
    }
}

bool UvcStillCamera::SetHMirror(bool /*enabled*/) {
    return false;
}

bool UvcStillCamera::SetVFlip(bool /*enabled*/) {
    return false;
}

bool UvcStillCamera::Capture() {
    ReleaseJpeg();

    if (UsbUvcStill::IsBusy()) {
        ESP_LOGW(TAG, "Capture skipped: USB UVC busy");
        return false;
    }

    // 与自动测试一致：先 OTG 供电，再拉低 P0.0 切摄像头
    if (cx25601n_is_ready()) {
        const esp_err_t otg = cx25601n_enable_otg(true);
        if (otg != ESP_OK) {
            ESP_LOGE(TAG, "OTG enable failed: %s", esp_err_to_name(otg));
            return false;
        }
        otg_held_ = true;
    } else {
        ESP_LOGW(TAG, "CX25601N not ready, capture without OTG");
    }

    UsbUvcStill::SelectUsbMux(true);
    mux_held_ = true;

    if (!UsbUvcStill::IsSessionActive()) {
        const esp_err_t serr = UsbUvcStill::StartSession(kSessionEnumMs);
        if (serr != ESP_OK) {
            ESP_LOGE(TAG, "StartSession failed: %s", UsbUvcStill::ErrMessage(serr));
            ReleaseHardware();
            return false;
        }
    }

    const esp_err_t err = UsbUvcStill::CaptureOne(jpeg_, kCaptureTimeoutMs);
    if (err != ESP_OK || jpeg_.empty()) {
        ESP_LOGE(TAG, "CaptureOne failed: %s", UsbUvcStill::ErrMessage(err));
        ReleaseJpeg();
        ReleaseHardware();
        return false;
    }

    if (jpeg_.len > kMaxJpegBytes) {
        ESP_LOGE(TAG, "JPEG too large: %u bytes", (unsigned)jpeg_.len);
        ReleaseJpeg();
        ReleaseHardware();
        return false;
    }

    ESP_LOGI(TAG, "Captured JPEG %u bytes", (unsigned)jpeg_.len);
    // JPEG 已在内存：立刻按安全时序释放 USB，避免 Explain/HTTP 结束后再切 MUX 触发 hub abort
    ReleaseHardware();
    return true;
}

bool UvcStillCamera::CaptureStillJpeg(uint8_t** out_data, size_t* out_len) {
    if (out_data == nullptr || out_len == nullptr) {
        return false;
    }
    *out_data = nullptr;
    *out_len = 0;
    if (!Capture()) {
        return false;
    }
    if (jpeg_.empty()) {
        return false;
    }
    *out_data = jpeg_.data;
    *out_len = jpeg_.len;
    jpeg_.data = nullptr;
    jpeg_.len = 0;
    return true;
}

std::string UvcStillCamera::Explain(const std::string& question) {
    if (explain_url_.empty()) {
        ReleaseJpeg();
        ReleaseHardware();
        throw std::runtime_error("Image explain URL or token is not set");
    }
    if (jpeg_.empty()) {
        ReleaseHardware();
        throw std::runtime_error("No photo captured");
    }

    // Capture 成功路径已释放 USB；此处幂等兜底，保证 HTTP 全程不持有 UVC 会话
    ReleaseHardware();

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);
    const std::string boundary = "----ESP32_CAMERA_BOUNDARY";

    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!explain_token_.empty()) {
        http->SetHeader("Authorization", "Bearer " + explain_token_);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");

    ESP_LOGI(TAG, "MCP vision POST url=%s jpeg=%u question=%s", explain_url_.c_str(),
             (unsigned)jpeg_.len, question.c_str());

    if (!http->Open("POST", explain_url_)) {
        ESP_LOGE(TAG, "Failed to connect to explain URL: %s", explain_url_.c_str());
        ReleaseJpeg();
        throw std::runtime_error("Failed to connect to explain URL");
    }

    {
        std::string question_field;
        question_field += "--" + boundary + "\r\n";
        question_field += "Content-Disposition: form-data; name=\"question\"\r\n";
        question_field += "\r\n";
        question_field += question + "\r\n";
        http->Write(question_field.c_str(), question_field.size());
    }
    {
        std::string file_header;
        file_header += "--" + boundary + "\r\n";
        file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
        file_header += "Content-Type: image/jpeg\r\n";
        file_header += "\r\n";
        http->Write(file_header.c_str(), file_header.size());
    }

    http->Write(reinterpret_cast<const char*>(jpeg_.data), jpeg_.len);
    const size_t jpeg_len = jpeg_.len;
    ReleaseJpeg();

    {
        std::string multipart_footer;
        multipart_footer += "\r\n--" + boundary + "--\r\n";
        http->Write(multipart_footer.c_str(), multipart_footer.size());
    }
    http->Write("", 0);

    const int status = http->GetStatusCode();
    if (status != 200) {
        ESP_LOGE(TAG, "Failed to upload photo, status code: %d", status);
        http->Close();
        throw std::runtime_error("Failed to upload photo");
    }

    std::string result = http->ReadAll();
    http->Close();

    ESP_LOGI(TAG, "Explain done jpeg=%u question=%s\n%s", (unsigned)jpeg_len, question.c_str(),
             result.c_str());
    return result;
}
