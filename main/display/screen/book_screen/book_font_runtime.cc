#include "book_screen/book_font_runtime.h"
#include "sd_paths.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/reader/book_reader_prefs.h"

#include <lvgl.h>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>
#include <esp_log.h>

#include "display/font/epdfont.h"
#include "display/font/ttf_runtime.h"
#include "fontpack_lvgl.h"
#include "reader/reader.h"
#include "reader/text_encoding.h"
#include "reader/vertical_text.h"

epdfont_t* s_epdfont = nullptr;
static TtfRuntime* s_ttf_font = nullptr; // 直接加载的正文矢量字体
static char s_prepared_file[SD_FILE_NAME_CAPACITY] = {}; // 等待旧编排退出后应用的源文件名
static int s_prepared_size = 0; // 待应用字体的像素字号
static char s_prepared_base_file[SD_FILE_NAME_CAPACITY] = {}; // 接收选择时的字体偏好，用于识别后续选择
static int s_prepared_base_size = 0; // 接收选择时的字体字号

static void ClearPreparedTtf() {
    s_prepared_file[0] = s_prepared_base_file[0] = '\0';
    s_prepared_size = s_prepared_base_size = 0;
}

// 书库/详情等界面标题：固件 fontpack 30@2
const lv_font_t* Book_ListFont() {
    const lv_font_t* f = fontpack_lv_font_get(30, 2);
    return f != nullptr ? f : fontpack_lv_font_ui();
}

// 书库列表 item：固件 fontpack UI 默认字（30@2）
const lv_font_t* Book_ItemFont() {
    return fontpack_lv_font_ui();
}

// 正文字体由 NVS 选择，支持 TTF/OTF 和 .ef。
const lv_font_t* BookFont() {
    if (s_ttf_font) return TtfRuntimeFont(s_ttf_font);
    const lv_font_t* f = epdfont_get_lv_font(s_epdfont);
    return f != nullptr ? f : fontpack_lv_font_ui();
}

const lv_font_t* BookLayoutFont() {
    return s_ttf_font ? TtfRuntimeLayoutFont(s_ttf_font) : BookFont();
}

void EnsureBookFont() {
    BookReaderPrefsEnsureLoaded();
    if (s_epdfont != nullptr || s_ttf_font != nullptr) {
        return;
    }
    char path[SD_FILE_PATH_CAPACITY];
    const char* loaded_path = path;
    if (BookReaderPrefsFontFullPath(path, sizeof(path)) != nullptr) {
        if (BookReaderPrefsFontSize() > 0) {
            s_ttf_font = TtfRuntimeOpen(path, BookReaderPrefsFontSize());
            if (s_ttf_font) return;
        } else {
            s_epdfont = epdfont_open(path);
        }
    }
    if (s_epdfont == nullptr) {
        s_epdfont = epdfont_open(kBookFontPath);
        loaded_path = kBookFontPath;
    }
    if (s_epdfont == nullptr) {
        s_epdfont = epdfont_open(kBookFontPathAlt);
        loaded_path = kBookFontPathAlt;
    }
    if (s_epdfont == nullptr) {
        ESP_LOGW(TAG, "epdfont load failed (%s), fallback MiSans flash",
                 BookReaderPrefsFontFile());
    } else {
        ESP_LOGI(TAG, "epdfont loaded file=%s glyphs=%u bpp=%u", loaded_path,
                 static_cast<unsigned>(epdfont_glyph_count(s_epdfont)), epdfont_get_bpp(s_epdfont));
    }
}

bool PrepareBookTtfFont(const char* filename, int size_px) {
    if (!SdIsValidFileName(filename) || size_px < 20 || size_px > 80) return false;
    std::snprintf(s_prepared_file, sizeof(s_prepared_file), "%s", filename);
    s_prepared_size = size_px;
    std::snprintf(s_prepared_base_file, sizeof(s_prepared_base_file), "%s", BookReaderPrefsFontFile());
    s_prepared_base_size = BookReaderPrefsFontSize();
    return true;
}

bool ReloadBookFont() {
    TtfRuntime* prepared = nullptr;
    if (s_prepared_file[0] != '\0' && (s_prepared_base_size != BookReaderPrefsFontSize() ||
        std::strcmp(s_prepared_base_file, BookReaderPrefsFontFile()) != 0)) {
        ClearPreparedTtf();
    }
    if (s_prepared_file[0] != '\0') {
        char path[SD_FILE_PATH_CAPACITY];
        std::snprintf(path, sizeof(path), "%s/%s", SD_PATH_FONT_SRC, s_prepared_file);
        prepared = TtfRuntimeOpen(path, s_prepared_size);
        if (!prepared) {
            ClearPreparedTtf();
            return false;
        }
        BookReaderPrefsSetFontFile(s_prepared_file, s_prepared_size);
        ClearPreparedTtf();
        const auto& session = Book_State().reader.session;
        if (session && session->IsPageImagesMode()) {
            TtfRuntimeClose(prepared);
            return true;
        }
    }
    ReleaseBookFont();
    if (prepared) s_ttf_font = prepared;
    else EnsureBookFont();
    return true;
}

void ReleaseBookFont(bool cancel_prepared) {
    if (cancel_prepared) ClearPreparedTtf();
    TtfRuntimeClose(s_ttf_font);
    s_ttf_font = nullptr;
    if (s_epdfont == nullptr) {
        return;
    }
    epdfont_close(s_epdfont);
    s_epdfont = nullptr;
}

void PrewarmPageFont(const reader::Page* page) {
    if (page == nullptr) {
        return;
    }
    if (s_ttf_font) {
        static bool allocation_error_logged = false;
        try {
            std::vector<uint32_t> codepoints;
            codepoints.reserve(512);
            const auto& session = Book_State().reader.session;
            const bool vertical = session && session->VerticalWriting();
            for (const auto& item : page->items) {
                if (item.kind != reader::ContentKind::kText) continue;
                for (size_t offset = 0; offset < item.text.size();) {
                    uint32_t cp = 0;
                    const size_t n = reader::Utf8Next(reinterpret_cast<const uint8_t*>(item.text.data()) + offset,
                                                     item.text.size() - offset, &cp);
                    if (n == 0) break;
                    offset += n;
                    if (cp == '\t') cp = ' ';
                    if (cp >= 0x20) codepoints.push_back(vertical ? reader::VerticalGlyph(cp) : cp);
                }
            }
            std::sort(codepoints.begin(), codepoints.end());
            codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());
            TtfRuntimePrewarm(s_ttf_font, codepoints.data(), codepoints.size());
            allocation_error_logged = false;
        } catch (const std::bad_alloc&) {
            if (!allocation_error_logged) ESP_LOGW(TAG, "TTF page prewarm allocation failed; using on-demand glyphs");
            allocation_error_logged = true;
        }
        return;
    }
    if (!s_epdfont) return;
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

