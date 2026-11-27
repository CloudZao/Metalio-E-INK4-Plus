#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "infra/tasks/task_create.h"
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <strings.h>
#include "ui/common/drawing/ui_profile.h"
#include "ui/common/drawing/rendering/epd_reader_aa.h"
#include "ui/apps/book/book_font_runtime.h"
#include "boards/common/storage/sd_access.h"
#include "infra/storage/sd_paths.h"
#include "business/reader/reader.h"
#include "business/reader/text_encoding.h"
#include "business/reader/vertical_text.h"
#include "ui/common/drawing/font/epdfont.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/drawing/font/ttf_runtime.h"
#include "ui/apps/book/book_screen_priv.h"
#include "ui/apps/book/book_nav.h"
#include "business/reader/book_reader_prefs.h"

epdfont_t* s_epdfont = nullptr;
static TtfRuntime* s_ttf_font = nullptr;                       // 直接加载的正文矢量字体
static TtfRuntime* s_retired_ttf = nullptr;                    // 新正文接入前保留旧标签引用的字体
static epdfont_t* s_retired_ef = nullptr;                      // 新正文接入前保留旧标签引用的字库
static char s_prepared_file[SD_FILE_NAME_CAPACITY] = {};       // 等待旧编排退出后应用的源文件名
static int s_prepared_size = 0;                                // 待应用字体的像素字号
static char s_prepared_base_file[SD_FILE_NAME_CAPACITY] = {};  // 接收选择时的字体偏好，用于识别后续选择
static int s_prepared_base_size = 0;                           // 接收选择时的字体字号
static uint32_t s_font_request = 0;                            // UI 字体选择代次

struct BookFontRuntimeFontReload {
    char file[SD_FILE_NAME_CAPACITY]{};                    // 本次加载的文件名
    char path[SD_FILE_PATH_CAPACITY]{};                    // 工作任务独占的路径快照
    char base_file[SD_FILE_NAME_CAPACITY]{};               // 接受请求时的偏好名
    int size = 0, base_size = 0;                           // 请求字号和原偏好字号
    uint32_t token = 0;                                    // 请求代次
    bool selected = false, unchanged = false, ok = false;  // 待应用选择、无需重开和加载结果
    TtfRuntime* ttf = nullptr;                             // 尚未发布的矢量字体
    epdfont_t* ef = nullptr;                               // 尚未发布的位图字体
    std::atomic<bool> done{false};                         // 工作任务完成后交由 UI 接入

    ~BookFontRuntimeFontReload() {
        TtfRuntimeClose(ttf);
        if (ef) {
            epdfont_close(ef);
        }
    }
};

static std::unique_ptr<BookFontRuntimeFontReload> s_reload;

static void LoadFont(void* data) {
    auto& job = *static_cast<BookFontRuntimeFontReload*>(data);
    try {
        {
            SdAccessTransaction transaction;
            if (transaction) {
                if (job.unchanged) {
                    job.ok = true;
                } else if (job.size > 0) {
                    job.ok = (job.ttf = TtfRuntimeOpen(job.path, job.size)) != nullptr;
                } else {
                    job.ef = epdfont_open(job.path);
                    const bool defaults = std::strcmp(job.file, kBookReaderDefaultFontFile) == 0;
                    if (!job.ef && (!job.selected || defaults)) {
                        job.ef = epdfont_open(kBookFontPath);
                    }
                    if (!job.ef && (!job.selected || defaults)) {
                        job.ef = epdfont_open(kBookFontPathAlt);
                    }
                    job.ok = job.ef != nullptr || (defaults && Book_ItemFont() != nullptr);
                }
            }
        }
    } catch (const std::bad_alloc&) {
        job.ok = false;
        ESP_LOGE("BookFont", "operation=load stage=execute result=no_memory");
    }
    job.done.store(true, std::memory_order_release);
    BookRequestUiWork();
    vTaskDeleteWithCaps(nullptr);
}

static void ClearPreparedFont() {
    s_prepared_file[0] = s_prepared_base_file[0] = '\0';
    s_prepared_size = s_prepared_base_size = 0;
}

// 书库及阅读内容的大号字体。
const lv_font_t* Book_ListFont() {
    return UiFont(UiFontRole::Large);
}

// 书库列表的正文角色字体
const lv_font_t* Book_ItemFont() {
    return UiFont();
}

// 正文字体由 NVS 选择，支持 TTF/OTF 和 .ef。
const lv_font_t* BookFont() {
    if (s_ttf_font) {
        return TtfRuntimeFont(s_ttf_font);
    }
    const lv_font_t* f = epdfont_get_lv_font(s_epdfont);
    return f != nullptr ? f : UiFont();
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
    if (BookReaderPrefsFontFullPath(path, sizeof(path)) != nullptr) {
        if (BookReaderPrefsFontSize() > 0) {
            s_ttf_font = TtfRuntimeOpen(path, BookReaderPrefsFontSize());
            if (s_ttf_font) {
                return;
            }
        } else {
            s_epdfont = epdfont_open(path);
        }
    }
    if (s_epdfont == nullptr) {
        s_epdfont = epdfont_open(kBookFontPath);
    }
    if (s_epdfont == nullptr) {
        s_epdfont = epdfont_open(kBookFontPathAlt);
    }
    if (s_epdfont == nullptr) {
        ESP_LOGW(TAG, "bitmap font unavailable; using embedded font");
    } else {
        ESP_LOGI(TAG, "bitmap font loaded glyphs=%u bpp=%u", static_cast<unsigned>(epdfont_glyph_count(s_epdfont)),
                 epdfont_get_bpp(s_epdfont));
    }
}

bool PrepareBookFont(const char* filename, int size_px) {
    if (!SdIsValidFileName(filename) || (size_px != 0 && (size_px < GetUiResourceProfile().reader_font_min ||
                                                          size_px > GetUiResourceProfile().reader_font_max))) {
        return false;
    }
    const char* extension = std::strrchr(filename, '.');
    const bool vector = extension && (strcasecmp(extension, ".ttf") == 0 || strcasecmp(extension, ".otf") == 0);
    if (!extension || (vector ? size_px == 0 : strcasecmp(extension, ".ef") != 0 || size_px != 0)) {
        return false;
    }
    if (!s_prepared_file[0] && !s_reload && size_px == BookReaderPrefsFontSize() &&
        std::strcmp(filename, BookReaderPrefsFontFile()) == 0) {
        return false;
    }
    ++s_font_request;
    std::snprintf(s_prepared_file, sizeof(s_prepared_file), "%s", filename);
    s_prepared_size = size_px;
    std::snprintf(s_prepared_base_file, sizeof(s_prepared_base_file), "%s", BookReaderPrefsFontFile());
    s_prepared_base_size = BookReaderPrefsFontSize();
    return true;
}

bool ReloadBookFont() {
    TtfRuntime* prepared = nullptr;
    epdfont_t* prepared_ef = nullptr;
    if (s_prepared_file[0] != '\0' && (s_prepared_base_size != BookReaderPrefsFontSize() ||
                                       std::strcmp(s_prepared_base_file, BookReaderPrefsFontFile()) != 0)) {
        ClearPreparedFont();
    }
    if (s_prepared_file[0] != '\0') {
        char path[SD_FILE_PATH_CAPACITY];
        std::snprintf(path, sizeof(path), "%s/%s", s_prepared_size > 0 ? SD_PATH_FONT_SRC : SD_PATH_FONTS,
                      s_prepared_file);
        if (s_prepared_size > 0) {
            prepared = TtfRuntimeOpen(path, s_prepared_size);
        } else {
            prepared_ef = epdfont_open(path);
        }
        const bool defaults = s_prepared_size == 0 && std::strcmp(s_prepared_file, kBookReaderDefaultFontFile) == 0;
        if (!prepared_ef && defaults) {
            prepared_ef = epdfont_open(kBookFontPathAlt);
        }
        if (!prepared && !prepared_ef && !defaults) {
            ESP_LOGE(TAG, "font reload failed kind=%s size=%d", s_prepared_size > 0 ? "vector" : "bitmap",
                     s_prepared_size);
            ClearPreparedFont();
            return false;
        }
        BookReaderPrefsSetFontFile(s_prepared_file, s_prepared_size);
        ClearPreparedFont();
        const auto& session = Book_State().reader.session;
        if (session && session->IsPageImagesMode()) {
            TtfRuntimeClose(prepared);
            if (prepared_ef) {
                epdfont_close(prepared_ef);
            }
            return true;
        }
    }
    ReleaseBookFont();
    if (prepared) {
        s_ttf_font = prepared;
    } else if (prepared_ef) {
        s_epdfont = prepared_ef;
    } else {
        EnsureBookFont();
    }
    return true;
}

bool StartBookFontReload() {
    if (s_reload) {
        return false;
    }
    if (s_prepared_file[0] && (s_prepared_base_size != BookReaderPrefsFontSize() ||
                               std::strcmp(s_prepared_base_file, BookReaderPrefsFontFile()) != 0)) {
        ClearPreparedFont();
    }
    auto job = std::unique_ptr<BookFontRuntimeFontReload>(new (std::nothrow) BookFontRuntimeFontReload);
    if (!job) {
        ESP_LOGE("BookFont", "font request allocation failed");
        return false;
    }
    job->selected = s_prepared_file[0] != '\0';
    job->size = job->selected ? s_prepared_size : BookReaderPrefsFontSize();
    job->base_size = BookReaderPrefsFontSize();
    job->token = s_font_request;
    std::snprintf(job->file, sizeof(job->file), "%s", job->selected ? s_prepared_file : BookReaderPrefsFontFile());
    std::snprintf(job->base_file, sizeof(job->base_file), "%s", BookReaderPrefsFontFile());
    std::snprintf(job->path, sizeof(job->path), "%s/%s", job->size > 0 ? SD_PATH_FONT_SRC : SD_PATH_FONTS, job->file);
    job->unchanged =
        job->selected && s_ttf_font && job->size == job->base_size && std::strcmp(job->file, job->base_file) == 0;
    s_reload = std::move(job);
    if (FirmwareCreateTask(LoadFont, "book_font", 12 * 1024, s_reload.get(), 2, nullptr, tskNO_AFFINITY,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_reload.reset();
        ESP_LOGE("BookFont", "font load task creation failed");
        return false;
    }
    return true;
}

BookFontReloadResult PollBookFontReload(bool apply) {
    if (!s_reload || !s_reload->done.load(std::memory_order_acquire)) {
        return BookFontReloadResult::kPending;
    }
    auto job = std::move(s_reload);
    if (!apply || job->token != s_font_request || job->base_size != BookReaderPrefsFontSize() ||
        std::strcmp(job->base_file, BookReaderPrefsFontFile()) != 0) {
        return BookFontReloadResult::kDiscarded;
    }
    if (!job->ok) {
        ESP_LOGE("BookFont", "font load failed kind=%s size=%d", job->size > 0 ? "vector" : "bitmap", job->size);
        ClearPreparedFont();
        return BookFontReloadResult::kFailed;
    }
    if (job->selected) {
        BookReaderPrefsSetFontFile(job->file, job->size);
    }
    ClearPreparedFont();
    const auto& session = Book_State().reader.session;
    if (!job->unchanged && !(session && session->IsPageImagesMode())) {
        if (s_retired_ttf || s_retired_ef) {
            TtfRuntimeClose(s_ttf_font);
            if (s_epdfont) {
                epdfont_close(s_epdfont);
            }
        } else {
            s_retired_ttf = s_ttf_font;
            s_retired_ef = s_epdfont;
        }
        s_ttf_font = job->ttf;
        s_epdfont = job->ef;
        job->ttf = nullptr;
        job->ef = nullptr;
    }
    return BookFontReloadResult::kApplied;
}

bool BookFontReloadBusy() {
    return s_reload != nullptr;
}

void ReleaseRetiredBookFont() {
    TtfRuntimeClose(s_retired_ttf);
    if (s_retired_ef) {
        epdfont_close(s_retired_ef);
    }
    s_retired_ttf = nullptr;
    s_retired_ef = nullptr;
}

void ReleaseBookFont(bool cancel_prepared) {
    if (cancel_prepared) {
        ClearPreparedFont();
    }
    ReleaseRetiredBookFont();
    TtfRuntimeClose(s_ttf_font);
    s_ttf_font = nullptr;
    if (s_epdfont == nullptr) {
        return;
    }
    epdfont_close(s_epdfont);
    s_epdfont = nullptr;
}

bool PrewarmPageFont(const Page* page, const std::atomic<bool>* cancelled) {
    if (page == nullptr) {
        return true;
    }
    if (s_ttf_font) {
        static bool allocation_error_logged = false;
        try {
            std::vector<uint32_t> codepoints;
            codepoints.reserve(512);
            const auto& session = Book_State().reader.session;
            const bool vertical = session && session->VerticalWriting();
            for (const auto& item : page->items) {
                if (item.kind != ContentKind::kText) {
                    continue;
                }
                for (size_t offset = 0; offset < item.text.size();) {
                    uint32_t cp = 0;
                    const size_t n = Utf8Next(reinterpret_cast<const uint8_t*>(item.text.data()) + offset,
                                              item.text.size() - offset, &cp);
                    if (n == 0) {
                        break;
                    }
                    offset += n;
                    if (cp == '\t') {
                        cp = ' ';
                    }
                    if (cp >= 0x20) {
                        codepoints.push_back(vertical ? VerticalGlyph(cp) : cp);
                    }
                }
            }
            std::sort(codepoints.begin(), codepoints.end());
            codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());
            if (!TtfRuntimePrewarm(s_ttf_font, codepoints.data(), codepoints.size(), cancelled)) {
                return false;
            }
            allocation_error_logged = false;
        } catch (const std::bad_alloc&) {
            if (!allocation_error_logged) {
                ESP_LOGW(TAG, "TTF page prewarm allocation failed; using on-demand glyphs");
            }
            allocation_error_logged = true;
        }
        return !cancelled || !cancelled->load();
    }
    if (!s_epdfont) {
        return true;
    }
    std::string blob;
    blob.reserve(2048);
    for (const auto& item : page->items) {
        if (item.kind == ContentKind::kText) {
            blob.append(item.text);
            blob.push_back('\n');
        }
    }
    if (!blob.empty()) {
        epdfont_prewarm_utf8(s_epdfont, blob.c_str(), blob.size());
    }
    return !cancelled || !cancelled->load();
}

void BookReaderPrefsSyncGlyphAa(int text_visible) {
    const int prev = EpdReaderAaEnabled() ? 1 : 0;
    const int on = text_visible && BookReaderPrefsAntialias() ? 1 : 0;
    EpdSetReaderAaEnabled(on != 0);
    if (prev != on) {
        ESP_LOGI(TAG, "glyph aa=%d (%s)", on, on ? "gray4" : "bw");
    }
}
