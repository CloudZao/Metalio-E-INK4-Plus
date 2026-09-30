// assistant_fonts.cc — UI fonts / idle hint / a2ui init
#include "assistant_screen_priv.h"

#include "assistant_fonts.h"
#include "a2ui.h"
#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "image_util.h"
#include "lv_adapter_display.h"
#include "reader_types.h"

#include <cstring>

#include <esp_log.h>

// CMake EMBED：assets/embedded/ic_s_assistant_hint_en.a2i1
extern const uint8_t ic_s_assistant_hint_en_a2i1_start[] asm("_binary_ic_s_assistant_hint_en_a2i1_start");
extern const uint8_t ic_s_assistant_hint_en_a2i1_end[] asm("_binary_ic_s_assistant_hint_en_a2i1_end");

const lv_font_t* Assistant_UiFont() {
    if (Assistant_State().layout.ui_font != nullptr) {
        return Assistant_State().layout.ui_font;
    }
    Assistant_State().layout.ui_font = fontpack_lv_font_get(kUiFontSize, kUiFontBpp);
    if (Assistant_State().layout.ui_font == nullptr) {
        ESP_LOGW(TAG, "fontpack %u/%ubpp unavailable, fallback UI 30@2", kUiFontSize, kUiFontBpp);
        Assistant_State().layout.ui_font = fontpack_lv_font_ui();
    } else {
        ESP_LOGI(TAG, "using fontpack regular size=%u req_bpp=%u got_bpp=%u", kUiFontSize,
                 kUiFontBpp, fontpack_lv_font_bpp(Assistant_State().layout.ui_font));
    }
    return Assistant_State().layout.ui_font;
}

const lv_font_t* Assistant_UiFontBold() {
    if (Assistant_State().layout.ui_font_bold != nullptr) {
        return Assistant_State().layout.ui_font_bold;
    }
    Assistant_State().layout.ui_font_bold = fontpack_lv_font_get(kUiFontSize, kUiFontBoldBpp);
    if (Assistant_State().layout.ui_font_bold == nullptr) {
        ESP_LOGW(TAG, "fontpack bold %u/%ubpp unavailable, fallback regular", kUiFontSize,
                 kUiFontBoldBpp);
        Assistant_State().layout.ui_font_bold = Assistant_UiFont();
    } else {
        ESP_LOGI(TAG, "using fontpack bold size=%u req_bpp=%u got_bpp=%u", kUiFontSize,
                 kUiFontBoldBpp, fontpack_lv_font_bpp(Assistant_State().layout.ui_font_bold));
    }
    return Assistant_State().layout.ui_font_bold;
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

void Assistant_HideIdleHint() {
    if (Assistant_State().chrome.hint_img != nullptr && lv_obj_is_valid(Assistant_State().chrome.hint_img)) {
        lv_obj_add_flag(Assistant_State().chrome.hint_img, LV_OBJ_FLAG_HIDDEN);
    }
}

void Assistant_ShowIdleHint() {
    if (Assistant_State().chrome.hint_img == nullptr || !lv_obj_is_valid(Assistant_State().chrome.hint_img) || Assistant_State().chrome.hint_raster == nullptr) {
        return;
    }
    lv_obj_remove_flag(Assistant_State().chrome.hint_img, LV_OBJ_FLAG_HIDDEN);
}

// 从固件嵌入（en）或 resources（zh）解码空状态全屏图到 L8。失败则控件保持隐藏。
void Assistant_LoadIdleHintImage(lv_obj_t* img) {
    if (img == nullptr) {
        return;
    }

    const uint8_t* mem = nullptr;
    size_t size = 0;
    const bool use_en = (Lang::CODE != nullptr && std::strcmp(Lang::CODE, "en-US") == 0);
    if (use_en) {
        mem = ic_s_assistant_hint_en_a2i1_start;
        size = static_cast<size_t>(ic_s_assistant_hint_en_a2i1_end - ic_s_assistant_hint_en_a2i1_start);
        if (mem == nullptr || size == 0) {
            ESP_LOGW(TAG, "idle hint: embedded en a2i1 empty");
            return;
        }
    } else {
        auto* disp = LVAdapterDisplay::Instance();
        if (disp == nullptr) {
            ESP_LOGW(TAG, "idle hint: no display");
            return;
        }
        if (!disp->TryGetResource(kIdleHintAssetZh, &mem, &size) || mem == nullptr || size == 0) {
            ESP_LOGW(TAG, "idle hint: missing %s (rebuild resources?)", kIdleHintAssetZh);
            return;
        }
    }

    auto* raster = new reader::RasterImage();
    if (!reader::DecodeImageToL8(mem, size, kIdleHintMaxW, kIdleHintMaxH, *raster) ||
        raster->empty()) {
        ESP_LOGW(TAG, "idle hint: decode failed (%s)", use_en ? "en-embed" : "zh-res");
        delete raster;
        return;
    }
    Assistant_State().chrome.hint_raster = raster;
    lv_obj_set_user_data(img, raster);
    lv_image_set_src(img, &raster->dsc);
    lv_obj_set_size(img, raster->width, raster->height);
    lv_obj_align(img, LV_ALIGN_BOTTOM_MID, 0, 0);
    ESP_LOGI(TAG, "idle hint ready %ux%u (%s)", static_cast<unsigned>(raster->width),
             static_cast<unsigned>(raster->height), use_en ? "en-embed" : "zh-res");
}

