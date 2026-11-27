#include <memory>
#include "settings_test_aging_sd.h"
#include "ui/common/widgets/storage_error_text.h"
#include "assets/lang_config.h"
#include "business/diagnostics/aging_test.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/drawing/rendering/epd_content_policy.h"
#include "ui/common/drawing/ui_scale.h"
#include "ui/common/drawing/ui_theme.h"

static lv_obj_t* s_image = nullptr;
static lv_obj_t* s_notice = nullptr;
static int s_image_side = 0;
static std::shared_ptr<const AgingSdResult> s_pixels;

static const char* AgingSdStatusText(const AgingSdResult& result) {
    if (result.error) {
        return StorageErrorText(result.error);
    }
    switch (result.decode_error) {
        case ImageDecodeError::kInvalidData: return LangString_SETTINGS_TEST_AGING_SD_IMAGE_INVALID;
        case ImageDecodeError::kUnsupported: return LangString_SETTINGS_TEST_AGING_SD_IMAGE_UNSUPPORTED;
        case ImageDecodeError::kNoMemory: return LangString_SETTINGS_TEST_AGING_SD_MEMORY;
        case ImageDecodeError::kFileIo: return LangString_SETTINGS_TEST_AGING_SD_FILE_READ;
        case ImageDecodeError::kFailed: return LangString_SETTINGS_TEST_AGING_SD_DECODE;
        default: return LangString_SETTINGS_TEST_AGING_NO_WALLPAPER;
    }
}

lv_obj_t* SettingsTestAgingSd_Build(lv_obj_t* parent, lv_coord_t side, int& preview_side) {
    SettingsTestAgingSd_Reset();
    auto* frame = lv_obj_create(parent);
    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, side, side);
    lv_obj_set_style_border_width(frame, UiSx(2), 0);
    lv_obj_set_style_border_color(frame, UiColorForeground(), 0);
    lv_obj_set_style_bg_color(frame, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(frame, UiSx(8), 0);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_CLICKABLE);
    s_image_side = side - 2 * (UiSx(8) + UiSx(2));
    preview_side = s_image_side;
    s_image = lv_image_create(frame);
    EpdSetContentRole(s_image, EpdContentRole::Photo);
    lv_obj_clear_flag(s_image, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_image, LV_OBJ_FLAG_HIDDEN);
    s_notice = lv_label_create(frame);
    lv_obj_set_width(s_notice, s_image_side);
    lv_label_set_long_mode(s_notice, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_notice, UiFont(), 0);
    lv_obj_set_style_text_color(s_notice, UiColorForeground(), 0);
    lv_obj_set_style_text_align(s_notice, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_notice, LangString_WALLPAPER_ENTER_LOADING);
    lv_obj_center(s_notice);
    return frame;
}

void SettingsTestAgingSd_Show(const std::shared_ptr<const AgingSdResult>& result) {
    if (!s_image || !s_notice || result == s_pixels || !result) {
        return;
    }
    lv_image_set_src(s_image, static_cast<const void*>(nullptr));
    s_pixels = result;
    if (result->status == AgingSdStatus::kReady && !result->image.empty()) {
        lv_image_set_src(s_image, &result->image.dsc);
        lv_obj_set_size(s_image, result->image.width, result->image.height);
        lv_obj_center(s_image);
        lv_obj_clear_flag(s_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_notice, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_image, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_notice, AgingSdStatusText(*result));
        lv_obj_clear_flag(s_notice, LV_OBJ_FLAG_HIDDEN);
        lv_obj_center(s_notice);
    }
}

void SettingsTestAgingSd_Reset() {
    if (s_image) {
        lv_image_set_src(s_image, static_cast<const void*>(nullptr));
    }
    s_image = s_notice = nullptr;
    s_pixels.reset();
}
