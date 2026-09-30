#include "wallpaper_screen/preview/wallpaper_adjust.h"
#include "wallpaper_screen/wallpaper_screen_priv.h"
#include "wallpaper_screen/wallpaper_ui_helpers.h"

#include <lvgl.h>
#include <cstdint>
#include <cstring>
#include <utility>

#include "assets/lang_config.h"
#include "haptic_feedback.h"
#include "reader/reader.h"

void ResetOrientState() {
    Wallpaper_State().preview.rot_cw = 0;
    Wallpaper_State().preview.mirror_h = false;
}

bool AdjustDirty() {
    return Wallpaper_State().preview.rot_cw != 0 || Wallpaper_State().preview.mirror_h;
}

// 顺时针 90°；dst 宽高互换
void RotateCw90(const reader::RasterImage& src, reader::RasterImage& dst) {
    const int w = src.width;
    const int h = src.height;
    dst.pixels.assign(static_cast<size_t>(h) * static_cast<size_t>(w), 0xFF);
    dst.width = static_cast<uint16_t>(h);
    dst.height = static_cast<uint16_t>(w);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // dst(h-1-y, x) = src(x,y)
            const int dx = h - 1 - y;
            const int dy = x;
            dst.pixels[static_cast<size_t>(dy) * static_cast<size_t>(h) + static_cast<size_t>(dx)] =
                src.pixels[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
        }
    }
    dst.BindDsc();
}

// 若对边都有明显白边（旧版 90° 烤进去的留白），裁到墨迹包围盒，便于再次旋转铺满
void TrimBakedLetterbox(reader::RasterImage& img) {
    if (img.empty()) {
        return;
    }
    const int w = img.width;
    const int h = img.height;
    int min_x = w;
    int min_y = h;
    int max_x = -1;
    int max_y = -1;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = img.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
        for (int x = 0; x < w; ++x) {
            if (row[x] >= 0xF0) {
                continue;
            }
            if (x < min_x) {
                min_x = x;
            }
            if (y < min_y) {
                min_y = y;
            }
            if (x > max_x) {
                max_x = x;
            }
            if (y > max_y) {
                max_y = y;
            }
        }
    }
    if (max_x < 0) {
        return;
    }
    const int top = min_y;
    const int bot = h - 1 - max_y;
    const int left = min_x;
    const int right = w - 1 - max_x;
    const bool vert_box = top > h / 20 && bot > h / 20;
    const bool horz_box = left > w / 20 && right > w / 20;
    if (!vert_box && !horz_box) {
        return;
    }
    const int nw = max_x - min_x + 1;
    const int nh = max_y - min_y + 1;
    if (nw >= w && nh >= h) {
        return;
    }
    reader::RasterImage cropped;
    cropped.pixels.resize(static_cast<size_t>(nw) * static_cast<size_t>(nh));
    cropped.width = static_cast<uint16_t>(nw);
    cropped.height = static_cast<uint16_t>(nh);
    for (int y = 0; y < nh; ++y) {
        std::memcpy(cropped.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(nw),
                    img.pixels.data() + static_cast<size_t>(min_y + y) * static_cast<size_t>(w) +
                        static_cast<size_t>(min_x),
                    static_cast<size_t>(nw));
    }
    img = std::move(cropped);
    img.BindDsc();
}

// 朝向：先水平镜像，再顺时针 rot_cw×90°。 90°/270° 后宽高对调，不把留白烤进像素——预览/待机靠白底居中露出空白；
void ApplyWallpaperOrient(const reader::RasterImage& src, uint8_t rot_cw, bool mirror_h,
                          reader::RasterImage& out) {
    if (src.empty()) {
        out.Reset();
        return;
    }
    rot_cw = static_cast<uint8_t>(rot_cw & 3u);

    reader::RasterImage work = src;
    if (mirror_h) {
        reader::RasterImage mirrored;
        mirrored.pixels.resize(work.pixels.size());
        mirrored.width = work.width;
        mirrored.height = work.height;
        for (int y = 0; y < work.height; ++y) {
            for (int x = 0; x < work.width; ++x) {
                mirrored.pixels[static_cast<size_t>(y) * work.width + static_cast<size_t>(x)] =
                    work.pixels[static_cast<size_t>(y) * work.width +
                                static_cast<size_t>(work.width - 1 - x)];
            }
        }
        work = std::move(mirrored);
        work.BindDsc();
    }

    if (rot_cw == 0) {
        if (&out != &src || mirror_h) {
            out = std::move(work);
            out.BindDsc();
        }
        return;
    }

    reader::RasterImage rotated = std::move(work);
    for (uint8_t i = 0; i < rot_cw; ++i) {
        reader::RasterImage next;
        RotateCw90(rotated, next);
        rotated = std::move(next);
    }
    out = std::move(rotated);
    out.BindDsc();
}

void BindPreviewRasterToImg() {
    if (Wallpaper_State().preview.raster == nullptr || Wallpaper_State().ui.preview_img == nullptr) {
        return;
    }
    Wallpaper_State().preview.raster->BindDsc();
    lv_image_set_scale(Wallpaper_State().ui.preview_img, LV_SCALE_NONE);
    lv_image_set_src(Wallpaper_State().ui.preview_img, &Wallpaper_State().preview.raster->dsc);
    lv_obj_set_size(Wallpaper_State().ui.preview_img, Wallpaper_State().preview.raster->width, Wallpaper_State().preview.raster->height);
    lv_obj_clear_flag(Wallpaper_State().ui.preview_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(Wallpaper_State().ui.preview_img);
    lv_obj_invalidate(Wallpaper_State().ui.preview_img);
    if (Wallpaper_State().ui.preview_img_host != nullptr) {
        lv_obj_invalidate(Wallpaper_State().ui.preview_img_host);
    }
}

void RefreshPreviewFromBase() {
    if (Wallpaper_State().preview.base == nullptr || Wallpaper_State().preview.base->empty()) {
        return;
    }
    if (Wallpaper_State().preview.raster == nullptr) {
        Wallpaper_State().preview.raster = new reader::RasterImage();
    }
    ApplyWallpaperOrient(*Wallpaper_State().preview.base, Wallpaper_State().preview.rot_cw, Wallpaper_State().preview.mirror_h, *Wallpaper_State().preview.raster);
    BindPreviewRasterToImg();
}

void UpdateAdjustUi() {
    if (!Wallpaper_State().screen_alive) {
        return;
    }
    const bool show_normal = !Wallpaper_State().preview.adjust_open;
    auto set_hidden = [](lv_obj_t* obj, bool hidden) {
        if (obj == nullptr) {
            return;
        }
        if (hidden) {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    };
    // 两行槽位高度固定；只切换槽内整行，避免预览框被挤动
    set_hidden(Wallpaper_State().ui.actions_normal_row, !show_normal);
    set_hidden(Wallpaper_State().ui.actions_adjust_row, show_normal);
    set_hidden(Wallpaper_State().ui.delete_btn, !show_normal);
    set_hidden(Wallpaper_State().ui.save_row, show_normal);

    const bool can_enter = show_normal && Wallpaper_State().preview.base != nullptr && !Wallpaper_State().preview.base->empty() &&
                           !Wallpaper_State().workers.load_busy.load() && !Wallpaper_State().workers.save_busy.load() && !Wallpaper_State().workers.delete_busy.load() &&
                           !Wallpaper_State().workers.enable_busy.load();
    StyleActionBtn(Wallpaper_State().ui.adjust_btn, Wallpaper_State().ui.adjust_lbl, false, can_enter);

    const bool xform_ok = Wallpaper_State().preview.adjust_open && !Wallpaper_State().workers.save_busy.load();
    StyleActionBtn(Wallpaper_State().ui.rotate_left_btn, nullptr, false, xform_ok);
    StyleActionBtn(Wallpaper_State().ui.rotate_right_btn, nullptr, false, xform_ok);
    StyleActionBtn(Wallpaper_State().ui.mirror_btn, nullptr, false, xform_ok);
    StyleActionBtn(Wallpaper_State().ui.cancel_btn, nullptr, false, xform_ok);
    if (Wallpaper_State().ui.save_btn != nullptr && Wallpaper_State().ui.save_lbl != nullptr) {
        StyleActionBtn(Wallpaper_State().ui.save_btn, Wallpaper_State().ui.save_lbl, true, xform_ok);
    }
}

void CloseOverwriteDialog() {
    if (Wallpaper_State().ui.dialog_mask != nullptr && Wallpaper_State().screen_alive) {
        lv_obj_delete(Wallpaper_State().ui.dialog_mask);
    }
    Wallpaper_State().ui.dialog_mask = nullptr;
}

void ExitAdjustMode(bool restore_preview) {
    CloseOverwriteDialog();
    if (!Wallpaper_State().preview.adjust_open) {
        UpdateAdjustUi();
        return;
    }
    Wallpaper_State().preview.adjust_open = false;
    ResetOrientState();
    if (restore_preview) {
        RefreshPreviewFromBase();
    }
    UpdateEnableButtonUi();
    UpdateDeleteButtonUi();
    UpdateAdjustUi();
}

void EnterAdjustMode() {
    if (Wallpaper_State().mode != UiMode::kPreview || Wallpaper_State().preview.adjust_open || Wallpaper_State().workers.save_busy.load()) {
        return;
    }
    if (Wallpaper_State().preview.base == nullptr || Wallpaper_State().preview.base->empty() || Wallpaper_State().workers.load_busy.load() ||
        Wallpaper_State().workers.delete_busy.load() || Wallpaper_State().workers.enable_busy.load()) {
        return;
    }
    ResetOrientState();
    Wallpaper_State().preview.adjust_open = true;
    RefreshPreviewFromBase();
    UpdateEnableButtonUi();
    UpdateDeleteButtonUi();
    UpdateAdjustUi();
}

void OnAdjustClicked(lv_event_t* /*e*/) {
    EnterAdjustMode();
}

void OnAdjustCancelClicked(lv_event_t* /*e*/) {
    if (Wallpaper_State().workers.save_busy.load()) {
        return;
    }
    ExitAdjustMode(true);
}

void OnRotateLeftClicked(lv_event_t* /*e*/) {
    if (!Wallpaper_State().preview.adjust_open || Wallpaper_State().workers.save_busy.load()) {
        return;
    }
    Wallpaper_State().preview.rot_cw = static_cast<uint8_t>((Wallpaper_State().preview.rot_cw + 3) & 3);
    RefreshPreviewFromBase();
}

void OnRotateRightClicked(lv_event_t* /*e*/) {
    if (!Wallpaper_State().preview.adjust_open || Wallpaper_State().workers.save_busy.load()) {
        return;
    }
    Wallpaper_State().preview.rot_cw = static_cast<uint8_t>((Wallpaper_State().preview.rot_cw + 1) & 3);
    RefreshPreviewFromBase();
}

void OnMirrorHClicked(lv_event_t* /*e*/) {
    if (!Wallpaper_State().preview.adjust_open || Wallpaper_State().workers.save_busy.load()) {
        return;
    }
    // 对当前画面水平镜像：H∘R^r∘H^m = R^{-r}∘H^{m+1}
    Wallpaper_State().preview.rot_cw = static_cast<uint8_t>((4 - Wallpaper_State().preview.rot_cw) & 3);
    Wallpaper_State().preview.mirror_h = !Wallpaper_State().preview.mirror_h;
    RefreshPreviewFromBase();
}

void OnSaveAsClicked(lv_event_t* /*e*/) {
    CloseOverwriteDialog();
    ScheduleSaveTransform(true);
}

void OnOverwriteClicked(lv_event_t* /*e*/) {
    CloseOverwriteDialog();
    ScheduleSaveTransform(false);
}

void OnSaveCancelClicked(lv_event_t* /*e*/) {
    CloseOverwriteDialog();
}

void ShowOverwriteDialog() {
    if (!Wallpaper_State().screen_alive || Wallpaper_State().ui.dialog_mask != nullptr || Wallpaper_State().workers.save_busy.load()) {
        return;
    }
    const lv_coord_t card_w = LV_HOR_RES - 48;
    const lv_coord_t text_w = card_w - 28;
    const lv_coord_t third_w = (text_w - 16) / 3;

    lv_obj_t* mask = lv_obj_create(Wallpaper_State().ui.screen);
    Wallpaper_State().ui.dialog_mask = mask;
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(mask, 0, 0);
    ScreenApplyDotBackdrop(mask);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(mask);
    lv_obj_add_event_cb(mask, OnSaveCancelClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, card_w, 180);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, kBorderW, 0);
    lv_obj_set_style_radius(card, kBtnRadius > 24 ? 16 : kBtnRadius, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, [](lv_event_t* e) { lv_event_stop_bubbling(e); }, LV_EVENT_CLICKED,
                        nullptr);

    // 文案槽：控件本身居中（本板 text_align 对部分字体不生效）
    lv_obj_t* title_slot = lv_obj_create(card);
    lv_obj_remove_style_all(title_slot);
    lv_obj_set_width(title_slot, text_w);
    lv_obj_set_flex_grow(title_slot, 1);
    lv_obj_set_flex_flow(title_slot, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(title_slot, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(title_slot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(title_slot, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* lbl = lv_label_create(title_slot);
    lv_label_set_text(lbl, Lang::Strings::WALLPAPER_SAVE);
    lv_obj_set_style_text_font(lbl, Wallpaper_UiFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* btn_row = lv_obj_create(card);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, text_w);
    lv_obj_set_height(btn_row, kPreviewBtnRowH);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_CLICKABLE);

    auto make_btn = [&](const char* text, bool filled, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_obj_create(btn_row);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, third_w, 44);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(btn);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* bl = lv_label_create(btn);
        lv_label_set_text(bl, text);
        lv_obj_set_style_text_font(bl, Wallpaper_UiFont(), 0);
        lv_obj_center(bl);
        lv_obj_clear_flag(bl, LV_OBJ_FLAG_CLICKABLE);
        StyleActionBtn(btn, bl, filled, true);
    };
    make_btn(Lang::Strings::WALLPAPER_SAVE_AS, true, OnSaveAsClicked);
    make_btn(Lang::Strings::WALLPAPER_OVERWRITE, false, OnOverwriteClicked);
    make_btn(Lang::Strings::COMMON_CANCEL, false, OnSaveCancelClicked);
}

void OnSaveClicked(lv_event_t* /*e*/) {
    if (!Wallpaper_State().preview.adjust_open || Wallpaper_State().workers.save_busy.load()) {
        return;
    }
    if (!AdjustDirty()) {
        return;
    }
    ShowOverwriteDialog();
}

