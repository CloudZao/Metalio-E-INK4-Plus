#include "book_screen/book_font_runtime.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/reader/book_reader_prefs.h"

#include <lvgl.h>
#include <string>
#include <esp_log.h>

#include "display/font/epdfont.h"
#include "fontpack_lvgl.h"
#include "reader/reader.h"

epdfont_t* s_epdfont = nullptr;

// 书库/详情等界面标题：固件 fontpack 30@2
const lv_font_t* Book_ListFont() {
    const lv_font_t* f = fontpack_lv_font_get(30, 2);
    return f != nullptr ? f : fontpack_lv_font_ui();
}

// 书库列表 item：固件 fontpack UI 默认字（30@2）
const lv_font_t* Book_ItemFont() {
    return fontpack_lv_font_ui();
}

// 仅正文内容：SD epdfont（NVS 所选），失败回退 fontpack UI
const lv_font_t* BookFont() {
    const lv_font_t* f = epdfont_get_lv_font(s_epdfont);
    return f != nullptr ? f : fontpack_lv_font_ui();
}

void EnsureBookFont() {
    BookReaderPrefsEnsureLoaded();
    if (s_epdfont != nullptr) {
        return;
    }
    char path[192];
    if (BookReaderPrefsFontFullPath(path, sizeof(path)) != nullptr) {
        s_epdfont = epdfont_open(path);
    }
    if (s_epdfont == nullptr) {
        s_epdfont = epdfont_open(kBookFontPath);
    }
    if (s_epdfont == nullptr) {
        s_epdfont = epdfont_open(kBookFontPathAlt);
    }
    if (s_epdfont == nullptr) {
        ESP_LOGW(TAG, "epdfont load failed (%s), fallback MiSans flash",
                 BookReaderPrefsFontFile());
    } else {
        ESP_LOGI(TAG, "epdfont loaded file=%s glyphs=%u bpp=%u", BookReaderPrefsFontFile(),
                 static_cast<unsigned>(epdfont_glyph_count(s_epdfont)), epdfont_get_bpp(s_epdfont));
    }
}

void ReleaseBookFont() {
    if (s_epdfont == nullptr) {
        return;
    }
    epdfont_close(s_epdfont);
    s_epdfont = nullptr;
}

void PrewarmPageFont(const reader::Page* page) {
    if (s_epdfont == nullptr || page == nullptr) {
        return;
    }
    std::string blob;
    blob.reserve(2048);
    for (const auto& item : page->items) {
        if (item.kind == reader::ContentKind::kText) {
            blob.append(item.text);
            blob.push_back('\n');
        }
    }
    if (!blob.empty()) {
        epdfont_prewarm_utf8(s_epdfont, blob.c_str(), blob.size());
    }
}

