#include "assistant_boot_photo.h"
#include "assistant_screen_priv.h"
#include "assistant_fonts.h"
#include "assets/lang_screen.h"
#include "assets/lang_config.h"
#include "board.h"
#include "camera.h"
#include "chat_cloud.h"
#include "application.h"
#include "image_util.h"
#include "haptic_feedback.h"
#include "epd_content_policy.h"
#include "api_endpoints.h"
#include "api_http.h"
#include "a2ui_image.h"
#include "a2ui_img_cache.h"

#include <memory>
#include <new>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include "src/misc/cache/instance/lv_image_cache.h"

namespace {
constexpr int kThumbnailWidth = UiSx(140);
constexpr int kThumbnailHeight = UiSy(108);
std::atomic<uint32_t> s_generation{0}; // 页面和待机代号，阻止迟到回调。
enum class PhotoOperation {
    Idle, // 无照片任务。
    Capture, // 正在拍摄或解码。
    Upload, // 正在准备会话、上传或提交提问。
};
std::atomic<PhotoOperation> s_operation{PhotoOperation::Idle}; // 后台照片任务占用状态。
std::atomic<uint32_t> s_next_image_id{0}; // 每次拍照递增，不跨重启保存。
lv_obj_t* s_screen = nullptr;
lv_obj_t* s_tray = nullptr;
lv_obj_t* s_preview = nullptr;
lv_obj_t* s_thumbnail = nullptr;

struct Photo {
    uint8_t* jpeg = nullptr; // 原始 JPEG，后续附件上传使用。
    size_t length = 0; // JPEG 字节数。
    std::string image_id; // 本地生成、由上传响应确认的图片标识。
    int64_t uploaded_at = 0; // 最近上传完成时间，使用单调时钟。
    bool history_added = false; // 防止提问重试重复加入聊天记录。
    reader::RasterImage image; // 适屏大图。
    reader::RasterImage thumbnail; // 附件缩略图。
    ~Photo() {
        std::free(jpeg);
    }
};
std::shared_ptr<Photo> s_pending;
std::shared_ptr<Photo> s_recording_photo;
std::shared_ptr<Photo> s_submitted_photo;
uint32_t s_recording_turn = 0; // 仅 LVGL 任务访问。
void ClearPending() {
    if (!s_pending) return;
    lv_image_cache_drop(&s_pending->image.dsc);
    lv_image_cache_drop(&s_pending->thumbnail.dsc);
    s_pending.reset();
}
struct CaptureJob {
    Camera* camera = nullptr; // 板级缓存摄像头，生命周期覆盖后台任务。
    uint32_t generation = 0; // 拍摄所属页面代号。
    std::shared_ptr<Photo> photo; // 后台创建所有权，UI 接收时不再分配控制块。
};

struct UploadJob {
    std::shared_ptr<Photo> photo; // 保持原图存活，离页清理不会释放上传中的数据。
    uint32_t generation = 0; // 请求所属页面代号。
    std::string image_id; // 已验证的上传结果。
    int64_t uploaded_at = 0; // 本次 HTTP 上传完成时间；缓存重试保持为零。
    std::string history_url; // 本机会话图片缓存键。
    uint32_t turn = 0; // 语音任务分配的提问轮次。
    bool recorded = false; // 照片关联录音时，上传后发送 stop。
    Application::ImageQuestionResult result = Application::ImageQuestionResult::Failed; // 提问提交结果。
};

bool AppendUploadedPhoto(UploadJob& job) try {
    if (job.history_url.empty()) {
        ESP_LOGE(TAG, "photo history failed stage=cache_key");
        return false;
    }
    using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
    Json root(cJSON_Parse(R"({"updateComponents":{"root":"photo","components":[
        {"id":"photo","component":"Row","children":["sender","shot"]},
        {"id":"sender","component":"RichText","spans":[{"bold":true}]},
        {"id":"shot","component":"Image"}
    ]}})"), cJSON_Delete);
    if (!root) {
        ESP_LOGE(TAG, "photo history failed stage=metadata_allocate");
        return false;
    }
    auto* components = cJSON_GetObjectItem(cJSON_GetObjectItem(root.get(), "updateComponents"), "components");
    auto* sender = cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetArrayItem(components, 1), "spans"), 0);
    auto* image = cJSON_GetArrayItem(components, 2);
    if (!cJSON_AddStringToObject(sender, "text", Lang::Strings::ASSISTANT_USER_PREFIX) ||
        !cJSON_AddStringToObject(image, "url", job.history_url.c_str()) ||
        !cJSON_AddNumberToObject(image, "width", job.photo->thumbnail.width) ||
        !cJSON_AddNumberToObject(image, "height", job.photo->thumbnail.height)) {
        ESP_LOGE(TAG, "photo history failed stage=metadata_fields");
        return false;
    }
    std::unique_ptr<char, decltype(&cJSON_free)> json(cJSON_PrintUnformatted(root.get()), cJSON_free);
    if (!json) {
        ESP_LOGE(TAG, "photo history failed stage=metadata_encode");
        return false;
    }
    const auto& raster = job.photo->thumbnail;
    const auto cached = a2ui_image_put_local_l8(job.history_url.c_str(), raster.width, raster.height,
                                               raster.pixels.data(), raster.pixels.size());
    if (cached != ESP_OK) {
        ESP_LOGE(TAG, "photo history failed stage=ram_cache result=%d", static_cast<int>(cached));
        return false;
    }
    AssistantScreen::AddMessage("user", json.get());
    std::lock_guard<std::mutex> guard(Assistant_State().stream.session_mu);
    for (const auto& item : Assistant_State().layout.flow) {
        if (item.kind == FlowKind::Image && item.image_url == job.history_url.c_str()) return true;
    }
    ESP_LOGE(TAG, "photo history failed stage=insert");
    return false;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "photo history failed stage=allocate");
    return false;
}

bool UploadPhoto(UploadJob& job) try {
    const int64_t started_at = esp_timer_get_time();
    PowerNeedHold hold(PowerNeed::OtaDownload);
    auto& board = Board::GetInstance();
    if (job.generation != s_generation.load()) return false;
    if (!board.EnsureNetworkReady(5000)) {
        ESP_LOGW(TAG, "photo upload deferred stage=network; photo retained");
        return false;
    }
    if (job.generation != s_generation.load()) return false;
    const std::string base = api::VisionUploadUrl();
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
    constexpr const char* boundary = "----ESP32_ASST_CAM_BOUNDARY";
    api::ApplyCommonHeaders(http);
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
            if (job.generation != s_generation.load()) return false;
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
    const std::string header = std::string("--") + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"image\"; filename=\"camera.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n";
    const std::string footer = std::string("\r\n--") + boundary + "--\r\n";
    if (!write(header.data(), header.size()) ||
        !write(reinterpret_cast<const char*>(job.photo->jpeg), job.photo->length) ||
        !write(footer.data(), footer.size())) return false;
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
        if (job.generation != s_generation.load()) return false;
        if (response.size() + size_t(received) > 4096) {
            ESP_LOGE(TAG, "photo upload failed stage=response_limit");
            return false;
        }
        response.append(buffer, size_t(received));
    }
    const size_t expected = http->GetBodyLength();
    if (received < 0 || (expected && expected != response.size())) {
        ESP_LOGE(TAG, "photo upload failed stage=response_read result=%d expected=%u received=%u",
                 received, static_cast<unsigned>(expected), static_cast<unsigned>(response.size()));
        return false;
    }
    http->Close();
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
    if (!cJSON_IsString(image_id) || !image_id->valuestring[0] ||
        std::strlen(image_id->valuestring) > 64 || job.photo->image_id != image_id->valuestring) {
        ESP_LOGE(TAG, "photo upload failed stage=response_image_ref");
        return false;
    }
    job.image_id = image_id->valuestring;
    job.uploaded_at = esp_timer_get_time();
    ESP_LOGI(TAG, "photo upload completed bytes=%u elapsed_ms=%lld",
             static_cast<unsigned>(job.photo->length),
             static_cast<long long>((job.uploaded_at - started_at) / 1000));
    return true;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "photo upload failed stage=allocate");
    return false;
}

void ApplySubmitted(void* argument) {
    std::unique_ptr<UploadJob> job(static_cast<UploadJob*>(argument));
    s_operation.store(PhotoOperation::Idle);
    if (job->generation != s_generation.load() || !AssistantScreen::IsActive()) return;
    if (job->result == Application::ImageQuestionResult::Failed) {
        ScreenShowToast(Lang::Strings::ASSISTANT_PHOTO_QUESTION_FAILED);
        return;
    }
    if (job->result == Application::ImageQuestionResult::Sent) s_submitted_photo = job->photo;
    if (s_pending == job->photo) {
        lv_image_set_src(s_thumbnail, nullptr);
        ClearPending();
        lv_obj_add_flag(s_tray, LV_OBJ_FLAG_HIDDEN);
        Assistant_UpdatePhotoAction();
    }
    if (job->result == Application::ImageQuestionResult::Sent) ScreenShowToast(Lang::Strings::ASSISTANT_PHOTO_SENT);
}

void ApplyUpload(void* argument) {
    std::unique_ptr<UploadJob> job(static_cast<UploadJob*>(argument));
    if (job->uploaded_at != 0) job->photo->uploaded_at = job->uploaded_at;
    if (job->generation != s_generation.load() || !AssistantScreen::IsActive()) {
        s_operation.store(PhotoOperation::Idle);
        return;
    }
    if (job->image_id.empty() || (!job->photo->history_added && !AppendUploadedPhoto(*job))) {
        s_operation.store(PhotoOperation::Idle);
        Application::GetInstance().CompleteBaiwenQuestion(job->turn, false, "");
        ScreenShowToast(job->image_id.empty() ? Lang::Strings::ASSISTANT_PHOTO_UPLOAD_FAILED :
                                               Lang::Strings::ASSISTANT_PHOTO_HISTORY_FAILED);
        return;
    }
    job->photo->history_added = true;
    auto* context = job.get();
    if (Application::GetInstance().CompleteBaiwenQuestion(job->turn, job->recorded, job->image_id,
        [context](Application::ImageQuestionResult result) {
            context->result = result;
            if (!ScreenLvAsync(ApplySubmitted, context) && !ScreenLvAsyncUrgent(ApplySubmitted, context)) {
                delete context;
                s_operation.store(PhotoOperation::Idle);
                ESP_LOGE(TAG, "photo submission result failed stage=ui_dispatch");
            }
        })) {
        job.release();
    } else {
        s_operation.store(PhotoOperation::Idle);
        ScreenShowToast(Lang::Strings::ASSISTANT_PHOTO_QUESTION_FAILED);
    }
}

bool PrepareHistoryCache(UploadJob& job) try {
    if (job.generation != s_generation.load() || job.photo->thumbnail.empty()) return false;
    char url[80];
    std::snprintf(url, sizeof(url), "mem://assistant/upload_%016llx",
                  static_cast<unsigned long long>(esp_timer_get_time()));
    job.history_url = url;
    if (a2ui_img_cache_is_ready()) {
        std::vector<uint8_t> encoded;
        if (reader::EncodeL8ToA2l8(job.photo->thumbnail, encoded)) {
            if (a2ui_img_cache_store(url, encoded.data(), encoded.size()) != ESP_OK) {
                ESP_LOGW(TAG, "photo cache unavailable stage=sd_store; using RAM cache");
            }
        } else {
            ESP_LOGW(TAG, "photo cache unavailable stage=sd_encode; using RAM cache");
        }
    }
    return true;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "photo cache failed stage=allocate");
    return false;
}

void UploadWorker(void* argument) {
    auto* job = static_cast<UploadJob*>(argument);
    if (!job->image_id.empty() && esp_timer_get_time() - job->photo->uploaded_at >= 150000000LL) {
        job->image_id.clear();
    }
    if (job->image_id.empty()) UploadPhoto(*job);
    if (!job->image_id.empty() && !job->photo->history_added && !PrepareHistoryCache(*job)) job->history_url.clear();
    if (!ScreenLvAsync(ApplyUpload, job) && !ScreenLvAsyncUrgent(ApplyUpload, job)) {
        Application::GetInstance().CompleteBaiwenQuestion(job->turn, false, "");
        delete job;
        s_operation.store(PhotoOperation::Idle);
        ESP_LOGE(TAG, "photo upload result failed stage=ui_dispatch");
    }
    vTaskDeleteWithCaps(nullptr);
}

lv_obj_t* Button(lv_obj_t* parent, const char* text, int width, lv_event_cb_t callback) {
    auto* button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(button, width, UiSy(52));
    lv_obj_set_style_bg_color(button, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(button, UiSx(8), 0);
    lv_obj_set_style_border_color(button, lv_color_black(), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_text_color(button, lv_color_black(), 0);
    lv_obj_set_style_text_font(button, &lv_font_montserrat_28, 0);
    Assistant_DisableScroll(button);
    auto* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    HapticAttachClick(button);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
    return button;
}

void OpenPreview(lv_event_t*) {
    if (!s_pending || s_preview) return;
    if (s_operation.load() != PhotoOperation::Idle || AssistantScreen::IsPttHeld()) {
        ScreenShowToast(Lang::Strings::ASSISTANT_PHOTO_BUSY);
        return;
    }
    s_preview = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_preview);
    lv_obj_add_flag(s_preview, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_preview, LV_HOR_RES, LV_VER_RES - ScreenStatusBarHeight());
    lv_obj_align(s_preview, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_preview, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_preview, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_preview, 0, 0);
    lv_obj_set_style_radius(s_preview, 0, 0);
    Assistant_DisableScroll(s_preview);
    lv_obj_clear_flag(s_preview, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_preview, [](lv_event_t* event) {
        if (lv_event_get_target(event) == s_preview) AssistantPhoto_ClosePreview();
    }, LV_EVENT_CLICKED, nullptr);
    const int max_w = LV_HOR_RES - UiSx(64);
    const int max_h = (LV_VER_RES - ScreenStatusBarHeight()) * 3 / 4 - UiSy(88);
    const int scale = std::min(256, std::min(max_w * 256 / s_pending->image.width,
                                           max_h * 256 / s_pending->image.height));
    const int image_w = s_pending->image.width * scale / 256;
    const int image_h = s_pending->image.height * scale / 256;
    auto* window = lv_obj_create(s_preview);
    lv_obj_remove_style_all(window);
    lv_obj_add_flag(window, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(window, image_w + UiSx(24), image_h + UiSy(88));
    lv_obj_center(window);
    lv_obj_set_style_bg_color(window, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(window, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(window, 2, 0);
    lv_obj_set_style_border_color(window, lv_color_black(), 0);
    lv_obj_set_style_radius(window, UiSx(12), 0);
    lv_obj_set_style_pad_all(window, 0, 0);
    lv_obj_set_style_shadow_width(window, 0, 0);
    Assistant_DisableScroll(window);
    auto* image = lv_image_create(window);
    lv_image_set_src(image, &s_pending->image.dsc);
    lv_obj_set_size(image, image_w, image_h);
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
    EpdSetContentRole(image, EpdContentRole::Photo);
    lv_obj_align(image, LV_ALIGN_TOP_MID, 0, UiSy(12));
    auto* close = Button(window, Lang::Strings::SETTINGS_TEST_WIFI_CLOSE, UiSx(112), [](lv_event_t*) {
        AssistantPhoto_ClosePreview();
    });
    lv_obj_set_style_text_font(close, Assistant_UiFont(), 0);
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -UiSy(10));
}

void ApplyCapture(void* argument) {
    std::unique_ptr<CaptureJob> job(static_cast<CaptureJob*>(argument));
    s_operation.store(PhotoOperation::Idle);
    if (job->generation != s_generation.load() || !AssistantScreen::IsActive() || !s_tray) return;
    if (!job->photo || job->photo->image.empty() || job->photo->thumbnail.empty()) {
        ScreenShowToast(Lang::Strings::ASSISTANT_PHOTO_FAILED);
        return;
    }
    AssistantPhoto_ClosePreview();
    // 先解除控件引用，再销毁旧照片及其图像缓存。
    lv_image_set_src(s_thumbnail, nullptr);
    ClearPending();
    s_pending = std::move(job->photo);
    lv_image_set_src(s_thumbnail, &s_pending->thumbnail.dsc);
    lv_obj_remove_flag(s_tray, LV_OBJ_FLAG_HIDDEN);
    Assistant_UpdatePhotoAction();
}

void CaptureWorker(void* argument) {
    auto* job = static_cast<CaptureJob*>(argument);
    try {
        job->photo = std::make_shared<Photo>();
        job->photo->image_id = "photo-" + std::to_string(s_next_image_id.fetch_add(1) + 1);
    } catch (const std::bad_alloc&) {
        job->photo.reset();
        ESP_LOGE(TAG, "photo capture failed stage=allocate");
    }
    if (job->photo && job->generation == s_generation.load()) {
        auto& photo = *job->photo;
        if (job->camera->CaptureStillJpeg(&photo.jpeg, &photo.length) && job->generation == s_generation.load()) {
            if (!photo.jpeg || photo.length == 0 || photo.length > 5 * 1024 * 1024) {
                ESP_LOGE(TAG, "photo capture failed stage=jpeg_size bytes=%u", static_cast<unsigned>(photo.length));
            } else {
                reader::ImageDecodeError error{};
                if (reader::DecodeImageToL8(photo.jpeg, photo.length, LV_HOR_RES - UiSx(24),
                                           LV_VER_RES - ScreenStatusBarHeight() - UiSy(90), photo.image,
                                           [](void* context) {
                                               return static_cast<CaptureJob*>(context)->generation != s_generation.load();
                                           }, job, &error)) {
                    if (!reader::ScaleRasterToFit(photo.image, UiSx(124), UiSy(96), photo.thumbnail)) {
                        ESP_LOGE(TAG, "photo capture failed stage=thumbnail");
                    } else {
                        ESP_LOGI(TAG, "photo capture completed bytes=%u", static_cast<unsigned>(photo.length));
                    }
                } else if (job->generation == s_generation.load()) {
                    ESP_LOGE(TAG, "photo capture failed stage=decode result=%d", static_cast<int>(error));
                }
            }
        }
    }
    if (!ScreenLvAsync(ApplyCapture, job) && !ScreenLvAsyncUrgent(ApplyCapture, job)) {
        delete job;
        s_operation.store(PhotoOperation::Idle);
        ESP_LOGE(TAG, "photo capture result failed stage=ui_dispatch");
    }
    vTaskDeleteWithCaps(nullptr);
}

} // namespace

static void RequestCapture() {
    if (!AssistantScreen::IsActive() || chat_cloud::ActiveProvider() != chat_cloud::Provider::Baiwen) return;
    auto* camera = Board::GetInstance().GetCamera();
    if (!camera) return;
    const auto state = Application::GetInstance().GetDeviceState();
    if (s_preview) {
        ScreenShowToast(Lang::Strings::ASSISTANT_PHOTO_CLOSE);
        return;
    }
    if (s_operation.load() != PhotoOperation::Idle || AssistantScreen::IsPttHeld() || state == kDeviceStateListening) {
        ScreenShowToast(Lang::Strings::ASSISTANT_PHOTO_BUSY);
        return;
    }
    s_submitted_photo.reset();
    auto* job = new (std::nothrow) CaptureJob;
    if (!job) {
        ESP_LOGE(TAG, "photo capture failed stage=job_allocate");
        ScreenShowToast(Lang::Strings::ASSISTANT_PHOTO_FAILED);
        return;
    }
    job->camera = camera;
    job->generation = s_generation.load();
    s_operation.store(PhotoOperation::Capture);
    const auto result = xTaskCreatePinnedToCoreWithCaps(CaptureWorker, "asst_photo", 16 * 1024,
        job, 5, nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (result != pdPASS) {
        s_operation.store(PhotoOperation::Idle);
        delete job;
        ESP_LOGE(TAG, "photo capture failed stage=task_create result=%d", static_cast<int>(result));
        ScreenShowToast(Lang::Strings::ASSISTANT_PHOTO_FAILED);
    } else {
        ESP_LOGI(TAG, "photo capture started");
        ScreenShowToast(Lang::Strings::SETTINGS_TEST_CAPTURING);
    }
}

bool AssistantBootPhoto_OnDoubleClick() {
    if (!AssistantScreen::IsActive() || chat_cloud::ActiveProvider() != chat_cloud::Provider::Baiwen) return false;
    const auto generation = s_generation.load();
    return ScreenLvAsync([](void* argument) {
        if (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(argument)) == s_generation.load() && s_screen) {
            RequestCapture();
        }
    }, reinterpret_cast<void*>(static_cast<uintptr_t>(generation)));
}

void AssistantPhoto_Create(lv_obj_t* screen) {
    s_generation.fetch_add(1);
    s_screen = screen;
    s_tray = lv_obj_create(screen);
    lv_obj_remove_style_all(s_tray);
    lv_obj_add_flag(s_tray, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_tray, kThumbnailWidth, kThumbnailHeight);
    lv_obj_align(s_tray, LV_ALIGN_BOTTOM_LEFT, kFooterInset + UiSx(6), -kFooterH - UiSy(4));
    lv_obj_set_style_pad_all(s_tray, 0, 0);
    lv_obj_set_style_border_width(s_tray, 1, 0);
    lv_obj_set_style_border_color(s_tray, lv_color_black(), 0);
    lv_obj_set_style_bg_color(s_tray, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_tray, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_tray, UiSx(8), 0);
    lv_obj_set_style_clip_corner(s_tray, true, 0);
    Assistant_DisableScroll(s_tray);
    lv_obj_add_flag(s_tray, LV_OBJ_FLAG_HIDDEN);
    s_thumbnail = lv_image_create(s_tray);
    EpdSetContentRole(s_thumbnail, EpdContentRole::Photo);
    lv_obj_center(s_thumbnail);
    lv_obj_add_event_cb(s_tray, OpenPreview, LV_EVENT_CLICKED, nullptr);
    auto* remove = Button(s_tray, LV_SYMBOL_CLOSE, UiSx(40), [](lv_event_t*) {
        if (s_operation.load() != PhotoOperation::Idle || s_preview || AssistantScreen::IsPttHeld()) {
            ScreenShowToast(Lang::Strings::ASSISTANT_PHOTO_BUSY);
            return;
        }
        lv_image_set_src(s_thumbnail, nullptr);
        ClearPending();
        lv_obj_add_flag(s_tray, LV_OBJ_FLAG_HIDDEN);
        Assistant_UpdatePhotoAction();
    });
    lv_obj_set_height(remove, UiSx(40));
    lv_obj_set_style_radius(remove, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(remove, LV_ALIGN_TOP_RIGHT, -UiSx(4), UiSy(4));
}

bool AssistantPhoto_ClosePreview() {
    if (!s_preview) return false;
    lv_obj_delete(s_preview);
    s_preview = nullptr;
    return true;
}

void AssistantPhoto_Suspend() {
    s_generation.fetch_add(1);
    AssistantPhoto_ClosePreview();
}

void AssistantPhoto_Reset() {
    AssistantPhoto_Suspend();
    if (s_thumbnail) lv_image_set_src(s_thumbnail, nullptr);
    ClearPending();
    s_recording_photo.reset();
    s_submitted_photo.reset();
    s_recording_turn = 0;
    s_screen = s_tray = s_thumbnail = nullptr;
}

bool AssistantPhoto_AllowListening() {
    if (s_preview) {
        ScreenShowToast(Lang::Strings::ASSISTANT_PHOTO_CLOSE);
        return false;
    }
    if (s_operation.load() == PhotoOperation::Capture) {
        ScreenShowToast(Lang::Strings::ASSISTANT_PHOTO_BUSY);
        return false;
    }
    return true;
}

bool AssistantPhoto_HasPending() {
    return s_pending != nullptr;
}

namespace {
void StartUpload(const std::shared_ptr<Photo>& photo, uint32_t turn, bool recorded) {
    auto* job = new (std::nothrow) UploadJob;
    if (job) {
        job->photo = photo;
        job->generation = s_generation.load();
        job->turn = turn;
        job->recorded = recorded;
        try {
            if (photo->uploaded_at != 0) job->image_id = photo->image_id;
        } catch (const std::bad_alloc&) {
            delete job;
            job = nullptr;
        }
    }
    s_operation.store(PhotoOperation::Upload);
    const auto created = job ? xTaskCreatePinnedToCoreWithCaps(UploadWorker, "asst_upload", 16 * 1024, job, 5, nullptr,
                                                              0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : pdFAIL;
    if (created != pdPASS) {
        ESP_LOGE(TAG, "photo upload failed stage=%s result=%d", job ? "task_create" : "job_allocate",
                 static_cast<int>(created));
        delete job;
        s_operation.store(PhotoOperation::Idle);
        Application::GetInstance().CompleteBaiwenQuestion(turn, false, "");
        ScreenShowToast(Lang::Strings::ASSISTANT_PHOTO_UPLOAD_FAILED);
        return;
    }
    ScreenShowToast(Lang::Strings::ASSISTANT_PHOTO_UPLOADING);
}
}

void AssistantPhoto_Send() {
    if (!s_pending || !AssistantScreen::IsActive() || chat_cloud::ActiveProvider() != chat_cloud::Provider::Baiwen) return;
    if (const char* hint = Board::GetInstance().GetNetworkErrorText(nullptr)) {
        ScreenShowToast(hint);
        return;
    }
    if (s_operation.load() != PhotoOperation::Idle || AssistantScreen::IsPttHeld()) {
        ScreenShowToast(Lang::Strings::ASSISTANT_PHOTO_BUSY);
        return;
    }
    s_operation.store(PhotoOperation::Upload);
    if (!Application::GetInstance().AskBaiwenImage()) {
        s_operation.store(PhotoOperation::Idle);
        ScreenShowToast(Lang::Strings::ASSISTANT_PHOTO_QUESTION_FAILED);
    }
}

void AssistantPhoto_UploadQuestion(uint32_t turn, bool ready) {
    if (!ready || !s_pending || !s_screen || !AssistantScreen::IsActive()) {
        s_operation.store(PhotoOperation::Idle);
        if (s_screen && AssistantScreen::IsActive()) ScreenShowToast(Lang::Strings::ASSISTANT_PHOTO_QUESTION_FAILED);
        return;
    }
    StartUpload(s_pending, turn, false);
}

void AssistantPhoto_BeginListening(uint32_t turn) {
    s_submitted_photo.reset();
    s_recording_turn = turn;
    s_recording_photo = s_operation.load() == PhotoOperation::Idle ? s_pending : nullptr;
}

void AssistantPhoto_EndListening(uint32_t turn) {
    if (turn != s_recording_turn) return;
    auto photo = std::move(s_recording_photo);
    s_recording_turn = 0;
    if (photo && s_operation.load() == PhotoOperation::Idle) {
        StartUpload(photo, turn, true);
    } else if (photo) {
        // 松手与点击发送相邻发生时，由后来的单图发送接管，避免重复上传。
        Application::GetInstance().CompleteBaiwenQuestion(turn, false, "");
    } else if (!Application::GetInstance().CompleteBaiwenQuestion(turn, true, "")) {
        ScreenShowToast(Lang::Strings::ASSISTANT_PHOTO_QUESTION_FAILED);
    }
}

void AssistantPhoto_QuestionError(const std::string& image_id, bool reupload, bool disabled) {
    if (!s_submitted_photo || s_submitted_photo->image_id != image_id || !s_tray) return;
    if (!s_pending && s_operation.load() == PhotoOperation::Idle) {
        s_pending = std::move(s_submitted_photo);
        if (reupload) s_pending->uploaded_at = 0;
        lv_image_set_src(s_thumbnail, &s_pending->thumbnail.dsc);
        lv_obj_remove_flag(s_tray, LV_OBJ_FLAG_HIDDEN);
        Assistant_UpdatePhotoAction();
    }
    ScreenShowToast(disabled ? Lang::Strings::ASSISTANT_PHOTO_DISABLED : Lang::Strings::ASSISTANT_PHOTO_ANALYSIS_FAILED);
}

void AssistantPhoto_FinishQuestion() {
    s_submitted_photo.reset();
}
