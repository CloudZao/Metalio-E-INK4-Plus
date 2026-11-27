#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>
#include <esp_log.h>
#include "ui/apps/settings/settings_test/settings_test_camera.h"
#include "assets/lang_cellular.h"
#include "assets/lang_config.h"
#include "business/reader/reader_types.h"
#include "ui/apps/settings/settings_test/settings_test_common.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "business/diagnostics/camera_diagnostic.h"
#include "ui/common/runtime/page_lifecycle.h"
#include "ui/common/drawing/display_orient.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/drawing/rendering/epd_content_policy.h"
#include "ui/common/widgets/screen_text.h"
#include "ui/apps/settings/settings_common.h"
#include "ui/common/drawing/ui_layout.h"
#include "ui/common/drawing/ui_scale.h"
#include "ui/common/drawing/ui_theme.h"

static constexpr const char* kTag = "SettingsTestCam";
static bool s_busy = false, s_need_detect = false;
static lv_obj_t* s_mask = nullptr;
static lv_obj_t* s_image = nullptr;
static lv_obj_t* s_photo_status = nullptr;
static lv_obj_t* s_title = nullptr;
static lv_obj_t* s_shot = nullptr;
static lv_obj_t* s_close = nullptr;
static UiPageLifecycle s_camera_lifecycle;
static CameraDiagnosticRequest s_request, s_preview;

static void CloseOverlay() {
    if (s_mask && LVAdapterDisplay::Instance()) {
        LVAdapterDisplay::Instance()->RequestFullOnNextCommit();
    }
    if (s_image) {
        lv_image_set_src(s_image, static_cast<const void*>(nullptr));
    }
    s_image = nullptr;
    s_photo_status = s_title = s_shot = s_close = nullptr;
    if (s_mask) {
        lv_obj_delete(s_mask);
    }
    s_mask = nullptr;
    s_preview.reset();
}

static void StartJob(bool capture);

static void OnCaptureClicked(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    StartJob(true);
}

static void OnCloseClicked(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    if (!s_busy) {
        CloseOverlay();
    }
}

static lv_obj_t* Button(lv_obj_t* parent, const char* text, lv_event_cb_t callback) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, std::min<int>(UiSx(112), (LV_HOR_RES - UiSx(48)) / 2),
                    std::max<int>(UiSy(40), UiFont()->line_height + UiSy(12)));
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, UiColorBackground(), 0);
    lv_obj_set_style_border_color(button, UiColorForeground(), 0);
    lv_obj_set_style_border_width(button, kScreenBorderW, 0);
    lv_obj_set_style_radius(button, kScreenOptionRadius, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    HapticAttachClick(button);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, UiFont(), 0);
    lv_obj_set_style_text_color(label, UiColorForeground(), 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, ScreenFitButtonLabelOnSize, LV_EVENT_SIZE_CHANGED, label);
    return button;
}

static void LayoutPhoto() {
    const int pad = UiSx(16);
    lv_obj_set_width(s_title, LV_HOR_RES - 2 * pad);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_WRAP);
    lv_obj_update_layout(s_mask);
    const int button_h = lv_obj_get_height(s_shot);
    const int top = lv_obj_get_y(s_title) + lv_obj_get_height(s_title) + pad;
    const int available_h = std::max<int>(1, LV_VER_RES - top - button_h - 2 * pad);
    const int source_w = s_preview ? s_preview->raster.width : 640;
    const int source_h = s_preview ? s_preview->raster.height : 480;
    const auto image_size =
        UiLayoutFitImage({static_cast<int>(LV_HOR_RES) - 2 * pad, available_h}, {source_w, source_h});
    const int width = image_size.width, height = image_size.height;
    lv_obj_set_size(s_image, width, height);
    lv_image_set_inner_align(s_image, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_set_pos(s_image, (LV_HOR_RES - width) / 2, top + (available_h - height) / 2);
    lv_obj_align(s_photo_status, LV_ALIGN_TOP_MID, 0, top + available_h / 2);
    lv_obj_align(s_shot, LV_ALIGN_BOTTOM_MID, -LV_HOR_RES / 4, -pad);
    lv_obj_align(s_close, LV_ALIGN_BOTTOM_MID, LV_HOR_RES / 4, -pad);
}

static void SetPhotoStatus(const char* text, bool busy) {
    if (!s_mask) {
        return;
    }
    lv_label_set_text(s_photo_status, text);
    lv_obj_clear_flag(s_photo_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_image, LV_OBJ_FLAG_HIDDEN);
    if (busy) {
        lv_obj_add_state(s_shot, LV_STATE_DISABLED);
        lv_obj_add_state(s_close, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(s_shot, LV_STATE_DISABLED);
        lv_obj_remove_state(s_close, LV_STATE_DISABLED);
    }
    if (LVAdapterDisplay::Instance()) {
        LVAdapterDisplay::Instance()->RequestFullOnNextCommit();
    }
}

static void ShowOverlay(CameraDiagnosticRequest job) {
    if (!s_mask) {
        s_mask = lv_obj_create(SettingsTest_Ui().root_scr);
        lv_obj_remove_style_all(s_mask);
        lv_obj_add_flag(s_mask, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(s_mask, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_pos(s_mask, 0, 0);
        lv_obj_set_style_bg_color(s_mask, UiColorBackground(), 0);
        lv_obj_set_style_bg_opa(s_mask, LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_mask, LV_OBJ_FLAG_SCROLLABLE);
        s_title = lv_label_create(s_mask);
        lv_label_set_text(s_title, LangString_SETTINGS_TEST_CAMERA_RESULT);
        lv_obj_set_style_text_font(s_title, UiFont(), 0);
        lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, UiSy(14));
        s_image = lv_image_create(s_mask);
        lv_obj_set_style_image_opa(s_image, LV_OPA_COVER, 0);
        lv_obj_align(s_image, LV_ALIGN_CENTER, 0, -UiSy(24));
        s_photo_status = lv_label_create(s_mask);
        lv_obj_set_style_text_font(s_photo_status, UiFont(), 0);
        lv_obj_set_width(s_photo_status, LV_HOR_RES - UiSx(40));
        lv_obj_set_style_text_align(s_photo_status, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_photo_status, LV_ALIGN_CENTER, 0, -UiSy(24));
        s_shot = Button(s_mask, LangString_SETTINGS_TEST_CAMERA_SHOT, OnCaptureClicked);
        s_close = Button(s_mask, LangString_SETTINGS_TEST_WIFI_CLOSE, OnCloseClicked);
        LayoutPhoto();
    }
    if (!job) {
        SetPhotoStatus(LangString_SETTINGS_TEST_CAPTURING, true);
        return;
    }
    // Detach the old descriptor before replacing its owning photo.
    lv_image_set_src(s_image, static_cast<const void*>(nullptr));
    s_preview = std::move(job);
    s_preview->raster.BindDsc();
    lv_image_set_src(s_image, &s_preview->raster.dsc);
    EpdSetContentRole(s_image, EpdContentRole::Photo);
    lv_obj_clear_flag(s_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_photo_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_state(s_shot, LV_STATE_DISABLED);
    lv_obj_remove_state(s_close, LV_STATE_DISABLED);
    lv_label_set_text_fmt(s_title, "%s  %u×%u", LangString_SETTINGS_TEST_CAMERA_RESULT, s_preview->raster.width,
                          s_preview->raster.height);
    LayoutPhoto();
    ESP_LOGI(kTag, "photo overlay: image=%ux%u format=L8 bytes=%u", s_preview->raster.width, s_preview->raster.height,
             unsigned(s_preview->raster.pixels.size()));
    if (LVAdapterDisplay::Instance()) {
        LVAdapterDisplay::Instance()->RequestFullOnNextCommit();
    }
}

static void ApplyResult(CameraDiagnosticRequest job) {
    if (!SettingsTest_IsLive()) {
        return;
    }
    s_busy = false;
    auto& row = SettingsTest_Ui().camera;
    const bool pass = job->error == ESP_OK;
    if (!job->capture) {
        // Detection reports presence; an absent camera is not a failed test.
        lv_obj_add_flag(row.icon, LV_OBJ_FLAG_HIDDEN);
        SettingsTest_SetRowValue(
            row, pass ? LangString_SETTINGS_TEST_CAMERA_DETECTED : LangString_SETTINGS_TEST_CAMERA_NOT_DETECTED);
        return;
    }
    SettingsTest_SetRowStatus(row, pass);
    if (!pass) {
        char text[80];
        std::snprintf(text, sizeof(text), "%s (%s)", LangString_SETTINGS_TEST_FAIL, esp_err_to_name(job->error));
        SettingsTest_SetRowValue(row, text);
        SetPhotoStatus(text, false);
        return;
    }
    char text[64];
    std::snprintf(text, sizeof(text), "%s %ux%u", LangString_SETTINGS_TEST_OK, job->raster.width, job->raster.height);
    SettingsTest_SetRowValue(row, text);
    ShowOverlay(std::move(job));
}

static void StartJob(bool capture) {
    if (!SettingsTest_IsLive() || s_busy) {
        return;
    }
    s_request = CameraDiagnosticStart(capture, DisplayUiLogW() - 32, DisplayUiLogH() - 200);
    if (!s_request) {
        if (capture) {
            SettingsTest_SetRowValue(SettingsTest_Ui().camera, LangString_SETTINGS_TEST_TASK_CREATE_FAIL);
        }
        return;
    }
    s_busy = true;
    s_need_detect = false;
    if (capture) {
        ShowOverlay(nullptr);
    }
    SettingsTest_SetRowValue(SettingsTest_Ui().camera,
                             capture ? LangString_SETTINGS_TEST_CAPTURING : LangString_SETTINGS_TEST_DETECTING);
}

static void Poll() {
    if (!SettingsTest_IsLive()) {
        return;
    }
    if (s_request && s_request->completed.load(std::memory_order_acquire)) {
        ApplyResult(std::move(s_request));
    }
    // A previous page's worker may still be unwinding. Wait for its cleanup,
    // then detect for the new page; stale completion never changes its status.
    if (s_need_detect && !s_busy) {
        StartJob(false);
    }
}

void SettingsTestCamera_BuildRow(lv_obj_t* parent) {
    auto& row = SettingsTest_Ui().camera;
    row = SettingsTest_CreateRow(parent, LangString_SETTINGS_TEST_CAMERA, OnCaptureClicked);
    Button(row.row, LangString_SETTINGS_TEST_CAMERA_SHOT, OnCaptureClicked);
}

void SettingsTestCamera_OnLoad() {
    CameraDiagnosticCancel(s_request);
    s_request.reset();
    s_busy = false;
    s_need_detect = true;
    CloseOverlay();
    lv_obj_add_flag(SettingsTest_Ui().camera.icon, LV_OBJ_FLAG_HIDDEN);
    SettingsTest_SetRowValue(SettingsTest_Ui().camera, LangString_SETTINGS_TEST_DETECTING);
    s_camera_lifecycle.Bind(SettingsTest_Ui().root_scr, {Poll, [](UiPagePowerEvent event) {
        if (event == UiPagePowerEvent::PauseApplications) {
            SettingsTestCamera_Teardown();
        } else if (event == UiPagePowerEvent::ResumeApplications) {
            SettingsTestCamera_OnLoad();
        }
    }, nullptr, 100});
    Poll();
}

void SettingsTestCamera_Teardown() {
    // 先作废业务接收，再解除预览控件引用。
    CameraDiagnosticCancel(s_request);
    s_request.reset();
    s_need_detect = false;
    s_busy = false;
    CloseOverlay();
}
