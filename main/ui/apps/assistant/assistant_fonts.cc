// assistant_fonts.cc — UI fonts / a2ui init
#include <cstring>
#include <esp_log.h>
#include "ui/apps/assistant/assistant_screen_priv.h"
#include "ui/common/drawing/a2ui/a2ui.h"
#include "ui/common/drawing/font/ui_fonts.h"

const lv_font_t* Assistant_UiFont() {
    return UiFont(UiFontRole::Large);
}

const lv_font_t* Assistant_UiFontBold() {
    return UiFont(UiFontRole::LargeEmphasis);
}

void Assistant_DisableScroll(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

lv_coord_t Assistant_FontLineHeight(const lv_font_t* font) {
    return font != nullptr ? font->line_height : 28;
}

lv_coord_t Assistant_GlyphWidth(const lv_font_t* font, uint32_t cp) {
    if (font == nullptr) {
        return 12;
    }
    const lv_coord_t w = static_cast<lv_coord_t>(lv_font_get_glyph_width(font, cp, 0));
    return w > 0 ? w : 1;
}

lv_coord_t Assistant_VariantLetterSpace(const char* variant) {
    if (variant == nullptr) {
        return 0;
    }
    if (std::strcmp(variant, "title") == 0) {
        return 2;
    }
    if (std::strcmp(variant, "subtitle") == 0) {
        return 1;
    }
    if (std::strcmp(variant, "display") == 0) {
        return 3;
    }
    return 0;
}

const lv_font_t* Assistant_FontFor(bool bold) {
    return bold ? Assistant_UiFontBold() : Assistant_UiFont();
}

void Assistant_EnsureA2ui(lv_obj_t* scr) {
    if (Assistant_State().layout.a2ui_ready) {
        return;
    }
    lv_display_t* disp = lv_obj_get_display(scr);
    if (disp == nullptr) {
        disp = lv_display_get_default();
    }
    if (disp == nullptr) {
        ESP_LOGE(TAG, "a2ui init: no lv_display");
        return;
    }
    a2ui_config_t cfg = A2UI_CONFIG_DEFAULT();
    cfg.disp = disp;
    cfg.font_regular = Assistant_UiFont();
    cfg.font_bold = Assistant_UiFontBold();
    cfg.on_action = nullptr;
    if (a2ui_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "a2ui_init failed");
        return;
    }
    Assistant_State().layout.a2ui_ready = true;
}
