#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <esp_log.h>
#include <lvgl.h>
#include "ui/apps/wallpaper/wallpaper_ui_helpers.h"
#include "assets/lang_config.h"
#include "assets/lang_wallpaper.h"
#include "business/reader/reader.h"
#include "business/reader/text_encoding.h"
#include "business/wallpaper/wallpaper_active.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "business/reader/book_reader_prefs.h"
#include "ui/common/widgets/screen_text.h"
#include "ui/common/widgets/storage_error_text.h"
#include "ui/apps/wallpaper/wallpaper_screen_priv.h"
#include "ui/common/drawing/ui_theme.h"

const lv_font_t* Wallpaper_UiFont() {
    return UiFont();
}

lv_coord_t Wallpaper_ContentWidth() {
    return LV_HOR_RES - kPad * 2;
}

// 按像素尽量排成两行，避免长文件名导致首行留白。
void Wallpaper_DisableScroll(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

void Wallpaper_CloseDialog() {
    auto& state = Wallpaper_State();
    lv_obj_t* mask = state.ui.dialog_mask;
    state.ui.dialog_mask = nullptr;
    if (mask && state.screen_alive) {
        lv_obj_delete(mask);
    }
}

void Wallpaper_FormatDecodeFailure(char* out, size_t capacity, ImageDecodeError error, const StorageError& storage_error) {
    if (storage_error) {
        std::snprintf(out, capacity, "%s", StorageErrorText(storage_error));
        return;
    }
    const auto code = error == ImageDecodeError::kNone ? ImageDecodeError::kFailed : error;
    std::snprintf(out, capacity, LangString_WALLPAPER_DECODE_FAIL_CODE, static_cast<unsigned>(code));
}

bool EndsWithIgnoreCase(const char* name, const char* ext) {
    if (name == nullptr || ext == nullptr) {
        return false;
    }
    const size_t nlen = std::strlen(name);
    const size_t elen = std::strlen(ext);
    if (nlen < elen) {
        return false;
    }
    for (size_t i = 0; i < elen; ++i) {
        char a = name[nlen - elen + i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<char>(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

bool IsListableFile(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
        return false;
    }
    if (EndsWithIgnoreCase(name, ".tmp")) {
        return false;
    }
    if (EndsWithIgnoreCase(name, ".meta.json") || EndsWithIgnoreCase(name, ".meta")) {
        return false;
    }
    return true;
}

void StyleActionBtn(lv_obj_t* btn, lv_obj_t* lbl, bool filled, bool enabled) {
    if (btn == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(btn, filled ? UiColorForeground() : UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, UiColorForeground(), 0);
    lv_obj_set_style_border_width(btn, kBorderW, 0);
    lv_obj_set_style_radius(btn, kBtnRadius, 0);
    if (enabled) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_state(btn, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_state(btn, LV_STATE_DISABLED);
    }
    if (lbl != nullptr) {
        lv_obj_set_style_text_color(lbl, filled ? UiColorBackground() : UiColorForeground(), 0);
        lv_obj_update_layout(btn);
        ScreenFitLabel(lbl, lv_obj_get_content_width(btn), lv_obj_get_content_height(btn), Wallpaper_UiFont());
    }
}

void UpdatePreviewMeta() {
    if (!Wallpaper_State().screen_alive || Wallpaper_State().ui.preview_meta == nullptr) {
        return;
    }
    lv_obj_clean(Wallpaper_State().ui.preview_meta);
    if (Wallpaper_State().preview.idx < 0 ||
        Wallpaper_State().preview.idx >= static_cast<int>(Wallpaper_State().list.files.size())) {
        return;
    }
    const FileEntry& fe = Wallpaper_State().list.files[static_cast<size_t>(Wallpaper_State().preview.idx)];
    const bool shutdown_on = !Wallpaper_State().shutdown_name.empty() && Wallpaper_State().shutdown_name == fe.name;
    const bool standby_on = !Wallpaper_State().standby_name.empty() && Wallpaper_State().standby_name == fe.name;

    // 删除/保存状态放在文件大小左侧，不盖预览图
    const char* left_hint = nullptr;
    if (Wallpaper_OperationPending(WallpaperOperation::Save)) {
        left_hint = LangString_WALLPAPER_SAVING;
    } else if (Wallpaper_OperationPending(WallpaperOperation::Delete)) {
        left_hint = LangString_WALLPAPER_DELETING;
    }
    if (left_hint != nullptr) {
        lv_obj_t* hint = lv_label_create(Wallpaper_State().ui.preview_meta);
        lv_label_set_text(hint, left_hint);
        lv_obj_set_style_text_font(hint, Wallpaper_UiFont(), 0);
        lv_obj_set_style_text_color(hint, UiColorForeground(), 0);
        lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    }

    auto make_chip = [&](const char* text, bool filled) {
        lv_obj_t* chip = lv_obj_create(Wallpaper_State().ui.preview_meta);
        lv_obj_remove_style_all(chip);
        lv_obj_set_height(chip, kPreviewChipH);
        lv_obj_set_width(chip, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(chip, 12, 0);
        lv_obj_set_style_border_width(chip, kBorderW, 0);
        lv_obj_set_style_border_color(chip, UiColorForeground(), 0);
        lv_obj_set_style_radius(chip, kBtnRadius, 0);
        lv_obj_set_style_bg_color(chip, filled ? UiColorForeground() : UiColorBackground(), 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        Wallpaper_DisableScroll(chip);
        lv_obj_t* lbl = lv_label_create(chip);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, Wallpaper_UiFont(), 0);
        lv_obj_set_style_text_color(lbl, filled ? UiColorBackground() : UiColorForeground(), 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    };

    char size_buf[24];
    FormatFileSize(size_buf, sizeof(size_buf), fe.size_bytes);
    make_chip(size_buf, false);
    if (shutdown_on) {
        make_chip(LangString_WALLPAPER_CHIP_SHUTDOWN, true);
    }
    if (standby_on) {
        make_chip(LangString_WALLPAPER_CHIP_STANDBY, true);
    }
}

void UpdateEnableButtonUi() {
    if (!Wallpaper_State().screen_alive) {
        return;
    }
    UpdatePreviewMeta();
    const bool ok_idx = Wallpaper_State().preview.idx >= 0 &&
                        Wallpaper_State().preview.idx < static_cast<int>(Wallpaper_State().list.files.size());
    const char* name =
        ok_idx ? Wallpaper_State().list.files[static_cast<size_t>(Wallpaper_State().preview.idx)].name : "";
    const bool busy = Wallpaper_OperationPending(WallpaperOperation::Select) || Wallpaper_OperationPending(WallpaperOperation::Delete) ||
                      Wallpaper_State().workers.load_busy.load() || Wallpaper_OperationPending(WallpaperOperation::Save) ||
                      Wallpaper_State().preview.adjust_open;

    auto paint = [&](lv_obj_t* btn, lv_obj_t* lbl, bool is_on, const char* idle_text, const char* on_text) {
        if (btn == nullptr || lbl == nullptr) {
            return;
        }
        if (!ok_idx) {
            lv_label_set_text(lbl, idle_text);
            StyleActionBtn(btn, lbl, false, false);
            return;
        }
        if (is_on) {
            lv_label_set_text(lbl, on_text);
            StyleActionBtn(btn, lbl, true, !busy);
        } else {
            lv_label_set_text(lbl, idle_text);
            StyleActionBtn(btn, lbl, false, !busy);
        }
    };

    const bool shutdown_on =
        ok_idx && !Wallpaper_State().shutdown_name.empty() && Wallpaper_State().shutdown_name == name;
    const bool standby_on = ok_idx && !Wallpaper_State().standby_name.empty() && Wallpaper_State().standby_name == name;
    paint(Wallpaper_State().ui.enable_shutdown_btn, Wallpaper_State().ui.enable_shutdown_lbl, shutdown_on,
          LangString_WALLPAPER_SET_SHUTDOWN, LangString_WALLPAPER_SHUTDOWN_ON);
    paint(Wallpaper_State().ui.enable_standby_btn, Wallpaper_State().ui.enable_standby_lbl, standby_on,
          LangString_WALLPAPER_SET_STANDBY, LangString_WALLPAPER_STANDBY_ON);
    bool reading_on = false;
    try {
        reading_on = ok_idx && BookReaderPrefsBackground() == 3 && Wallpaper_GetReadingFilename() == name;
    } catch (const std::bad_alloc&) {
        ESP_LOGW(TAG, "reading wallpaper selection lookup failed: no memory");
    }
    paint(Wallpaper_State().ui.enable_reading_btn, Wallpaper_State().ui.enable_reading_lbl, reading_on,
          LangString_WALLPAPER_SET_READING, LangString_WALLPAPER_READING_ON);
}

void UpdateDeleteButtonUi() {
    if (!Wallpaper_State().screen_alive || Wallpaper_State().ui.delete_btn == nullptr ||
        Wallpaper_State().ui.delete_lbl == nullptr) {
        return;
    }
    const bool ok_idx = Wallpaper_State().preview.idx >= 0 &&
                        Wallpaper_State().preview.idx < static_cast<int>(Wallpaper_State().list.files.size());
    const bool busy = Wallpaper_OperationPending(WallpaperOperation::Delete) || Wallpaper_OperationPending(WallpaperOperation::Select) ||
                      Wallpaper_State().workers.load_busy.load() || Wallpaper_OperationPending(WallpaperOperation::Save) ||
                      Wallpaper_State().preview.adjust_open;
    StyleActionBtn(Wallpaper_State().ui.delete_btn, Wallpaper_State().ui.delete_lbl, false, ok_idx && !busy);
}

// 创建角标底座；外框与内框留白后用于绘制图标。
lv_obj_t* MakeTipBadge(lv_obj_t* parent) {
    lv_obj_t* outer = lv_obj_create(parent);
    lv_obj_remove_style_all(outer);
    lv_obj_set_size(outer, 30, 30);
    lv_obj_set_style_bg_color(outer, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(outer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(outer, 2, 0);
    lv_obj_set_style_border_color(outer, UiColorForeground(), 0);
    lv_obj_set_style_radius(outer, 8, 0);
    lv_obj_set_style_pad_all(outer, 2, 0);
    lv_obj_clear_flag(outer, LV_OBJ_FLAG_CLICKABLE);
    Wallpaper_DisableScroll(outer);

    lv_obj_t* inner = lv_obj_create(outer);
    lv_obj_remove_style_all(inner);
    lv_obj_set_size(inner, 22, 22);
    lv_obj_set_style_bg_color(inner, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(inner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(inner, 2, 0);
    lv_obj_set_style_border_color(inner, UiColorForeground(), 0);
    lv_obj_set_style_radius(inner, 5, 0);
    lv_obj_clear_flag(inner, LV_OBJ_FLAG_CLICKABLE);
    Wallpaper_DisableScroll(inner);
    return inner;
}

// 关机壁纸的角标：电源键。
void DrawPowerTipIcon(lv_obj_t* parent) {
    lv_obj_t* tip = MakeTipBadge(parent);
    lv_obj_t* ring = lv_obj_create(tip);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, 12, 12);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, UiColorForeground(), 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, 1);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    Wallpaper_DisableScroll(ring);
    lv_obj_t* stem = lv_obj_create(tip);
    lv_obj_remove_style_all(stem);
    lv_obj_set_size(stem, 2, 6);
    lv_obj_set_style_bg_color(stem, UiColorForeground(), 0);
    lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(stem, 1, 0);
    lv_obj_align(stem, LV_ALIGN_TOP_MID, 0, 3);
    lv_obj_clear_flag(stem, LV_OBJ_FLAG_CLICKABLE);
    Wallpaper_DisableScroll(stem);
}

// 待机壁纸的角标：月牙。
void DrawMoonTipIcon(lv_obj_t* parent) {
    lv_obj_t* tip = MakeTipBadge(parent);
    lv_obj_t* moon = lv_obj_create(tip);
    lv_obj_remove_style_all(moon);
    lv_obj_set_size(moon, 10, 10);
    lv_obj_set_style_radius(moon, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(moon, UiColorForeground(), 0);
    lv_obj_set_style_bg_opa(moon, LV_OPA_COVER, 0);
    lv_obj_align(moon, LV_ALIGN_CENTER, -1, 0);
    lv_obj_clear_flag(moon, LV_OBJ_FLAG_CLICKABLE);
    Wallpaper_DisableScroll(moon);
    // 白圆切出月牙（与角标白底同色）
    lv_obj_t* cut = lv_obj_create(tip);
    lv_obj_remove_style_all(cut);
    lv_obj_set_size(cut, 8, 8);
    lv_obj_set_style_radius(cut, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cut, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(cut, LV_OPA_COVER, 0);
    lv_obj_align(cut, LV_ALIGN_CENTER, 3, -1);
    lv_obj_clear_flag(cut, LV_OBJ_FLAG_CLICKABLE);
    Wallpaper_DisableScroll(cut);
}

void Wallpaper_ShowMessage(lv_obj_t* host, const char* text) {
    if (host == nullptr) {
        return;
    }
    lv_obj_set_style_layout(host, LV_LAYOUT_NONE, 0);
    lv_obj_t* lbl = lv_label_create(host);
    lv_obj_set_style_text_font(lbl, Wallpaper_UiFont(), 0);
    lv_obj_set_style_text_color(lbl, UiColorForeground(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl, text != nullptr ? text : "");
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    Wallpaper_DisableScroll(lbl);
    Wallpaper_State().ui.list_empty = lbl;
}
