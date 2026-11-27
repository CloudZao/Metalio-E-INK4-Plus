#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "boards/common/board/board.h"
#include "boards/common/camera/camera.h"
#include "infra/image/image_decode.h"
#include "infra/network/endpoints/api_endpoints.h"
#include "infra/network/http/api_http.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "sysmgmt/tasks/task_executor.h"
#include "conversation_photo.h"

static constexpr const char* TAG = "ConversationPhoto";
static std::atomic<uint32_t> s_next_image_id{0};
static std::mutex s_mutex;
static std::weak_ptr<ConversationPhotoResult> s_request;

static bool UploadPhoto(ConversationPhotoResult& job) try {
    const int64_t started_at = esp_timer_get_time();
    PowerNeedHold hold(PowerNeed::OtaDownload);
    auto& board = Board::GetInstance();
    if (job.gate.IsCancelled()) {
        return false;
    }
    if (!board.EnsureNetworkReady(5000)) {
        ESP_LOGW(TAG, "photo upload deferred stage=network; photo retained");
        return false;
    }
    if (job.gate.IsCancelled()) {
        return false;
    }
    const std::string base = ApiVisionUploadUrl();
    if (base.empty()) {
        ESP_LOGE(TAG, "photo upload failed stage=configuration");
        return false;
    }
    const std::string url = base + "?image_id=" + job.photo->image_id;
    auto network = board.GetNetwork();
    auto http = network ? network->CreateHttp(3) : nullptr;
    if (!http) {
        ESP_LOGE(TAG, "photo upload failed stage=http_create");
        return false;
    }
    DownloadGate::Binding binding(&job.gate, http.get(), true);
    if (!binding) {
        return false;
    }
    constexpr const char* boundary = "----ESP32_ASST_CAM_BOUNDARY";
    ApiApplyCommonHeaders(http);
    http->SetHeader("Content-Type", std::string("multipart/form-data; boundary=") + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");
    http->SetTimeout(30000);
    http->SetRequestDeadline(started_at + 30000000LL);
    ESP_LOGI(TAG, "photo upload started bytes=%u", static_cast<unsigned>(job.photo->length));
    if (!http->Open("POST", url)) {
        ESP_LOGW(TAG, "photo upload failed stage=connect result=%d; photo retained", http->GetLastError());
        return false;
    }
    auto write = [&](const char* data, size_t length) {
        while (length) {
            if (job.gate.IsCancelled()) {
                return false;
            }
            const size_t chunk = std::min(length, size_t(4096));
            const int sent = http->Write(data, chunk);
            if (sent <= 0 || size_t(sent) > chunk) {
                ESP_LOGE(TAG, "photo upload failed stage=write result=%d", sent);
                return false;
            }
            data += sent;
            length -= size_t(sent);
        }
        return true;
    };
    const std::string header = std::string("--") + boundary +
                               "\r\n"
                               "Content-Disposition: form-data; name=\"image\"; filename=\"camera.jpg\"\r\n"
                               "Content-Type: image/jpeg\r\n\r\n";
    const std::string footer = std::string("\r\n--") + boundary + "--\r\n";
    if (!write(header.data(), header.size()) ||
        !write(reinterpret_cast<const char*>(job.photo->jpeg), job.photo->length) ||
        !write(footer.data(), footer.size())) {
        return false;
    }
    const int ended = http->Write("", 0);
    if (ended < 0) {
        ESP_LOGE(TAG, "photo upload failed stage=write_end result=%d", ended);
        return false;
    }
    const int status = http->GetStatusCode();
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "photo upload rejected stage=http_status status=%d; photo retained", status);
        return false;
    }
    std::string response;
    char buffer[512];
    int received = 0;
    while ((received = http->Read(buffer, sizeof(buffer))) > 0) {
        if (job.gate.IsCancelled()) {
            return false;
        }
        if (response.size() + size_t(received) > 4096) {
            ESP_LOGE(TAG, "photo upload failed stage=response_limit");
            return false;
        }
        response.append(buffer, size_t(received));
    }
    const size_t expected = http->GetBodyLength();
    if (received < 0 || (expected && expected != response.size())) {
        ESP_LOGE(TAG, "photo upload failed stage=response_read result=%d expected=%u received=%u", received,
                 static_cast<unsigned>(expected), static_cast<unsigned>(response.size()));
        return false;
    }
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(response.c_str()), cJSON_Delete);
    if (!root) {
        ESP_LOGE(TAG, "photo upload failed stage=response_json");
        return false;
    }
    const auto* success = cJSON_GetObjectItem(root.get(), "success");
    if (!cJSON_IsTrue(success)) {
        ESP_LOGW(TAG, "photo upload rejected stage=business_status; photo retained");
        return false;
    }
    const auto* image_id = cJSON_GetObjectItem(root.get(), "image_id");
    if (!cJSON_IsString(image_id) || !image_id->valuestring[0] || std::strlen(image_id->valuestring) > 64 ||
        job.photo->image_id != image_id->valuestring) {
        ESP_LOGE(TAG, "photo upload failed stage=response_image_ref");
        return false;
    }
    job.image_id = image_id->valuestring;
    job.uploaded_at = esp_timer_get_time();
    ESP_LOGI(TAG, "photo upload completed bytes=%u elapsed_ms=%lld", static_cast<unsigned>(job.photo->length),
             static_cast<long long>((job.uploaded_at - started_at) / 1000));
    return true;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "photo upload failed stage=allocate");
    return false;
}


static ConversationPhotoRequest SubmitPhoto(const std::shared_ptr<ConversationPhoto>& photo, Camera* camera,
                                             int width, int height, int thumbnail_width, int thumbnail_height,
                                             ConversationPhotoCompleted completed) try {
    std::lock_guard<std::mutex> lock(s_mutex);
    const auto previous = s_request.lock();
    if (previous && !previous->completed.load(std::memory_order_acquire)) {
        return {};
    }
    auto request = std::make_shared<ConversationPhotoResult>();
    request->photo = photo;
    if (!TaskExecutor::Schedule([request, camera, width, height, thumbnail_width, thumbnail_height,
                                 completed = std::move(completed)]() {
        {
            PowerNeedHold hold(PowerNeed::StandbyInhibit);
            try {
                if (!request->gate.IsCancelled() && camera) {
                    request->photo = std::make_shared<ConversationPhoto>();
                    auto& photo = *request->photo;
                    photo.image_id = "photo-" + std::to_string(s_next_image_id.fetch_add(1) + 1);
                    if (camera->CaptureStillJpeg(&photo.jpeg, &photo.length) && !request->gate.IsCancelled()) {
                        if (!photo.jpeg || !photo.length || photo.length > 5 * 1024 * 1024) {
                            ESP_LOGE(TAG, "operation=capture stage=jpeg_size bytes=%u", unsigned(photo.length));
                        } else {
                            ImageDecodeError error{};
                            if (DecodeImageToL8(photo.jpeg, photo.length, width, height, photo.image,
                                [](void* value) { return static_cast<ConversationPhotoResult*>(value)->gate.IsCancelled(); },
                                request.get(), &error)) {
                                if (!ScaleRasterToFit(photo.image, thumbnail_width, thumbnail_height, photo.thumbnail)) {
                                    ESP_LOGE(TAG, "operation=capture stage=thumbnail result=failed");
                                }
                            } else if (!request->gate.IsCancelled()) {
                                ESP_LOGE(TAG, "operation=capture stage=decode error=%d", int(error));
                            }
                        }
                    } else if (!request->gate.IsCancelled()) {
                        ESP_LOGE(TAG, "operation=capture stage=hardware result=failed");
                    }
                } else if (!request->gate.IsCancelled() && request->photo) {
                    if (request->photo->uploaded_at && esp_timer_get_time() - request->photo->uploaded_at < 150000000LL) {
                        request->image_id = request->photo->image_id;
                    } else {
                        UploadPhoto(*request);
                    }
                }
            } catch (const std::bad_alloc&) {
                ESP_LOGE(TAG, "operation=photo stage=execute result=no_memory");
            }
        }
        request->completed.store(true, std::memory_order_release);
        if (completed) {
            completed(request);
        }
    }, camera ? TaskDomain::Image : TaskDomain::NetworkIo)) {
        return {};
    }
    s_request = request;
    return request;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=photo stage=submit result=no_memory");
    return {};
}

ConversationPhotoRequest ConversationCapturePhoto(Camera* camera, int width, int height,
                                                  int thumbnail_width, int thumbnail_height,
                                                  ConversationPhotoCompleted completed) {
    if (!camera || width <= 0 || height <= 0 || thumbnail_width <= 0 || thumbnail_height <= 0) {
        return {};
    }
    return SubmitPhoto({}, camera, width, height, thumbnail_width, thumbnail_height, std::move(completed));
}

ConversationPhotoRequest ConversationUploadPhoto(const std::shared_ptr<ConversationPhoto>& photo,
                                                 ConversationPhotoCompleted completed) {
    if (!photo || !photo->jpeg || !photo->length) {
        return {};
    }
    return SubmitPhoto(photo, nullptr, 0, 0, 0, 0, std::move(completed));
}

void ConversationCancelPhoto(const ConversationPhotoRequest& request) {
    if (request) {
        request->gate.RequestCancel();
        TaskExecutor::Schedule([request]() { request->gate.AbortBoundHttp(); }, TaskDomain::Cleanup);
    }
}
