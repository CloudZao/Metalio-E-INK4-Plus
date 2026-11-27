#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include "business/conversation/conversation_presentation.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "business/conversation/conversation_controller.h"
#include "sysmgmt/system_state.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/apps/assistant/assistant_boot_photo.h"
#include "assets/lang_screen.h"
#include "ui/apps/assistant/assistant_screen_priv.h"
#include "boards/common/board/board.h"
#include "infra/image/image_decode.h"
#include "ui/common/drawing/a2ui/a2ui_image.h"
#include "ui/common/drawing/a2ui/a2ui_img_cache.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/drawing/rendering/epd_content_policy.h"
#include "ui/common/widgets/screen_text.h"
#include "ui/common/drawing/ui_layout.h"
#include "assets/lang_config.h"
#include "business/conversation/chat_cloud.h"
#include "business/conversation/conversation_photo.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/runtime/modal.h"

LV_FONT_DECLARE(font_awesome_30_1);

static constexpr int kThumbnailWidth = UiSx(140);
static constexpr int kThumbnailHeight = UiSy(108);
static std::atomic<uint32_t> s_generation{0};  // 页面和待机代号，阻止迟到回调。
enum class ConversationPhotoOperation {
    Idle,     // 无照片任务。
    Capture,  // 正在拍摄或解码。
    Upload,   // 正在准备会话、上传或提交提问。
};
static std::atomic<ConversationPhotoOperation> s_operation{
    ConversationPhotoOperation::Idle};      // 后台照片任务占用状态。
static ConversationPhotoRequest s_photo_request;
static lv_obj_t* s_screen = nullptr;
static lv_obj_t* s_tray = nullptr;
static lv_obj_t* s_preview = nullptr;
static lv_obj_t* s_thumbnail = nullptr;

static std::shared_ptr<ConversationPhoto> s_pending;
static std::shared_ptr<ConversationPhoto> s_recording_photo;
static std::shared_ptr<ConversationPhoto> s_submitted_photo;
static uint32_t s_recording_turn = 0;  // 仅 LVGL 任务访问。

static UiModal s_window_modal;

static void ClearPending() {
    if (!s_pending) {
        return;
    }
    lv_image_cache_drop(&s_pending->image.dsc);
    lv_image_cache_drop(&s_pending->thumbnail.dsc);
    s_pending.reset();
}

struct AssistantBootPhotoCaptureJob : ScreenUiCompletion {
    uint32_t generation = 0;                         // 拍摄所属页面代号。
    std::shared_ptr<ConversationPhoto> photo;  // 后台创建所有权，UI 接收时不再分配控制块。
};

struct AssistantBootPhotoUploadJob : ScreenUiCompletion {
    std::shared_ptr<ConversationPhoto> photo;            // 保持原图存活，离页清理不会释放上传中的数据。
    uint32_t generation = 0;                                   // 请求所属页面代号。
    std::string image_id;                                      // 已验证的上传结果。
    int64_t uploaded_at = 0;                                   // 本次 HTTP 上传完成时间；缓存重试保持为零。
    std::string history_url;                                   // 本机会话图片缓存键。
    uint32_t turn = 0;                                         // 语音任务分配的提问轮次。
    bool recorded = false;                                     // 照片关联录音时，上传后发送 stop。
    ImageQuestionResult result = ImageQuestionResult::Failed;  // 提问提交结果。
};

static bool AppendUploadedPhoto(AssistantBootPhotoUploadJob& job) try {
    if (job.history_url.empty()) {
        ESP_LOGE(TAG, "photo history failed stage=cache_key");
        return false;
    }
    using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
    Json root(cJSON_Parse(R"({"updateComponents":{"root":"photo","components":[
        {"id":"photo","component":"Row","children":["sender","shot"]},
        {"id":"sender","component":"RichText","spans":[{"bold":true}]},
        {"id":"shot","component":"Image"}
    ]}})"),
              cJSON_Delete);
    if (!root) {
        ESP_LOGE(TAG, "photo history failed stage=metadata_allocate");
        return false;
    }
    auto* components = cJSON_GetObjectItem(cJSON_GetObjectItem(root.get(), "updateComponents"), "components");
    auto* sender = cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetArrayItem(components, 1), "spans"), 0);
    auto* image = cJSON_GetArrayItem(components, 2);
    if (!cJSON_AddStringToObject(sender, "text", LangString_ASSISTANT_USER_PREFIX) ||
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
    ConversationPresentation::AddMessage("user", json.get());
    std::lock_guard<std::mutex> guard(Assistant_State().stream.session_mu);
    for (const auto& item : Assistant_State().layout.flow) {
        if (item.kind == FlowKind::Image && item.image_url == job.history_url.c_str()) {
            return true;
        }
    }
    ESP_LOGE(TAG, "photo history failed stage=insert");
    return false;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "photo history failed stage=allocate");
    return false;
}

static void ApplySubmitted(void* argument) {
    std::unique_ptr<AssistantBootPhotoUploadJob> job(static_cast<AssistantBootPhotoUploadJob*>(argument));
    if (job->generation != s_generation.load() || !ConversationPresentation::IsActive()) {
        return;
    }
    s_operation.store(ConversationPhotoOperation::Idle);
    if (job->result == ImageQuestionResult::Failed) {
        ScreenShowToast(LangString_ASSISTANT_PHOTO_QUESTION_FAILED);
        return;
    }
    if (job->result == ImageQuestionResult::Sent) {
        s_submitted_photo = job->photo;
    }
    if (s_pending == job->photo) {
        lv_image_set_src(s_thumbnail, nullptr);
        ClearPending();
        lv_obj_add_flag(s_tray, LV_OBJ_FLAG_HIDDEN);
        Assistant_UpdatePhotoAction();
    }
    if (job->result == ImageQuestionResult::Sent) {
        ScreenShowToast(LangString_ASSISTANT_PHOTO_SENT);
    }
}

static void ApplyUpload(void* argument) {
    std::unique_ptr<AssistantBootPhotoUploadJob> job(static_cast<AssistantBootPhotoUploadJob*>(argument));
    if (job->uploaded_at != 0) {
        job->photo->uploaded_at = job->uploaded_at;
    }
    if (job->generation != s_generation.load() || !ConversationPresentation::IsActive()) {
        return;
    }
    s_photo_request.reset();
    if (job->image_id.empty() || (!job->photo->history_added && !AppendUploadedPhoto(*job))) {
        s_operation.store(ConversationPhotoOperation::Idle);
        ConversationController::GetInstance().CompleteQuestion(job->turn, false, "");
        ScreenShowToast(job->image_id.empty() ? LangString_ASSISTANT_PHOTO_UPLOAD_FAILED
                                              : LangString_ASSISTANT_PHOTO_HISTORY_FAILED);
        return;
    }
    job->photo->history_added = true;
    auto* context = job.get();
    if (ConversationController::GetInstance().CompleteQuestion(
            job->turn, job->recorded, job->image_id, [context](ImageQuestionResult result) {
                context->result = result;
                context->apply = [](ScreenUiCompletion* completion) { ApplySubmitted(completion); };
                ScreenCompleteWork(context);
            })) {
        job.release();
    } else {
        s_operation.store(ConversationPhotoOperation::Idle);
        ScreenShowToast(LangString_ASSISTANT_PHOTO_QUESTION_FAILED);
    }
}

static bool PrepareHistoryCache(AssistantBootPhotoUploadJob& job) try {
    if (job.generation != s_generation.load() || job.photo->thumbnail.empty()) {
        return false;
    }
    char url[80];
    std::snprintf(url, sizeof(url), "mem://assistant/upload_%016llx",
                  static_cast<unsigned long long>(esp_timer_get_time()));
    job.history_url = url;
    if (a2ui_img_cache_is_ready()) {
        std::vector<uint8_t> encoded;
        if (EncodeL8ToA2l8(job.photo->thumbnail, encoded)) {
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

static lv_obj_t* Button(lv_obj_t* parent, const char* text, int width, lv_event_cb_t callback) {
    auto* button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(button, width, UiSy(52));
    lv_obj_set_style_bg_color(button, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(button, UiSx(8), 0);
    lv_obj_set_style_border_color(button, UiColorForeground(), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_text_color(button, UiColorForeground(), 0);
    lv_obj_set_style_text_font(button, Assistant_UiFont(), 0);
    Assistant_DisableScroll(button);
    auto* label = lv_label_create(button);
    lv_label_set_text(label, text);
    if (std::strcmp(text, LV_SYMBOL_CLOSE) == 0) {
        lv_obj_set_style_text_font(label, &font_awesome_30_1, 0);
    } else {
        lv_obj_add_event_cb(button, ScreenFitButtonLabelOnSize, LV_EVENT_SIZE_CHANGED, label);
    }
    lv_obj_center(label);
    HapticAttachClick(button);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
    return button;
}

static void OpenPreview(lv_event_t*) {
    if (!s_pending || s_preview) {
        return;
    }
    if (s_operation.load() != ConversationPhotoOperation::Idle || ConversationPresentation::IsPttHeld()) {
        ScreenShowToast(LangString_ASSISTANT_PHOTO_BUSY);
        return;
    }
    s_preview = lv_obj_create(lv_layer_top());
    s_window_modal.Bind(s_preview, [](void*) { AssistantPhoto_ClosePreview(); });
    lv_obj_remove_style_all(s_preview);
    lv_obj_add_flag(s_preview, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_preview, LV_HOR_RES, LV_VER_RES - ScreenStatusBarHeight());
    lv_obj_align(s_preview, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_preview, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(s_preview, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_preview, 0, 0);
    lv_obj_set_style_radius(s_preview, 0, 0);
    Assistant_DisableScroll(s_preview);
    lv_obj_clear_flag(s_preview, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(
        s_preview,
        [](lv_event_t* event) {
            if (lv_event_get_target(event) == s_preview) {
                AssistantPhoto_ClosePreview();
            }
        },
        LV_EVENT_CLICKED, nullptr);
    const int max_w = std::max<int>(1, LV_HOR_RES - UiSx(64));
    const int controls_h = std::max<int>(UiSy(88), Assistant_UiFont()->line_height + UiSy(36));
    const int max_h = std::max<int>(1, (LV_VER_RES - ScreenStatusBarHeight()) * 3 / 4 - controls_h);
    const auto image_size = UiLayoutFitImage(
        {max_w, max_h}, {static_cast<int>(s_pending->image.width), static_cast<int>(s_pending->image.height)}, false);
    const int image_w = image_size.width, image_h = image_size.height;
    auto* window = lv_obj_create(s_preview);
    lv_obj_remove_style_all(window);
    lv_obj_add_flag(window, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(window, image_w + UiSx(24), image_h + controls_h);
    lv_obj_center(window);
    lv_obj_set_style_bg_color(window, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(window, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(window, 2, 0);
    lv_obj_set_style_border_color(window, UiColorForeground(), 0);
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
    auto* close = Button(window, LangString_SETTINGS_TEST_WIFI_CLOSE, UiSx(112),
                         [](lv_event_t*) { AssistantPhoto_ClosePreview(); });
    lv_obj_set_style_text_font(close, Assistant_UiFont(), 0);
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -UiSy(10));
}

static void ApplyCapture(void* argument) {
    std::unique_ptr<AssistantBootPhotoCaptureJob> job(static_cast<AssistantBootPhotoCaptureJob*>(argument));
    if (job->generation != s_generation.load() || !ConversationPresentation::IsActive() || !s_tray) {
        return;
    }
    s_photo_request.reset();
    s_operation.store(ConversationPhotoOperation::Idle);
    if (!job->photo || job->photo->image.empty() || job->photo->thumbnail.empty()) {
        ScreenShowToast(LangString_ASSISTANT_PHOTO_FAILED);
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

static void RequestCapture() {
    if (!ConversationPresentation::IsActive() || ChatCloudActiveProvider() != ChatCloudProvider::Baiwen) {
        return;
    }
    auto* camera = Board::GetInstance().GetCamera();
    if (!camera) {
        return;
    }
    const auto state = SystemState::GetInstance().Device();
    if (s_preview) {
        ScreenShowToast(LangString_ASSISTANT_PHOTO_CLOSE);
        return;
    }
    if (s_operation.load() != ConversationPhotoOperation::Idle || ConversationPresentation::IsPttHeld() ||
        state == kDeviceStateListening) {
        ScreenShowToast(LangString_ASSISTANT_PHOTO_BUSY);
        return;
    }
    s_submitted_photo.reset();
    auto* job = new (std::nothrow) AssistantBootPhotoCaptureJob;
    if (!job) {
        ESP_LOGE(TAG, "photo capture failed stage=job_allocate");
        ScreenShowToast(LangString_ASSISTANT_PHOTO_FAILED);
        return;
    }
    job->generation = s_generation.load();
    s_operation.store(ConversationPhotoOperation::Capture);
    s_photo_request = ConversationCapturePhoto(camera, LV_HOR_RES - UiSx(24),
        LV_VER_RES - ScreenStatusBarHeight() - UiSy(90), UiSx(124), UiSy(96),
        [job](const ConversationPhotoRequest& result) {
            job->photo = result->photo;
            job->apply = [](ScreenUiCompletion* completion) { ApplyCapture(completion); };
            ScreenCompleteWork(job);
        });
    if (!s_photo_request) {
        s_operation.store(ConversationPhotoOperation::Idle);
        delete job;
        ScreenShowToast(LangString_ASSISTANT_PHOTO_FAILED);
    } else {
        ScreenShowToast(LangString_SETTINGS_TEST_CAPTURING);
    }
}

static void StartUpload(const std::shared_ptr<ConversationPhoto>& photo, uint32_t turn, bool recorded) {
    auto* job = new (std::nothrow) AssistantBootPhotoUploadJob;
    if (job) {
        job->photo = photo;
        job->generation = s_generation.load();
        job->turn = turn;
        job->recorded = recorded;
    }
    s_operation.store(ConversationPhotoOperation::Upload);
    if (job) {
        s_photo_request = ConversationUploadPhoto(photo, [job](const ConversationPhotoRequest& result) {
            std::unique_ptr<AssistantBootPhotoUploadJob> owner(job);
            job->image_id = std::move(result->image_id);
            job->uploaded_at = result->uploaded_at;
            if (!job->image_id.empty() && !job->photo->history_added && !PrepareHistoryCache(*job)) {
                job->history_url.clear();
            }
            job->apply = [](ScreenUiCompletion* completion) { ApplyUpload(completion); };
            ScreenCompleteWork(owner.release());
        });
    }
    if (!job || !s_photo_request) {
        ESP_LOGE(TAG, "photo upload failed stage=submit result=unavailable");
        delete job;
        s_operation.store(ConversationPhotoOperation::Idle);
        ConversationController::GetInstance().CompleteQuestion(turn, false, "");
        ScreenShowToast(LangString_ASSISTANT_PHOTO_UPLOAD_FAILED);
        return;
    }
    ScreenShowToast(LangString_ASSISTANT_PHOTO_UPLOADING);
}

bool AssistantBootPhoto_OnDoubleClick() {
    if (!ConversationPresentation::IsActive() || ChatCloudActiveProvider() != ChatCloudProvider::Baiwen) {
        return false;
    }
    const auto generation = s_generation.load();
    return ScreenLvAsync(
        [](void* argument) {
            if (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(argument)) == s_generation.load() && s_screen) {
                RequestCapture();
            }
        },
        reinterpret_cast<void*>(static_cast<uintptr_t>(generation)));
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
    lv_obj_set_style_border_color(s_tray, UiColorForeground(), 0);
    lv_obj_set_style_bg_color(s_tray, UiColorBackground(), 0);
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
        if (s_operation.load() != ConversationPhotoOperation::Idle || s_preview || ConversationPresentation::IsPttHeld()) {
            ScreenShowToast(LangString_ASSISTANT_PHOTO_BUSY);
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

void AssistantPhoto_Reset() {
    AssistantPhoto_Suspend();
    if (s_thumbnail) {
        lv_image_set_src(s_thumbnail, nullptr);
    }
    ClearPending();
    s_recording_photo.reset();
    s_submitted_photo.reset();
    s_recording_turn = 0;
    s_screen = s_tray = s_thumbnail = nullptr;
}

void AssistantPhoto_Suspend() {
    s_generation.fetch_add(1);
    ConversationCancelPhoto(s_photo_request);
    s_photo_request.reset();
    s_operation.store(ConversationPhotoOperation::Idle);
    AssistantPhoto_ClosePreview();
}

bool AssistantPhoto_ClosePreview() {
    if (!s_preview) {
        return false;
    }
    lv_obj_delete(s_preview);
    s_preview = nullptr;
    return true;
}

bool AssistantPhoto_AllowListening() {
    if (s_preview) {
        ScreenShowToast(LangString_ASSISTANT_PHOTO_CLOSE);
        return false;
    }
    if (s_operation.load() == ConversationPhotoOperation::Capture) {
        ScreenShowToast(LangString_ASSISTANT_PHOTO_BUSY);
        return false;
    }
    return true;
}

bool AssistantPhoto_HasPending() {
    return s_pending != nullptr;
}

void AssistantPhoto_Send() {
    if (!s_pending || !ConversationPresentation::IsActive() || ChatCloudActiveProvider() != ChatCloudProvider::Baiwen) {
        return;
    }
    if (const char* hint = Board::GetInstance().GetNetworkErrorText(nullptr)) {
        ScreenShowToast(hint);
        return;
    }
    if (s_operation.load() != ConversationPhotoOperation::Idle || ConversationPresentation::IsPttHeld()) {
        ScreenShowToast(LangString_ASSISTANT_PHOTO_BUSY);
        return;
    }
    s_operation.store(ConversationPhotoOperation::Upload);
    if (!ConversationController::GetInstance().AskImage()) {
        s_operation.store(ConversationPhotoOperation::Idle);
        ScreenShowToast(LangString_ASSISTANT_PHOTO_QUESTION_FAILED);
    }
}

void AssistantPhoto_BeginListening(uint32_t turn) {
    s_submitted_photo.reset();
    s_recording_turn = turn;
    s_recording_photo = s_operation.load() == ConversationPhotoOperation::Idle ? s_pending : nullptr;
}

void AssistantPhoto_EndListening(uint32_t turn) {
    if (turn != s_recording_turn) {
        return;
    }
    auto photo = std::move(s_recording_photo);
    s_recording_turn = 0;
    if (photo && s_operation.load() == ConversationPhotoOperation::Idle) {
        StartUpload(photo, turn, true);
    } else if (photo) {
        // 松手与点击发送相邻发生时，由后来的单图发送接管，避免重复上传。
        ConversationController::GetInstance().CompleteQuestion(turn, false, "");
    } else if (!ConversationController::GetInstance().CompleteQuestion(turn, true, "")) {
        ScreenShowToast(LangString_ASSISTANT_PHOTO_QUESTION_FAILED);
    }
}

void AssistantPhoto_UploadQuestion(uint32_t turn, bool ready) {
    if (!ready || !s_pending || !s_screen || !ConversationPresentation::IsActive()) {
        s_operation.store(ConversationPhotoOperation::Idle);
        if (s_screen && ConversationPresentation::IsActive()) {
            ScreenShowToast(LangString_ASSISTANT_PHOTO_QUESTION_FAILED);
        }
        return;
    }
    StartUpload(s_pending, turn, false);
}

void AssistantPhoto_QuestionError(const std::string& image_id, bool reupload, bool disabled) {
    if (!s_submitted_photo || s_submitted_photo->image_id != image_id || !s_tray) {
        return;
    }
    if (!s_pending && s_operation.load() == ConversationPhotoOperation::Idle) {
        s_pending = std::move(s_submitted_photo);
        if (reupload) {
            s_pending->uploaded_at = 0;
        }
        lv_image_set_src(s_thumbnail, &s_pending->thumbnail.dsc);
        lv_obj_remove_flag(s_tray, LV_OBJ_FLAG_HIDDEN);
        Assistant_UpdatePhotoAction();
    }
    ScreenShowToast(disabled ? LangString_ASSISTANT_PHOTO_DISABLED : LangString_ASSISTANT_PHOTO_ANALYSIS_FAILED);
}

void AssistantPhoto_FinishQuestion() {
    s_submitted_photo.reset();
}
