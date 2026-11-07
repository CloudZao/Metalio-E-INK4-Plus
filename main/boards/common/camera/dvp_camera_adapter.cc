#include "dvp_camera_adapter.h"

#include "board.h"
#include "dvp_camera.h"
#include "system_info.h"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <esp_log.h>
#include <esp_timer.h>

namespace {
constexpr const char* kTag = "DvpCameraMcp";
constexpr const char* kBoundary = "----ESP32_CAMERA_BOUNDARY";

void WriteAll(Http& http, const char* data, size_t length) {
    while (length > 0) {
        const size_t chunk = std::min(length, size_t(4096));
        const int written = http.Write(data, chunk);
        if (written <= 0 || size_t(written) > chunk) {
            throw std::runtime_error("Photo upload failed / 照片上传失败，请重试");
        }
        data += written;
        length -= size_t(written);
    }
}
}

void DvpCamera::SetExplainUrl(const std::string& url, const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    explain_url_ = url;
    explain_token_ = token;
}

bool DvpCamera::CaptureStillJpeg(uint8_t** out_data, size_t* out_len) {
    const esp_err_t err = dvp_camera::CaptureJpeg(out_data, out_len, {20000});
    if (err != ESP_OK) ESP_LOGW(kTag, "Capture failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

bool DvpCamera::Capture() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jpeg_.reset();
        jpeg_len_ = 0;
        if (explain_url_.empty()) {
            ESP_LOGW(kTag, "Capture skipped: vision service URL is not configured");
            return false;
        }
    }
    uint8_t* data = nullptr;
    size_t length = 0;
    if (!CaptureStillJpeg(&data, &length)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    jpeg_.reset(data);
    jpeg_len_ = length;
    return true;
}

std::string DvpCamera::Explain(const std::string& question) {
    std::unique_ptr<uint8_t, decltype(&std::free)> jpeg(nullptr, std::free);
    size_t length;
    std::string url, token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jpeg = std::move(jpeg_);
        length = std::exchange(jpeg_len_, 0);
        url = explain_url_;
        token = explain_token_;
    }
    if (url.empty()) {
        ESP_LOGW(kTag, "Explain skipped: vision service URL is not configured");
        return "Vision service is not configured / 未配置视觉服务";
    }
    if (!jpeg) throw std::runtime_error("No photo captured / 尚未拍摄照片");
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network ? network->CreateHttp(3) : nullptr;
    if (!http) throw std::runtime_error("Network is unavailable / 网络不可用，请重试");
    http->SetTimeout(30000);
    http->SetRequestDeadline(esp_timer_get_time() + 30000000LL);
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
    http->SetHeader("Client-Id", board.GetUuid());
    if (!token.empty()) http->SetHeader("Authorization", "Bearer " + token);
    http->SetHeader("Content-Type", std::string("multipart/form-data; boundary=") + kBoundary);
    http->SetHeader("Transfer-Encoding", "chunked");
    if (!http->Open("POST", url)) {
        throw std::runtime_error("Cannot connect to vision service / 无法连接视觉服务");
    }
    const std::string header = std::string("--") + kBoundary + "\r\n"
        "Content-Disposition: form-data; name=\"question\"\r\n\r\n" + question + "\r\n--" + kBoundary + "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n";
    WriteAll(*http, header.data(), header.size());
    WriteAll(*http, reinterpret_cast<const char*>(jpeg.get()), length);
    jpeg.reset();
    const std::string footer = std::string("\r\n--") + kBoundary + "--\r\n";
    WriteAll(*http, footer.data(), footer.size());
    if (http->Write("", 0) < 0) throw std::runtime_error("Photo upload failed / 照片上传失败，请重试");
    const int status = http->GetStatusCode();
    if (status != 200) {
        throw std::runtime_error("Vision service HTTP error / 视觉服务 HTTP 错误: " + std::to_string(status));
    }
    // 持续消费接收队列，避免较长识别结果触发 HTTP 接收背压。
    std::string result;
    char buffer[1024];
    int received;
    while ((received = http->Read(buffer, sizeof(buffer))) > 0) {
        if (result.size() + size_t(received) > 64 * 1024) {
            throw std::runtime_error("Vision response is too large / 视觉识别结果过长");
        }
        result.append(buffer, size_t(received));
    }
    const size_t expected = http->GetBodyLength();
    if (received < 0 || result.empty() || (expected && expected != result.size())) {
        throw std::runtime_error("Vision response failed / 未能获取视觉识别结果，请重试");
    }
    http->Close();
    return result;
}
