#include "wallpaper_screen/wallpaper_ui_helpers.h"
#include "wallpaper_screen/wallpaper_screen_priv.h"

#include <lvgl.h>
#include <cstdint>
#include <cstring>
#include <string>

#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "reader/reader.h"
#include "reader/text_encoding.h"

const lv_font_t* Wallpaper_UiFont() {
    return fontpack_lv_font_ui();
}

lv_coord_t Wallpaper_ContentWidth() {
    return LV_HOR_RES - kPad * 2;
}

// 按像素尽量排成两行，避免长文件名导致首行留白。
std::string Wallpaper_LayoutTitleTwoLines(const char* text, const lv_font_t* font, lv_coord_t max_w) {
    if (text == nullptr || text[0] == '\0') {
        return {};
    }
    if (font == nullptr || max_w <= 0) {
        return text;
    }

    const uint8_t* d = reinterpret_cast<const uint8_t*>(text);
    const size_t len = std::strlen(text);
    auto glyph_w = [font](uint32_t cp) -> lv_coord_t {
        lv_coord_t gw = static_cast<lv_coord_t>(lv_font_get_glyph_width(font, cp, 0));
        return gw > 0 ? gw : 1;
    };

    std::string line1;
    size_t i = 0;
    lv_coord_t used = 0;
    while (i < len) {
        uint32_t cp = 0;
        const size_t n = reader::Utf8Next(d + i, len - i, &cp);
        if (n == 0) {
            break;
        }
        const lv_coord_t gw = glyph_w(cp);
        if (used + gw > max_w) {
            break;
        }
        line1.append(text + i, n);
        used += gw;
        i += n;
    }
    if (i >= len) {
        return line1;
    }

    lv_coord_t rest_w = 0;
    for (size_t j = i; j < len;) {
        uint32_t cp = 0;
        const size_t n = reader::Utf8Next(d + j, len - j, &cp);
        if (n == 0) {
            break;
        }
        rest_w += glyph_w(cp);
        j += n;
    }
    if (rest_w <= max_w) {
        return line1 + "\n" + std::string(text + i);
    }

    constexpr const char* kEllipsis = "…";
    uint32_t ell_cp = 0;
    reader::Utf8Next(reinterpret_cast<const uint8_t*>(kEllipsis), 3, &ell_cp);
    const lv_coord_t budget = max_w - glyph_w(ell_cp);

    std::string line2;
    used = 0;
    while (i < len && budget > 0) {
        uint32_t cp = 0;
        const size_t n = reader::Utf8Next(d + i, len - i, &cp);
        if (n == 0) {
            break;
        }
        const lv_coord_t gw = glyph_w(cp);
        if (used + gw > budget) {
            break;
        }
        line2.append(text + i, n);
        used += gw;
        i += n;
    }
    line2.append(kEllipsis);
    return line1 + "\n" + line2;
}

void Wallpaper_DisableScroll(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
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
    if (EndsWithIgnoreCase(name, ".meta.json")) {
        return false;
    }
    return true;
}

void StyleActionBtn(lv_obj_t* btn, lv_obj_t* lbl, bool filled, bool enabled) {
    if (btn == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(btn, filled ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
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
        lv_obj_set_style_text_color(lbl, filled ? lv_color_white() : lv_color_black(), 0);
    }
}

void UpdatePreviewMeta() {
    if (!Wallpaper_State().screen_alive || Wallpaper_State().ui.preview_meta == nullptr) {
        return;
    }
    lv_obj_clean(Wallpaper_State().ui.preview_meta);
    if (Wallpaper_State().preview.idx < 0 || Wallpaper_State().preview.idx >= static_cast<int>(Wallpaper_State().list.files.size())) {
        return;
    }
    const FileEntry& fe = Wallpaper_State().list.files[static_cast<size_t>(Wallpaper_State().preview.idx)];
    const bool shutdown_on = !Wallpaper_State().shutdown_name.empty() && Wallpaper_State().shutdown_name == fe.name;
    const bool standby_on = !Wallpaper_State().standby_name.empty() && Wallpaper_State().standby_name == fe.name;

    // 删除/保存状态放在文件大小左侧，不盖预览图
    const char* left_hint = nullptr;
    if (Wallpaper_State().workers.save_busy.load()) {
        left_hint = Lang::Strings::WALLPAPER_SAVING;
    } else if (Wallpaper_State().workers.delete_busy.load()) {
        left_hint = Lang::Strings::WALLPAPER_DELETING;
    } else if (Wallpaper_State().preview.delete_meta_hint[0] != '\0') {
        left_hint = Wallpaper_State().preview.delete_meta_hint;
    }
    if (left_hint != nullptr) {
        lv_obj_t* hint = lv_label_create(Wallpaper_State().ui.preview_meta);
        lv_label_set_text(hint, left_hint);
        lv_obj_set_style_text_font(hint, Wallpaper_UiFont(), 0);
        lv_obj_set_style_text_color(hint, lv_color_black(), 0);
        lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    }

    auto make_chip = [&](const char* text, bool filled) {
        lv_obj_t* chip = lv_obj_create(Wallpaper_State().ui.preview_meta);
        lv_obj_remove_style_all(chip);
        lv_obj_set_height(chip, kPreviewChipH);
        lv_obj_set_width(chip, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(chip, 12, 0);
        lv_obj_set_style_border_width(chip, kBorderW, 0);
        lv_obj_set_style_border_color(chip, lv_color_black(), 0);
        lv_obj_set_style_radius(chip, kBtnRadius, 0);
        lv_obj_set_style_bg_color(chip, filled ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        Wallpaper_DisableScroll(chip);
        lv_obj_t* lbl = lv_label_create(chip);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, Wallpaper_UiFont(), 0);
        lv_obj_set_style_text_color(lbl, filled ? lv_color_white() : lv_color_black(), 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    };

    char size_buf[24];
    reader::FormatFileSize(size_buf, sizeof(size_buf), fe.size_bytes);
    make_chip(size_buf, false);
    if (shutdown_on) {
        make_chip(Lang::Strings::WALLPAPER_CHIP_SHUTDOWN, true);
    }
    if (standby_on) {
        make_chip(Lang::Strings::WALLPAPER_CHIP_STANDBY, true);
    }
}

void UpdateEnableButtonUi() {
    if (!Wallpaper_State().screen_alive) {
        return;
    }
    UpdatePreviewMeta();
    const bool ok_idx =
        Wallpaper_State().preview.idx >= 0 && Wallpaper_State().preview.idx < static_cast<int>(Wallpaper_State().list.files.size());
    const char* name =
        ok_idx ? Wallpaper_State().list.files[static_cast<size_t>(Wallpaper_State().preview.idx)].name : "";
    const bool busy = Wallpaper_State().workers.enable_busy.load() || Wallpaper_State().workers.delete_busy.load() || Wallpaper_State().workers.load_busy.load() ||
                      Wallpaper_State().workers.save_busy.load() || Wallpaper_State().preview.adjust_open;

    auto paint = [&](lv_obj_t* btn, lv_obj_t* lbl, bool is_on, const char* idle_text,
                     const char* on_text) {
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
    const bool standby_on =
        ok_idx && !Wallpaper_State().standby_name.empty() && Wallpaper_State().standby_name == name;
    paint(Wallpaper_State().ui.enable_shutdown_btn, Wallpaper_State().ui.enable_shutdown_lbl, shutdown_on, Lang::Strings::WALLPAPER_SET_SHUTDOWN,
          Lang::Strings::WALLPAPER_SHUTDOWN_ON);
    paint(Wallpaper_State().ui.enable_standby_btn, Wallpaper_State().ui.enable_standby_lbl, standby_on, Lang::Strings::WALLPAPER_SET_STANDBY,
          Lang::Strings::WALLPAPER_STANDBY_ON);
}

void UpdateDeleteButtonUi() {
    if (!Wallpaper_State().screen_alive || Wallpaper_State().ui.delete_btn == nullptr || Wallpaper_State().ui.delete_lbl == nullptr) {
        return;
    }
    const bool ok_idx =
        Wallpaper_State().preview.idx >= 0 && Wallpaper_State().preview.idx < static_cast<int>(Wallpaper_State().list.files.size());
    const bool busy = Wallpaper_State().workers.delete_busy.load() || Wallpaper_State().workers.enable_busy.load() || Wallpaper_State().workers.load_busy.load() ||
                      Wallpaper_State().workers.save_busy.load() || Wallpaper_State().preview.adjust_open;
    StyleActionBtn(Wallpaper_State().ui.delete_btn, Wallpaper_State().ui.delete_lbl, false, ok_idx && !busy);
}

// 创建角标底座；外框与内框留白后用于绘制图标。
lv_obj_t* MakeTipBadge(lv_obj_t* parent) {
    lv_obj_t* outer = lv_obj_create(parent);
    lv_obj_remove_style_all(outer);
    lv_obj_set_size(outer, 30, 30);
    lv_obj_set_style_bg_color(outer, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(outer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(outer, 2, 0);
    lv_obj_set_style_border_color(outer, lv_color_black(), 0);
    lv_obj_set_style_radius(outer, 8, 0);
    lv_obj_set_style_pad_all(outer, 2, 0);
    lv_obj_clear_flag(outer, LV_OBJ_FLAG_CLICKABLE);
    Wallpaper_DisableScroll(outer);

    lv_obj_t* inner = lv_obj_create(outer);
    lv_obj_remove_style_all(inner);
    lv_obj_set_size(inner, 22, 22);
    lv_obj_set_style_bg_color(inner, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(inner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(inner, 2, 0);
    lv_obj_set_style_border_color(inner, lv_color_black(), 0);
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
    lv_obj_set_style_border_color(ring, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, 1);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    Wallpaper_DisableScroll(ring);
    lv_obj_t* stem = lv_obj_create(tip);
    lv_obj_remove_style_all(stem);
    lv_obj_set_size(stem, 2, 6);
    lv_obj_set_style_bg_color(stem, lv_color_black(), 0);
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
    lv_obj_set_style_bg_color(moon, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(moon, LV_OPA_COVER, 0);
    lv_obj_align(moon, LV_ALIGN_CENTER, -1, 0);
    lv_obj_clear_flag(moon, LV_OBJ_FLAG_CLICKABLE);
    Wallpaper_DisableScroll(moon);
    // 白圆切出月牙（与角标白底同色）
    lv_obj_t* cut = lv_obj_create(tip);
    lv_obj_remove_style_all(cut);
    lv_obj_set_size(cut, 8, 8);
    lv_obj_set_style_radius(cut, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cut, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cut, LV_OPA_COVER, 0);
    lv_obj_align(cut, LV_ALIGN_CENTER, 3, -1);
    lv_obj_clear_flag(cut, LV_OBJ_FLAG_CLICKABLE);
    Wallpaper_DisableScroll(cut);
}

void Wallpaper_ShowMessage(lv_obj_t* host, const char* text) {
    if (host == nullptr) {
        return;
    }
    lv_obj_t* lbl = lv_label_create(host);
    lv_obj_set_style_text_font(lbl, Wallpaper_UiFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl, text != nullptr ? text : "");
    lv_obj_set_width(lbl, Wallpaper_ContentWidth());
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    Wallpaper_State().ui.list_empty = lbl;
}

