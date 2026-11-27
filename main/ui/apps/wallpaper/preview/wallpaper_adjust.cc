#include <cstdint>
#include <lvgl.h>
#include "ui/apps/wallpaper/preview/wallpaper_adjust.h"
#include "assets/lang_config.h"
#include "business/reader/reader.h"
#include "ui/common/drawing/rendering/epd_content_policy.h"
#include "ui/apps/wallpaper/wallpaper_screen_priv.h"
#include "ui/apps/wallpaper/wallpaper_ui_helpers.h"
#include "ui/common/widgets/screen_common.h"

void ResetOrientState() {
    Wallpaper_State().preview.rot_cw = 0;
    Wallpaper_State().preview.mirror_h = false;
}

bool AdjustDirty() {
    return Wallpaper_State().preview.rot_cw != 0 || Wallpaper_State().preview.mirror_h;
}

void BindPreviewRasterToImg() {
    if (Wallpaper_State().preview.raster == nullptr || Wallpaper_State().ui.preview_img == nullptr) {
        return;
    }
    if (!ScaleRasterToFit(*Wallpaper_State().preview.raster, FrameInner(kPreviewMaxW, kBorderW),
                          FrameInner(kPreviewMaxH, kBorderW), *Wallpaper_State().preview.raster)) {
        return;
    }
    Wallpaper_State().preview.raster->BindDsc();
    lv_image_set_scale(Wallpaper_State().ui.preview_img, LV_SCALE_NONE);
    lv_image_set_src(Wallpaper_State().ui.preview_img, &Wallpaper_State().preview.raster->dsc);
    EpdSetContentRole(Wallpaper_State().ui.preview_img, EpdContentRole::WallpaperPreview);
    lv_obj_set_size(Wallpaper_State().ui.preview_img, Wallpaper_State().preview.raster->width,
                    Wallpaper_State().preview.raster->height);
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
        Wallpaper_State().preview.raster = new RasterImage();
    }
    OrientRasterImage(*Wallpaper_State().preview.base, Wallpaper_State().preview.rot_cw,
                         Wallpaper_State().preview.mirror_h, *Wallpaper_State().preview.raster);
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
    set_hidden(Wallpaper_State().ui.enable_reading_btn, !show_normal);
    set_hidden(Wallpaper_State().ui.save_row, show_normal);

    const bool can_enter =
        show_normal && Wallpaper_State().preview.base != nullptr && !Wallpaper_State().preview.base->empty() &&
        !Wallpaper_State().workers.load_busy.load() && !Wallpaper_OperationPending(WallpaperOperation::Save) &&
        !Wallpaper_OperationPending(WallpaperOperation::Delete) && !Wallpaper_OperationPending(WallpaperOperation::Select);
    StyleActionBtn(Wallpaper_State().ui.adjust_btn, Wallpaper_State().ui.adjust_lbl, false, can_enter);

    const bool xform_ok = Wallpaper_State().preview.adjust_open && !Wallpaper_OperationPending(WallpaperOperation::Save);
    StyleActionBtn(Wallpaper_State().ui.rotate_left_btn, nullptr, false, xform_ok);
    StyleActionBtn(Wallpaper_State().ui.rotate_right_btn, nullptr, false, xform_ok);
    StyleActionBtn(Wallpaper_State().ui.mirror_btn, nullptr, false, xform_ok);
    StyleActionBtn(Wallpaper_State().ui.cancel_btn, nullptr, false, xform_ok);
    if (Wallpaper_State().ui.save_btn != nullptr && Wallpaper_State().ui.save_lbl != nullptr) {
        StyleActionBtn(Wallpaper_State().ui.save_btn, Wallpaper_State().ui.save_lbl, true, xform_ok);
    }
}

void ExitAdjustMode(bool restore_preview) {
    Wallpaper_CloseDialog();
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
    if (Wallpaper_State().mode != UiMode::kPreview || Wallpaper_State().preview.adjust_open ||
        Wallpaper_OperationPending(WallpaperOperation::Save)) {
        return;
    }
    if (Wallpaper_State().preview.base == nullptr || Wallpaper_State().preview.base->empty() ||
        Wallpaper_State().workers.load_busy.load() || Wallpaper_OperationPending(WallpaperOperation::Delete) ||
        Wallpaper_OperationPending(WallpaperOperation::Select)) {
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
    if (Wallpaper_OperationPending(WallpaperOperation::Save)) {
        return;
    }
    ExitAdjustMode(true);
}

void OnRotateLeftClicked(lv_event_t* /*e*/) {
    if (!Wallpaper_State().preview.adjust_open || Wallpaper_OperationPending(WallpaperOperation::Save)) {
        return;
    }
    Wallpaper_State().preview.rot_cw = static_cast<uint8_t>((Wallpaper_State().preview.rot_cw + 3) & 3);
    RefreshPreviewFromBase();
}

void OnRotateRightClicked(lv_event_t* /*e*/) {
    if (!Wallpaper_State().preview.adjust_open || Wallpaper_OperationPending(WallpaperOperation::Save)) {
        return;
    }
    Wallpaper_State().preview.rot_cw = static_cast<uint8_t>((Wallpaper_State().preview.rot_cw + 1) & 3);
    RefreshPreviewFromBase();
}

void OnMirrorHClicked(lv_event_t* /*e*/) {
    if (!Wallpaper_State().preview.adjust_open || Wallpaper_OperationPending(WallpaperOperation::Save)) {
        return;
    }
    // 对当前画面水平镜像：H∘R^r∘H^m = R^{-r}∘H^{m+1}
    Wallpaper_State().preview.rot_cw = static_cast<uint8_t>((4 - Wallpaper_State().preview.rot_cw) & 3);
    Wallpaper_State().preview.mirror_h = !Wallpaper_State().preview.mirror_h;
    RefreshPreviewFromBase();
}

void ShowOverwriteDialog() {
    if (!Wallpaper_State().screen_alive || Wallpaper_State().ui.dialog_mask != nullptr ||
        Wallpaper_OperationPending(WallpaperOperation::Save)) {
        return;
    }
    ScreenDialogAction actions[] = {
        {LangString_WALLPAPER_SAVE_AS, +[] { ScheduleSaveTransform(true); }},
        {LangString_WALLPAPER_OVERWRITE, +[] { ScheduleSaveTransform(false); }},
        {LangString_COMMON_CANCEL, {}}
    };
    auto* mask = ScreenShowChoiceDialog(Wallpaper_State().ui.screen, LangString_WALLPAPER_SAVE, actions, 3);
    Wallpaper_State().ui.dialog_mask = mask;
    if (mask) {
        lv_obj_add_event_cb(mask, [](lv_event_t*) { Wallpaper_State().ui.dialog_mask = nullptr; },
                            LV_EVENT_DELETE, nullptr);
    }
}

void OnSaveClicked(lv_event_t* /*e*/) {
    if (!Wallpaper_State().preview.adjust_open || Wallpaper_OperationPending(WallpaperOperation::Save)) {
        return;
    }
    if (!AdjustDirty()) {
        return;
    }
    ShowOverwriteDialog();
}
