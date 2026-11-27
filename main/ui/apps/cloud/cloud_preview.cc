#include <new>
#include <string>
#include <utility>
#include <vector>
#include <esp_log.h>
#include "ui/apps/cloud/cloud_preview.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "business/reader/reader.h"
#include "business/reader/text_encoding.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/drawing/rendering/epd_content_policy.h"
#include "ui/common/drawing/ui_scale.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/widgets/screen_text.h"

static constexpr const char* TAG = "CloudPreview";
static constexpr lv_coord_t kPad = UiSx(12);
static constexpr lv_coord_t kRowBorderW = UiSx(2);
static constexpr lv_coord_t kRowRadius = UiSx(8);
static constexpr lv_coord_t kActionH = UiSy(56);
static constexpr int kPreviewMaxW = UiSx(240);
static constexpr int kPreviewMaxH = UiSy(400);
static constexpr lv_coord_t kChipRadius = 999;     // 预览元信息胶囊
static constexpr lv_coord_t kChipH = UiSy(36);     // 略高于字高，避免贴边
static constexpr lv_coord_t kChipPadH = UiSx(16);  // 左右内边距，边框大于文字
static constexpr lv_coord_t kPreviewGap = UiSy(12);
static constexpr lv_coord_t kPreviewImgMaxH = UiSy(420);  // 预览图框上限，避免占满整屏
static constexpr lv_coord_t kPreviewBodyPadTop = UiSy(8);
static constexpr lv_coord_t kPreviewBodyPadBot = UiSy(12);
static constexpr lv_coord_t kPreviewImgHostRadius = UiSx(16);
static constexpr lv_coord_t kPreviewMetaH = UiSy(40);
static constexpr lv_coord_t kPreviewMetaGap = UiSx(8);

static void DisableScroll(lv_obj_t* object) {
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(object, LV_SCROLLBAR_MODE_OFF);
}

CloudPreview::~CloudPreview() {
    Reset();
}

void CloudPreview::FillMeta(const CloudPushResource* item) {
    if (widgets_.meta == nullptr) {
        return;
    }
    lv_obj_clean(widgets_.meta);
    if (item == nullptr) {
        return;
    }
    auto make_chip = [&](const char* text) {
        lv_obj_t* chip = lv_obj_create(widgets_.meta);
        lv_obj_remove_style_all(chip);
        lv_obj_set_height(chip, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(chip, kChipH, 0);
        lv_obj_set_width(chip, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(chip, kChipPadH, 0);
        lv_obj_set_style_pad_ver(chip, 4, 0);
        lv_obj_set_style_border_width(chip, kRowBorderW, 0);
        lv_obj_set_style_border_color(chip, UiColorForeground(), 0);
        lv_obj_set_style_radius(chip, kChipRadius, 0);
        lv_obj_set_style_bg_color(chip, UiColorBackground(), 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(chip);
        lv_obj_t* lbl = lv_label_create(chip);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, UiFont(UiFontRole::Caption), 0);
        lv_obj_set_style_text_color(lbl, UiColorForeground(), 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    };
    make_chip(item->TypeLabel());
    make_chip(FormatCloudFileSize(item->file_size).c_str());
}

void CloudPreview::StyleSaveButton() {
    if (widgets_.download_btn == nullptr || widgets_.download_lbl == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(widgets_.download_btn, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(widgets_.download_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(widgets_.download_btn, UiColorForeground(), 0);
    lv_obj_set_style_border_width(widgets_.download_btn, kRowBorderW, 0);
    lv_obj_set_style_radius(widgets_.download_btn, kRowRadius, 0);
    lv_obj_set_style_text_color(widgets_.download_lbl, UiColorForeground(), 0);
    lv_obj_add_flag(widgets_.download_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_state(widgets_.download_btn, LV_STATE_DISABLED);
}

void CloudPreview::ClearImage() {
    if (widgets_.img != nullptr) {
        lv_image_set_src(widgets_.img, nullptr);
        lv_obj_add_flag(widgets_.img, LV_OBJ_FLAG_HIDDEN);
    }
    image_.reset();
}

void CloudPreview::OnSave(lv_event_t* event) {
    auto& owner = *static_cast<CloudPreview*>(lv_event_get_user_data(event));
    if (owner.open_ && owner.index_ >= 0 && owner.save_) {
        owner.save_(owner.index_);
    }
}

void CloudPreview::OnDeleted(lv_event_t* event) {
    auto& owner = *static_cast<CloudPreview*>(lv_event_get_user_data(event));
    if (lv_event_get_target(event) == owner.widgets_.body) {
        owner.Reset();
    }
}

void CloudPreview::SetStatus(const char* text) {
    if (widgets_.status) {
        lv_label_set_text(widgets_.status, text);
        lv_obj_clear_flag(widgets_.status, LV_OBJ_FLAG_HIDDEN);
    }
}

void CloudPreview::StartLoad() try {
    CancelPending();
    if (!input_.cache && input_.url.empty() && input_.path.empty()) {
        SetStatus(LangString_CLOUD_NO_COVER);
        return;
    }
    CloudImageItem input;
    input.index = index_;
    input.width = input_.width;
    input.height = input_.height;
    input.cache_app = input_.cache_app;
    input.url = input_.url;
    input.path = input_.path;
    input.cache = input_.cache;
    std::vector<CloudImageItem> items;
    items.push_back(std::move(input));
    request_ = CloudLoadImages(std::move(items));
    if (!request_) {
        SetStatus(LangString_CLOUD_PREVIEW_START_FAIL);
    }
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=load stage=submit result=no_memory");
    SetStatus(LangString_CLOUD_PREVIEW_START_FAIL);
    ScreenShowToast(LangString_COMMON_MEMORY_RETRY);
}

void CloudPreview::Build(lv_obj_t* scr, lv_coord_t status_height, lv_coord_t width, void (*save)(int)) {
    Reset();
    width_ = width;
    save_ = save;
    // 预览：图框 → 双行文件名 → 类型/大小胶囊 → 保存（对齐壁纸设置页）
    lv_obj_t* preview_body = lv_obj_create(scr);
    lv_obj_remove_style_all(preview_body);
    lv_obj_set_size(preview_body, LV_HOR_RES, LV_VER_RES - status_height);
    lv_obj_align(preview_body, LV_ALIGN_TOP_MID, 0, status_height);
    lv_obj_set_style_bg_opa(preview_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_hor(preview_body, kPad, 0);
    lv_obj_set_style_pad_top(preview_body, kPreviewBodyPadTop, 0);
    lv_obj_set_style_pad_bottom(preview_body, kPreviewBodyPadBot, 0);
    lv_obj_set_flex_flow(preview_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(preview_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(preview_body, kPreviewGap, 0);
    DisableScroll(preview_body);
    lv_obj_add_flag(preview_body, LV_OBJ_FLAG_HIDDEN);
    widgets_.body = preview_body;

    lv_obj_t* img_host = lv_obj_create(preview_body);
    lv_obj_remove_style_all(img_host);
    lv_obj_set_width(img_host, width_);
    lv_obj_set_flex_grow(img_host, 1);
    lv_obj_set_style_max_height(img_host, kPreviewImgMaxH, 0);
    lv_obj_set_style_bg_color(img_host, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(img_host, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(img_host, kRowBorderW, 0);
    lv_obj_set_style_border_color(img_host, UiColorForeground(), 0);
    lv_obj_set_style_radius(img_host, kPreviewImgHostRadius, 0);
    lv_obj_set_style_clip_corner(img_host, true, 0);
    DisableScroll(img_host);
    widgets_.img_host = img_host;

    lv_obj_t* img = lv_image_create(img_host);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    widgets_.img = img;

    lv_obj_t* preview_status = lv_label_create(img_host);
    lv_obj_set_style_text_font(preview_status, UiFont(), 0);
    lv_obj_set_style_text_color(preview_status, UiColorForeground(), 0);
    lv_obj_set_style_text_align(preview_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(preview_status, "");
    lv_obj_align(preview_status, LV_ALIGN_CENTER, 0, 0);
    widgets_.status = preview_status;

    lv_obj_t* preview_title = lv_label_create(preview_body);
    lv_obj_set_width(preview_title, width_);
    {
        const lv_font_t* f = UiFont(UiFontRole::Large);
        const lv_coord_t lh = (f != nullptr && f->line_height > 0) ? f->line_height : 29;
        lv_obj_set_height(preview_title, lh * 2);
    }
    lv_obj_set_style_text_font(preview_title, UiFont(UiFontRole::Large), 0);
    lv_obj_set_style_text_color(preview_title, UiColorForeground(), 0);
    lv_obj_set_style_text_align(preview_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(preview_title, 0, 0);
    // 文案已按像素拆成最多两行；CLIP 避免再走 LVGL 整词换行
    lv_label_set_long_mode(preview_title, LV_LABEL_LONG_CLIP);
    lv_label_set_text(preview_title, "");
    widgets_.title = preview_title;

    lv_obj_t* preview_meta = lv_obj_create(preview_body);
    lv_obj_remove_style_all(preview_meta);
    lv_obj_set_width(preview_meta, width_);
    lv_obj_set_height(preview_meta, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(preview_meta, kPreviewMetaH, 0);
    lv_obj_set_flex_flow(preview_meta, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(preview_meta, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(preview_meta, kPreviewMetaGap, 0);
    lv_obj_clear_flag(preview_meta, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(preview_meta);
    widgets_.meta = preview_meta;

    lv_obj_t* download_btn = lv_obj_create(preview_body);
    lv_obj_remove_style_all(download_btn);
    lv_obj_set_size(download_btn, width_, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(download_btn, kActionH, 0);
    lv_obj_set_style_pad_all(download_btn, 0, 0);
    lv_obj_set_flex_flow(download_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(download_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    DisableScroll(download_btn);
    HapticAttachClick(download_btn);
    lv_obj_add_event_cb(download_btn, OnSave, LV_EVENT_CLICKED, this);
    widgets_.download_btn = download_btn;

    lv_obj_t* download_lbl = lv_label_create(download_btn);
    lv_obj_set_style_text_font(download_lbl, UiFont(), 0);
    lv_label_set_text(download_lbl, LangString_CLOUD_SAVE_LOCAL);
    ScreenLayoutTextButton(download_btn, download_lbl, kActionH);
    widgets_.download_lbl = download_lbl;
    StyleSaveButton();
    lv_obj_add_event_cb(widgets_.body, OnDeleted, LV_EVENT_DELETE, this);
}

bool CloudPreview::Open(int index, const CloudPushResource& resource,
                        std::shared_ptr<HttpTempFile> cache, bool waiting) try {
    CloudImageItem input;
    input.index = index;
    input.width = kPreviewMaxW;
    input.height = kPreviewMaxH;
    input.cache_app = HttpCacheApp::Wallpaper;
    input.cache = std::move(cache);
    if (!input.cache) {
        input.url = resource.CoverThumbUrl();
        if (input.url.empty() && resource.type == PushResourceType::kBadge) {
            input.path = resource.LocalPath();
        }
    }
    const char* raw = !resource.resource_name.empty() ? resource.resource_name.c_str() : resource.name.c_str();
    const std::string stem = TitleFromPath(raw);
    const std::string shown = ScreenLayoutTitleTwoLines(stem.empty() ? raw : stem.c_str(),
                                                        UiFont(UiFontRole::Large), width_);
    Close();
    input_ = std::move(input);
    index_ = index;
    open_ = true;
    if (widgets_.title) {
        lv_label_set_text(widgets_.title, shown.c_str());
    }
    FillMeta(&resource);
    if (widgets_.body) {
        lv_obj_clear_flag(widgets_.body, LV_OBJ_FLAG_HIDDEN);
    }
    if (waiting) {
        PauseForTransfer();
    } else {
        Resume();
    }
    return true;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=open result=no_memory");
    Close();
    ScreenShowToast(LangString_COMMON_MEMORY_RETRY);
    return false;
}

void CloudPreview::Close() {
    CancelPending();
    ClearImage();
    open_ = false;
    index_ = -1;
    input_ = {};
    if (widgets_.body) {
        lv_obj_add_flag(widgets_.body, LV_OBJ_FLAG_HIDDEN);
    }
}

void CloudPreview::CancelPending() {
    CloudCancelImages(request_);
    request_.reset();
}

void CloudPreview::PauseForTransfer() {
    CancelPending();
    ClearImage();
    if (open_) {
        SetStatus(LangString_CLOUD_WAIT_DOWNLOAD);
    }
}

void CloudPreview::Resume() {
    if (!open_ || index_ < 0) {
        return;
    }
    SetStatus(LangString_CLOUD_LOADING);
    StartLoad();
}

void CloudPreview::Poll() {
    if (!request_ || !request_->completed.load(std::memory_order_acquire)) {
        return;
    }
    const auto request = std::move(request_);
    if (!open_ || request->gate.IsCancelled() || request->items.empty() || request->items[0].index != index_) {
        return;
    }
    auto& item = request->items[0];
    ClearImage();
    if (!item.image || item.image->empty()) {
        SetStatus(LangString_CLOUD_PREVIEW_FAIL);
        return;
    }
    input_.cache = std::move(item.cache);
    image_ = std::move(item.image);
    image_->BindDsc();
    if (widgets_.img) {
        lv_image_set_scale(widgets_.img, LV_SCALE_NONE);
        lv_image_set_src(widgets_.img, &image_->dsc);
        EpdSetContentRole(widgets_.img, EpdContentRole::WallpaperPreview);
        lv_obj_set_size(widgets_.img, image_->width, image_->height);
        lv_obj_clear_flag(widgets_.img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_center(widgets_.img);
        lv_obj_invalidate(widgets_.img);
        if (widgets_.img_host) {
            lv_obj_invalidate(widgets_.img_host);
        }
    }
    if (widgets_.status) {
        lv_obj_add_flag(widgets_.status, LV_OBJ_FLAG_HIDDEN);
    }
}

void CloudPreview::Reset() {
    Close();
    if (widgets_.body) {
        lv_obj_remove_event_cb_with_user_data(widgets_.body, OnDeleted, this);
    }
    widgets_ = {};
    save_ = nullptr;
    width_ = 0;
}

void CloudPreview::OnItemErased(int index) {
    if (index_ == index) {
        CancelPending();
        index_ = -1;
    } else if (index_ > index) {
        CancelPending();
        --index_;
    }
}
