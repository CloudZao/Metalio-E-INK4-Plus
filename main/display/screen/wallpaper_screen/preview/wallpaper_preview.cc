#include "wallpaper_screen/preview/wallpaper_preview.h"
#include "wallpaper_screen/wallpaper_screen_priv.h"
#include "wallpaper_screen/preview/wallpaper_adjust.h"
#include "wallpaper_screen/wallpaper_list.h"
#include "wallpaper_screen/wallpaper_ops.h"
#include "wallpaper_screen/wallpaper_ui_helpers.h"

#include <freertos/FreeRTOS.h>
#include <lvgl.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>

#include "assets/lang_config.h"
#include "haptic_feedback.h"
#include "reader/reader.h"

void ClearPreviewImage() {
    if (Wallpaper_State().ui.preview_img != nullptr) {
        lv_image_set_src(Wallpaper_State().ui.preview_img, nullptr);
        lv_obj_add_flag(Wallpaper_State().ui.preview_img, LV_OBJ_FLAG_HIDDEN);
    }
    if (Wallpaper_State().preview.raster != nullptr) {
        delete Wallpaper_State().preview.raster;
        Wallpaper_State().preview.raster = nullptr;
    }
    if (Wallpaper_State().preview.base != nullptr) {
        delete Wallpaper_State().preview.base;
        Wallpaper_State().preview.base = nullptr;
    }
    Wallpaper_State().preview.rot_cw = 0;
    Wallpaper_State().preview.mirror_h = false;
}

struct LoadWork {
    int index = -1;
    uint32_t epoch = 0;
    char path[192] = {};
};

struct LoadResultMsg {
    int index = -1;
    uint32_t epoch = 0;
    bool ok = false;
    char err[80] = {};
    reader::RasterImage* img = nullptr;
};

void ApplyPreviewAsync(void* p) {
    auto* msg = static_cast<LoadResultMsg*>(p);
    Wallpaper_State().workers.load_busy.store(false);
    Wallpaper_State().workers.load_task = nullptr;
    if (msg == nullptr) {
        return;
    }
    if (!Wallpaper_State().screen_alive || msg->epoch != Wallpaper_State().epoch) {
        delete msg->img;
        delete msg;
        return;
    }
    if (Wallpaper_State().mode != UiMode::kPreview || msg->index != Wallpaper_State().preview.idx) {
        delete msg->img;
        delete msg;
        return;
    }

    ClearPreviewImage();
    if (!msg->ok || msg->img == nullptr || msg->img->empty()) {
        if (Wallpaper_State().ui.preview_status != nullptr) {
            lv_label_set_text(Wallpaper_State().ui.preview_status,
                              msg->err[0] != '\0' ? msg->err : Lang::Strings::WALLPAPER_PREVIEW_FAIL);
            lv_obj_clear_flag(Wallpaper_State().ui.preview_status, LV_OBJ_FLAG_HIDDEN);
        }
        UpdateEnableButtonUi();
        UpdateDeleteButtonUi();
        UpdateAdjustUi();
        delete msg->img;
        delete msg;
        return;
    }

    Wallpaper_State().preview.raster = msg->img;
    msg->img = nullptr;
    TrimBakedLetterbox(*Wallpaper_State().preview.raster);
    Wallpaper_State().preview.raster->BindDsc();
    delete Wallpaper_State().preview.base;
    Wallpaper_State().preview.base = new reader::RasterImage(*Wallpaper_State().preview.raster);
    Wallpaper_State().preview.rot_cw = 0;
    Wallpaper_State().preview.mirror_h = false;

    if (Wallpaper_State().ui.preview_img != nullptr) {
        lv_image_set_scale(Wallpaper_State().ui.preview_img, LV_SCALE_NONE);
        lv_image_set_src(Wallpaper_State().ui.preview_img, &Wallpaper_State().preview.raster->dsc);
        lv_obj_set_size(Wallpaper_State().ui.preview_img, Wallpaper_State().preview.raster->width, Wallpaper_State().preview.raster->height);
        lv_obj_clear_flag(Wallpaper_State().ui.preview_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_center(Wallpaper_State().ui.preview_img);
        lv_obj_invalidate(Wallpaper_State().ui.preview_img);
        if (Wallpaper_State().ui.preview_img_host != nullptr) {
            lv_obj_invalidate(Wallpaper_State().ui.preview_img_host);
        }
        ESP_LOGI(TAG, "preview L8 %ux%u", Wallpaper_State().preview.raster->width, Wallpaper_State().preview.raster->height);
    }
    if (Wallpaper_State().ui.preview_status != nullptr) {
        lv_obj_add_flag(Wallpaper_State().ui.preview_status, LV_OBJ_FLAG_HIDDEN);
    }
    UpdateEnableButtonUi();
    UpdateDeleteButtonUi();
    UpdateAdjustUi();
    delete msg;
}

void LoadPreviewTask(void* arg) {
    auto* work = static_cast<LoadWork*>(arg);
    auto* msg = new LoadResultMsg{};
    if (work == nullptr) {
        std::snprintf(msg->err, sizeof(msg->err), "%s", Lang::Strings::WALLPAPER_FILE_INVALID);
    } else {
        msg->index = work->index;
        msg->epoch = work->epoch;
        auto* img = new reader::RasterImage();
        if (!reader::DecodeImageFileToL8(work->path, FrameInner(kPreviewMaxW, kBorderW),
                                         FrameInner(kPreviewMaxH, kBorderW), *img) ||
            img->empty()) {
            delete img;
            std::snprintf(msg->err, sizeof(msg->err), "%s", Lang::Strings::WALLPAPER_DECODE_FAIL);
        } else {
            msg->ok = true;
            msg->img = img;
            ESP_LOGI(TAG, "preview decoded %ux%u from %s", img->width, img->height, work->path);
        }
        delete work;
    }

    if (lv_async_call(ApplyPreviewAsync, msg) != LV_RESULT_OK) {
        delete msg->img;
        delete msg;
        Wallpaper_State().workers.load_busy.store(false);
        Wallpaper_State().workers.load_task = nullptr;
    }
    vTaskDeleteWithCaps(nullptr);
}

void ScheduleLoadPreview(int index) {
    if (index < 0 || index >= static_cast<int>(Wallpaper_State().list.files.size())) {
        ESP_LOGW(TAG, "preview skip: bad index=%d files=%u", index,
                 static_cast<unsigned>(Wallpaper_State().list.files.size()));
        return;
    }
    if (Wallpaper_State().workers.load_busy.exchange(true)) {
        ESP_LOGW(TAG, "preview skip: load busy index=%d", index);
        return;
    }
    auto* work = new LoadWork{};
    work->index = index;
    work->epoch = Wallpaper_State().epoch;
    std::snprintf(work->path, sizeof(work->path), "%s",
                  Wallpaper_State().list.files[static_cast<size_t>(index)].path);
    // 与 thumb 相同：解码栈放 SPIRAM，避免内部 12KB 连续块不足 →「预览启动失败」
    constexpr uint32_t kPreviewStack = 12 * 1024;
    if (xTaskCreatePinnedToCoreWithCaps(LoadPreviewTask, "wp_preview", kPreviewStack, work, 5,
                                        &Wallpaper_State().workers.load_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t largest_int =
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const size_t largest_psram =
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        ESP_LOGW(TAG,
                 "preview task create failed: need stack=%u in SPIRAM; "
                 "int free=%u largest=%u; psram free=%u largest=%u; path=%s",
                 static_cast<unsigned>(kPreviewStack),
                 static_cast<unsigned>(free_int),
                 static_cast<unsigned>(largest_int),
                 static_cast<unsigned>(free_psram),
                 static_cast<unsigned>(largest_psram), work->path);
        delete work;
        Wallpaper_State().workers.load_busy.store(false);
        Wallpaper_State().workers.load_task = nullptr;
        if (Wallpaper_State().ui.preview_status != nullptr) {
            lv_label_set_text(Wallpaper_State().ui.preview_status,
                              largest_psram < kPreviewStack ? Lang::Strings::WALLPAPER_PREVIEW_OOM
                                                            : Lang::Strings::WALLPAPER_PREVIEW_START_FAIL);
            lv_obj_clear_flag(Wallpaper_State().ui.preview_status, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        ESP_LOGI(TAG, "preview task started index=%d path=%s", index, work->path);
    }
}

void OpenPreview(int index) {
    if (index < 0 || index >= static_cast<int>(Wallpaper_State().list.files.size())) {
        return;
    }
    CloseOverwriteDialog();
    Wallpaper_State().preview.adjust_open = false;
    ResetOrientState();
    Wallpaper_State().preview.idx = index;
    Wallpaper_State().preview.delete_meta_hint[0] = '\0';
    ClearPreviewImage();
    ShowMode(UiMode::kPreview);
    const FileEntry& fe = Wallpaper_State().list.files[static_cast<size_t>(index)];
    if (Wallpaper_State().ui.preview_title != nullptr) {
        const char* raw = fe.title[0] != '\0' ? fe.title : fe.name;
        const std::string shown = Wallpaper_LayoutTitleTwoLines(raw, Wallpaper_UiFont(), Wallpaper_ContentWidth());
        lv_label_set_text(Wallpaper_State().ui.preview_title, shown.c_str());
    }
    if (Wallpaper_State().ui.preview_status != nullptr) {
        lv_label_set_text(Wallpaper_State().ui.preview_status, Lang::Strings::WALLPAPER_LOADING);
        lv_obj_clear_flag(Wallpaper_State().ui.preview_status, LV_OBJ_FLAG_HIDDEN);
    }
    UpdateEnableButtonUi();
    UpdateDeleteButtonUi();
    UpdateAdjustUi();
    ScheduleLoadPreview(index);
}

void ClosePreview() {
    CloseOverwriteDialog();
    if (Wallpaper_State().workers.save_busy.load()) {
        ++Wallpaper_State().epoch;  // 作废在途保存
        Wallpaper_State().workers.save_busy.store(false);
        Wallpaper_State().workers.save_task = nullptr;
    }
    Wallpaper_State().preview.adjust_open = false;
    ResetOrientState();
    Wallpaper_State().preview.idx = -1;
    ClearPreviewImage();
    ShowMode(UiMode::kList);
    RebuildListPage();
}

void BuildPreviewBody(lv_obj_t* scr, lv_coord_t status_h) {
    // ---- 设置页：大预览 + 标题/标签 + 双列开关 + 删除 ----
    lv_obj_t* preview_body = lv_obj_create(scr);
    lv_obj_remove_style_all(preview_body);
    lv_obj_set_size(preview_body, LV_HOR_RES, Wallpaper_State().list.body_h);
    lv_obj_align(preview_body, LV_ALIGN_TOP_MID, 0, status_h);
    lv_obj_set_style_bg_opa(preview_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_hor(preview_body, kPad, 0);
    lv_obj_set_style_pad_top(preview_body, 8, 0);
    lv_obj_set_style_pad_bottom(preview_body, 12, 0);
    lv_obj_set_flex_flow(preview_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(preview_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(preview_body, kPreviewGap, 0);
    Wallpaper_DisableScroll(preview_body);
    lv_obj_add_flag(preview_body, LV_OBJ_FLAG_HIDDEN);
    Wallpaper_State().ui.preview_body = preview_body;

    lv_obj_t* img_host = lv_obj_create(preview_body);
    lv_obj_remove_style_all(img_host);
    lv_obj_set_width(img_host, Wallpaper_ContentWidth());
    lv_obj_set_flex_grow(img_host, 1);
    lv_obj_set_style_bg_color(img_host, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(img_host, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(img_host, kBorderW, 0);
    lv_obj_set_style_border_color(img_host, lv_color_black(), 0);
    lv_obj_set_style_pad_all(img_host, kBorderW, 0);
    lv_obj_set_style_radius(img_host, 16, 0);
    lv_obj_set_style_clip_corner(img_host, true, 0);
    Wallpaper_DisableScroll(img_host);
    Wallpaper_State().ui.preview_img_host = img_host;

    lv_obj_t* img = lv_image_create(img_host);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    Wallpaper_State().ui.preview_img = img;

    lv_obj_t* preview_status = lv_label_create(img_host);
    lv_obj_set_style_text_font(preview_status, Wallpaper_UiFont(), 0);
    lv_obj_set_style_text_color(preview_status, lv_color_black(), 0);
    lv_obj_set_style_text_align(preview_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(preview_status, "");
    lv_obj_align(preview_status, LV_ALIGN_CENTER, 0, 0);
    Wallpaper_State().ui.preview_status = preview_status;

    lv_obj_t* preview_title = lv_label_create(preview_body);
    lv_obj_set_width(preview_title, Wallpaper_ContentWidth());
    {
        const lv_font_t* f = Wallpaper_UiFont();
        const lv_coord_t lh = (f != nullptr && f->line_height > 0) ? f->line_height : 29;
        lv_obj_set_height(preview_title, lh * 2);
    }
    lv_obj_set_style_text_font(preview_title, Wallpaper_UiFont(), 0);
    lv_obj_set_style_text_color(preview_title, lv_color_black(), 0);
    lv_obj_set_style_text_align(preview_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(preview_title, 0, 0);
    // 文案已按像素拆成最多两行；CLIP 避免再走 LVGL 整词换行
    lv_label_set_long_mode(preview_title, LV_LABEL_LONG_CLIP);
    lv_label_set_text(preview_title, "");
    Wallpaper_State().ui.preview_title = preview_title;

    lv_obj_t* preview_meta = lv_obj_create(preview_body);
    lv_obj_remove_style_all(preview_meta);
    lv_obj_set_width(preview_meta, Wallpaper_ContentWidth());
    lv_obj_set_height(preview_meta, kPreviewMetaH);
    lv_obj_set_flex_flow(preview_meta, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(preview_meta, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(preview_meta, 8, 0);
    lv_obj_clear_flag(preview_meta, LV_OBJ_FLAG_CLICKABLE);
    Wallpaper_DisableScroll(preview_meta);
    Wallpaper_State().ui.preview_meta = preview_meta;

    lv_obj_t* actions = lv_obj_create(preview_body);
    lv_obj_remove_style_all(actions);
    lv_obj_set_width(actions, Wallpaper_ContentWidth());
    lv_obj_set_height(actions, kActionH);
    lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_CLICKABLE);
    Wallpaper_DisableScroll(actions);
    Wallpaper_State().ui.actions_row = actions;

    // 底栏两行槽位高度固定：上行关机/待机/调整 ↔ 左转/右转/镜像；下行删除 ↔ 取消/保存
    // 叠放子行须写死高度：创建时 get_height(parent) 常为 0，会导致整行不可见
    const lv_coord_t third_w = (Wallpaper_ContentWidth() - 16) / 3;
    auto make_row = [&](lv_obj_t* parent, lv_coord_t row_h, bool hidden) {
        lv_obj_t* row = lv_obj_create(parent);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), row_h);
        lv_obj_set_pos(row, 0, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        Wallpaper_DisableScroll(row);
        if (hidden) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        }
        return row;
    };
    auto make_slot_btn = [&](lv_obj_t* parent, lv_coord_t w, lv_coord_t h, const char* text,
                             lv_event_cb_t cb, bool filled, lv_obj_t** out_btn,
                             lv_obj_t** out_lbl) {
        lv_obj_t* btn = lv_obj_create(parent);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, w, h);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        Wallpaper_DisableScroll(btn);
        HapticAttachClick(btn);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, Wallpaper_UiFont(), 0);
        lv_label_set_text(lbl, text);
        StyleActionBtn(btn, lbl, filled, true);
        if (out_btn != nullptr) {
            *out_btn = btn;
        }
        if (out_lbl != nullptr) {
            *out_lbl = lbl;
        }
    };

    lv_obj_t* actions_normal = make_row(actions, kActionH, false);
    make_slot_btn(actions_normal, third_w, kActionH, Lang::Strings::WALLPAPER_SET_SHUTDOWN,
                  OnEnableShutdownClicked, false, &Wallpaper_State().ui.enable_shutdown_btn,
                  &Wallpaper_State().ui.enable_shutdown_lbl);
    make_slot_btn(actions_normal, third_w, kActionH, Lang::Strings::WALLPAPER_SET_STANDBY,
                  OnEnableStandbyClicked, false, &Wallpaper_State().ui.enable_standby_btn,
                  &Wallpaper_State().ui.enable_standby_lbl);
    make_slot_btn(actions_normal, third_w, kActionH, Lang::Strings::WALLPAPER_ADJUST, OnAdjustClicked,
                  false, &Wallpaper_State().ui.adjust_btn, &Wallpaper_State().ui.adjust_lbl);

    lv_obj_t* actions_adjust = make_row(actions, kActionH, true);
    make_slot_btn(actions_adjust, third_w, kActionH, Lang::Strings::WALLPAPER_ROTATE_LEFT,
                  OnRotateLeftClicked, false, &Wallpaper_State().ui.rotate_left_btn, nullptr);
    make_slot_btn(actions_adjust, third_w, kActionH, Lang::Strings::WALLPAPER_ROTATE_RIGHT,
                  OnRotateRightClicked, false, &Wallpaper_State().ui.rotate_right_btn, nullptr);
    make_slot_btn(actions_adjust, third_w, kActionH, Lang::Strings::WALLPAPER_MIRROR_H,
                  OnMirrorHClicked, false, &Wallpaper_State().ui.mirror_btn, nullptr);
    Wallpaper_State().ui.actions_row = actions;
    Wallpaper_State().ui.actions_normal_row = actions_normal;
    Wallpaper_State().ui.actions_adjust_row = actions_adjust;

    lv_obj_t* bottom_slot = lv_obj_create(preview_body);
    lv_obj_remove_style_all(bottom_slot);
    lv_obj_set_size(bottom_slot, Wallpaper_ContentWidth(), kDeleteH);
    lv_obj_set_style_bg_opa(bottom_slot, LV_OPA_TRANSP, 0);
    Wallpaper_DisableScroll(bottom_slot);
    lv_obj_clear_flag(bottom_slot, LV_OBJ_FLAG_CLICKABLE);
    Wallpaper_State().ui.bottom_slot = bottom_slot;

    lv_obj_t* delete_btn = lv_obj_create(bottom_slot);
    lv_obj_remove_style_all(delete_btn);
    lv_obj_set_size(delete_btn, Wallpaper_ContentWidth(), kDeleteH);
    lv_obj_set_pos(delete_btn, 0, 0);
    lv_obj_set_style_pad_all(delete_btn, 0, 0);
    lv_obj_set_flex_flow(delete_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(delete_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    Wallpaper_DisableScroll(delete_btn);
    HapticAttachClick(delete_btn);
    lv_obj_add_event_cb(delete_btn, Wallpaper_OnDeleteClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* delete_lbl = lv_label_create(delete_btn);
    lv_obj_set_style_text_font(delete_lbl, Wallpaper_UiFont(), 0);
    lv_label_set_text(delete_lbl, Lang::Strings::WALLPAPER_DELETE_BTN);
    StyleActionBtn(delete_btn, delete_lbl, false, true);
    Wallpaper_State().ui.delete_btn = delete_btn;
    Wallpaper_State().ui.delete_lbl = delete_lbl;

    lv_obj_t* save_row = lv_obj_create(bottom_slot);
    lv_obj_remove_style_all(save_row);
    lv_obj_set_size(save_row, Wallpaper_ContentWidth(), kDeleteH);
    lv_obj_set_pos(save_row, 0, 0);
    lv_obj_set_flex_flow(save_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(save_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(save_row, 12, 0);
    lv_obj_clear_flag(save_row, LV_OBJ_FLAG_CLICKABLE);
    Wallpaper_DisableScroll(save_row);
    lv_obj_add_flag(save_row, LV_OBJ_FLAG_HIDDEN);
    Wallpaper_State().ui.save_row = save_row;

    const lv_coord_t half_save_w = (Wallpaper_ContentWidth() - 12) / 2;
    make_slot_btn(save_row, half_save_w, kDeleteH, Lang::Strings::COMMON_CANCEL,
                  OnAdjustCancelClicked, false, &Wallpaper_State().ui.cancel_btn, nullptr);
    make_slot_btn(save_row, half_save_w, kDeleteH, Lang::Strings::WALLPAPER_SAVE, OnSaveClicked, true,
                  &Wallpaper_State().ui.save_btn, &Wallpaper_State().ui.save_lbl);
}

