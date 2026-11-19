#include <algorithm>
#include <atomic>
#include <vector>
#include <cstdio>
#include <memory>
#include <new>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "epd_content_policy.h"
#include "settings_test_camera.h"
#include "settings_test_common.h"
#include "settings_common.h"
#include "assets/lang_config.h"
#include "assets/lang_cellular.h"
#include "dvp_camera.h"
#include "display_orient.h"
#include "lv_adapter_epdiy.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "reader_types.h"
#include "ui_scale.h"

namespace {
constexpr const char* kTag = "SettingsTestCam";
std::atomic<bool> s_running{false};
bool s_busy = false, s_need_detect = false;
lv_obj_t* s_mask = nullptr;
lv_obj_t* s_image = nullptr;
lv_obj_t* s_photo_status = nullptr;
lv_obj_t* s_title = nullptr;
lv_obj_t* s_shot = nullptr;
lv_obj_t* s_close = nullptr;
lv_timer_t* s_timer = nullptr;

struct Job {
    uint32_t generation = 0;
    bool capture = false;
    esp_err_t error = ESP_FAIL;
    reader::RasterImage raster;
};
std::unique_ptr<Job> s_preview;
std::atomic<Job*> s_completed{nullptr};

bool Cancelled(void* context) {
    const auto* job = static_cast<const Job*>(context);
    return job->generation != SettingsTest_Generation() || !SettingsTest_IsLive();
}

void CloseOverlay() {
    if (s_mask && LvAdapterEpdiy::Instance()) LvAdapterEpdiy::Instance()->RequestFullOnNextCommit();
    if (s_image) lv_image_set_src(s_image, static_cast<const void*>(nullptr));
    s_image = nullptr;
    s_photo_status = s_title = s_shot = s_close = nullptr;
    if (s_mask) lv_obj_delete(s_mask);
    s_mask = nullptr;
    s_preview.reset();
}

void StartJob(bool capture);
void OnCaptureClicked(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    StartJob(true);
}
void OnCloseClicked(lv_event_t* event) {
    lv_event_stop_bubbling(event);
    if (!s_busy) CloseOverlay();
}

lv_obj_t* Button(lv_obj_t* parent, const char* text, lv_event_cb_t callback) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, UiSx(112), UiSy(40));
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, lv_color_white(), 0);
    lv_obj_set_style_border_color(button, lv_color_black(), 0);
    lv_obj_set_style_border_width(button, kSettingsBorderW, 0);
    lv_obj_set_style_radius(button, kSettingsOptionRadius, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    HapticAttachClick(button);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(label);
    return button;
}

void SetPhotoStatus(const char* text, bool busy) {
    if (!s_mask) return;
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
    if (LvAdapterEpdiy::Instance()) LvAdapterEpdiy::Instance()->RequestFullOnNextCommit();
}

void ShowOverlay(std::unique_ptr<Job> job) {
    if (!s_mask) {
        s_mask = lv_obj_create(SettingsTest_Ui().root_scr);
        lv_obj_remove_style_all(s_mask);
        lv_obj_add_flag(s_mask, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(s_mask, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_pos(s_mask, 0, 0);
        lv_obj_set_style_bg_color(s_mask, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(s_mask, LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_mask, LV_OBJ_FLAG_SCROLLABLE);
        s_title = lv_label_create(s_mask);
        lv_label_set_text(s_title, Lang::Strings::SETTINGS_TEST_CAMERA_RESULT);
        lv_obj_set_style_text_font(s_title, fontpack_lv_font_ui(), 0);
        lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, UiSy(14));
        s_image = lv_image_create(s_mask);
        lv_obj_set_size(s_image, 640, 480);
        lv_obj_set_style_image_opa(s_image, LV_OPA_COVER, 0);
        lv_obj_align(s_image, LV_ALIGN_CENTER, 0, -UiSy(24));
        s_photo_status = lv_label_create(s_mask);
        lv_obj_set_style_text_font(s_photo_status, fontpack_lv_font_ui(), 0);
        lv_obj_set_width(s_photo_status, LV_HOR_RES - UiSx(40));
        lv_obj_set_style_text_align(s_photo_status, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_photo_status, LV_ALIGN_CENTER, 0, -UiSy(24));
        s_shot = Button(s_mask, Lang::Strings::SETTINGS_TEST_CAMERA_SHOT, OnCaptureClicked);
        lv_obj_align_to(s_shot, s_image, LV_ALIGN_OUT_BOTTOM_MID, -UiSx(68), UiSy(24));
        s_close = Button(s_mask, Lang::Strings::SETTINGS_TEST_WIFI_CLOSE, OnCloseClicked);
        lv_obj_align_to(s_close, s_image, LV_ALIGN_OUT_BOTTOM_MID, UiSx(68), UiSy(24));
    }
    if (!job) {
        SetPhotoStatus(Lang::Strings::SETTINGS_TEST_CAPTURING, true);
        return;
    }
    // Detach the old descriptor before replacing its owning photo.
    lv_image_set_src(s_image, static_cast<const void*>(nullptr));
    s_preview = std::move(job);
    s_preview->raster.BindDsc();
    lv_image_set_src(s_image, &s_preview->raster.dsc);
    EpdSetContentRole(s_image,EpdContentRole::Photo);
    lv_obj_set_size(s_image, s_preview->raster.width, s_preview->raster.height);
    lv_obj_clear_flag(s_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_photo_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_state(s_shot, LV_STATE_DISABLED);
    lv_obj_remove_state(s_close, LV_STATE_DISABLED);
    lv_label_set_text_fmt(s_title, "%s  %u×%u", Lang::Strings::SETTINGS_TEST_CAMERA_RESULT,
                          s_preview->raster.width, s_preview->raster.height);
    ESP_LOGI(kTag, "photo overlay: image=%ux%u format=L8 native 16-gray full GC16 bytes=%u",
             s_preview->raster.width, s_preview->raster.height,
             unsigned(s_preview->raster.pixels.size()));
    if (LvAdapterEpdiy::Instance()) LvAdapterEpdiy::Instance()->RequestFullOnNextCommit();
}

void ApplyResult(Job* result) {
    std::unique_ptr<Job> job(result);
    if (Cancelled(job.get())) return;
    s_busy = false;
    auto& row = SettingsTest_Ui().camera;
    const bool pass = job->error == ESP_OK;
    if (!job->capture) {
        // Detection reports presence; an absent camera is not a failed test.
        lv_obj_add_flag(row.icon, LV_OBJ_FLAG_HIDDEN);
        SettingsTest_SetRowValue(row, pass ? Lang::Strings::SETTINGS_TEST_CAMERA_DETECTED
                                          : Lang::Strings::SETTINGS_TEST_CAMERA_NOT_DETECTED);
        return;
    }
    SettingsTest_SetRowStatus(row, pass);
    if (!pass) {
        char text[80];
        std::snprintf(text, sizeof(text), "%s (%s)", Lang::Strings::SETTINGS_TEST_FAIL,
                      esp_err_to_name(job->error));
        SettingsTest_SetRowValue(row, text);
        SetPhotoStatus(text, false);
        return;
    }
    char text[64];
    std::snprintf(text, sizeof(text), "%s %ux%u", Lang::Strings::SETTINGS_TEST_OK,
                  job->raster.width, job->raster.height);
    SettingsTest_SetRowValue(row, text);
    ShowOverlay(std::move(job));
}

void Worker(void* context) {
    std::unique_ptr<Job> job(static_cast<Job*>(context));
    dvp_camera::Request request{job->capture ? 20000u : 8000u, Cancelled, job.get()};
    if (job->capture) {
        dvp_camera::GrayImage gray;
        job->error = dvp_camera::Capture(gray, request);
        if (job->error == ESP_OK) {
            job->raster.width = gray.width;
            job->raster.height = gray.height;
            job->raster.pixels = std::move(gray.pixels);
        }
    } else {
        dvp_camera::DetectInfo info;
        job->error = dvp_camera::Detect(info, request);
    }
    ESP_LOGI(kTag, "%s result: %s", job->capture ? "capture" : "detect", esp_err_to_name(job->error));
    // Poll consumes results on the LVGL task. An adapter-lock timeout must
    // never turn successful detection into a failure or lose its completion.
    if (!Cancelled(job.get())) {
        const uint32_t generation = job->generation;
        delete s_completed.exchange(job.release());
        // Leaving may race publication. Drop a late result even if this page
        // is never opened again; the UI consumer uses the same atomic exchange.
        if (generation != SettingsTest_Generation() || !SettingsTest_IsLive())
            delete s_completed.exchange(nullptr);
    }
    s_running.store(false);
    vTaskDelete(nullptr);
}

void StartJob(bool capture) {
    if (!SettingsTest_IsLive() || s_busy) return;
    bool idle = false;
    if (!s_running.compare_exchange_strong(idle, true)) return;
    auto* job = new (std::nothrow) Job;
    if (!job) {
        s_running.store(false);
        SettingsTest_SetRowValue(SettingsTest_Ui().camera, Lang::Strings::BOOK_OUT_OF_MEMORY);
        if (capture) SettingsTest_SetRowStatus(SettingsTest_Ui().camera, false);
        else lv_obj_add_flag(SettingsTest_Ui().camera.icon, LV_OBJ_FLAG_HIDDEN);
        s_need_detect = false;
        return;
    }
    job->generation = SettingsTest_Generation();
    job->capture = capture;
    s_busy = true;
    if (capture) ShowOverlay(nullptr);
    s_need_detect = false;
    SettingsTest_SetRowValue(SettingsTest_Ui().camera, capture ? Lang::Strings::SETTINGS_TEST_CAPTURING
                                                            : Lang::Strings::SETTINGS_TEST_DETECTING);
    if (xTaskCreate(Worker, "test_cam", 10 * 1024, job, 5, nullptr) != pdPASS) {
        delete job;
        s_busy = false;
        s_running.store(false);
        SettingsTest_SetRowValue(SettingsTest_Ui().camera, Lang::Strings::SETTINGS_TEST_TASK_CREATE_FAIL);
        if (capture) SetPhotoStatus(Lang::Strings::SETTINGS_TEST_TASK_CREATE_FAIL, false);
        else lv_obj_add_flag(SettingsTest_Ui().camera.icon, LV_OBJ_FLAG_HIDDEN);
    }
}

void Poll(lv_timer_t*) {
    if (!SettingsTest_IsLive()) return;
    if (Job* result = s_completed.exchange(nullptr)) ApplyResult(result);
    // A previous page's worker may still be unwinding. Wait for its cleanup,
    // then detect for the new page; stale completion never changes its status.
    if (s_need_detect && !s_running.load()) StartJob(false);
}
}

void SettingsTestCamera_BuildRow(lv_obj_t* parent) {
    auto& row = SettingsTest_Ui().camera;
    row = SettingsTest_CreateRow(parent, Lang::Strings::SETTINGS_TEST_CAMERA, OnCaptureClicked);
    Button(row.row, Lang::Strings::SETTINGS_TEST_CAMERA_SHOT, OnCaptureClicked);
}

void SettingsTestCamera_OnLoad() {
    delete s_completed.exchange(nullptr);
    s_busy = false;
    s_need_detect = true;
    CloseOverlay();
    lv_obj_add_flag(SettingsTest_Ui().camera.icon, LV_OBJ_FLAG_HIDDEN);
    SettingsTest_SetRowValue(SettingsTest_Ui().camera, Lang::Strings::SETTINGS_TEST_DETECTING);
    s_timer = lv_timer_create(Poll, 100, nullptr);
    Poll(nullptr);
}

void SettingsTestCamera_Teardown() {
    // The shared auto-test session has already cancelled the worker.
    delete s_completed.exchange(nullptr);
    s_need_detect = false;
    s_busy = false;
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = nullptr;
    CloseOverlay();
}
