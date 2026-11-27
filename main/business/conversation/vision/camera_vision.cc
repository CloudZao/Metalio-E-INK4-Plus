#include <algorithm>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <esp_log.h>
#include <esp_timer.h>
#include "sysmgmt/power/power_lifecycle.h"
#include "business/conversation/conversation_presentation.h"
#include "camera_vision.h"
#include "boards/common/camera/camera.h"
#include "boards/common/board/board.h"
#include "infra/platform/system_info.h"

static const char* TAG = "CameraVision";
static constexpr const char* kBoundary = "----ESP32_CAMERA_BOUNDARY";

static void WriteAll(Http& http, const char* data, size_t length) {
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

void CameraVision::Configure(const std::string& url, const std::string& token) {
    // 两项均准备完成后再发布，分配失败时保留上一组完整配置。
    std::string next_url(url);
    std::string next_token(token);
    std::lock_guard<std::mutex> lock(mutex_);
    url_.swap(next_url);
    token_.swap(next_token);
}

std::string CameraVision::TakePhoto(Camera& camera, const std::string& question) {
    std::string url, token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url = url_;
        token = token_;
    }
    if (url.empty()) {
        return "Vision service is not configured / 未配置视觉服务";
    }
    uint8_t* data = nullptr;
    size_t length = 0;
    const bool captured = camera.CaptureStillJpeg(&data, &length);
    std::unique_ptr<uint8_t, decltype(&std::free)> jpeg(data, std::free);
    if (!captured || !jpeg || length == 0) {
        return "Camera capture unavailable: retry / 无法拍摄，请重试";
    }
    const char* stage = "network";
    int status = 0;
    try {
        auto& board = Board::GetInstance();
        auto network = board.GetNetwork();
        auto http = network ? network->CreateHttp(3) : nullptr;
        if (!http) {
            throw std::runtime_error("Network is unavailable / 网络不可用，请重试");
        }
        stage = "connect";
        http->SetTimeout(30000);
        http->SetRequestDeadline(esp_timer_get_time() + 30000000LL);
        http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
        http->SetHeader("Client-Id", board.GetUuid());
        if (!token.empty()) {
            http->SetHeader("Authorization", "Bearer " + token);
        }
        http->SetHeader("Content-Type", std::string("multipart/form-data; boundary=") + kBoundary);
        http->SetHeader("Transfer-Encoding", "chunked");
        if (!http->Open("POST", url)) {
            throw std::runtime_error("Cannot connect to vision service / 无法连接视觉服务");
        }
        const std::string header = std::string("--") + kBoundary +
                                   "\r\n"
                                   "Content-Disposition: form-data; name=\"question\"\r\n\r\n" +
                                   question + "\r\n--" + kBoundary +
                                   "\r\n"
                                   "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n"
                                   "Content-Type: image/jpeg\r\n\r\n";
        stage = "upload";
        WriteAll(*http, header.data(), header.size());
        WriteAll(*http, reinterpret_cast<const char*>(jpeg.get()), length);
        jpeg.reset();
        const std::string footer = std::string("\r\n--") + kBoundary + "--\r\n";
        WriteAll(*http, footer.data(), footer.size());
        if (http->Write("", 0) < 0) {
            throw std::runtime_error("Photo upload failed / 照片上传失败，请重试");
        }
        status = http->GetStatusCode();
        if (status != 200) {
            throw std::runtime_error("Vision service HTTP error / 视觉服务 HTTP 错误: " + std::to_string(status));
        }
        // 持续消费接收队列，避免较长识别结果触发 HTTP 接收背压。
        stage = "response";
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
    } catch (...) {
        ESP_LOGE(TAG, "request failed: stage=%s status=%d", stage, status);
        throw std::runtime_error("Vision request failed / 视觉请求失败，请重试");
    }
}
