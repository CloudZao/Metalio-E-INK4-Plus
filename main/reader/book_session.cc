#include "book_session.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <new>

#include <unistd.h>
#include <sys/stat.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#ifndef EBOOK_EMULATOR
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

#include "book_library.h"
#ifndef EBOOK_EMULATOR
#include "book_home_snapshot.h"
#include "book_progress_cache.h"
#endif
#include "image_util.h"
#include "text_encoding.h"
#include "txt_chapter.h"
#ifndef EBOOK_EMULATOR
#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#endif


namespace reader {
namespace {

#ifndef EBOOK_EMULATOR
const char* UiBlank() { return Lang::Strings::BOOK_BLANK; }
const char* UiReadFileFail() { return Lang::Strings::BOOK_READ_FILE_FAIL; }
const char* UiChapterNoText() { return Lang::Strings::BOOK_CHAPTER_NO_TEXT; }
const char* UiChapterFmt() { return Lang::Strings::BOOK_CHAPTER_FMT; }
#else
const char* UiBlank() { return "(Blank)"; }
const char* UiReadFileFail() { return "(Read failed)"; }
const char* UiChapterNoText() { return "(No text in chapter)"; }
const char* UiChapterFmt() { return "Ch.%d"; }
#endif

constexpr const char* TAG = "BookSession";
constexpr lv_coord_t kImageGap = 8;
constexpr size_t kMaxTxtBytes = 16 * 1024 * 1024;
constexpr size_t kTxtReadChunk = 1024;
constexpr uint32_t kMaxLineSourceBytes = 512;
// v4：v3 + TOC（章节表）落盘
constexpr uint32_t kTxtIdxVersion = 9;  // v9: 章名「第N章」+章节名分行居中
constexpr uint16_t kTxtLineParaStart = 0x8000u;
constexpr uint16_t kTxtLineLenMask = 0x7FFFu;
constexpr size_t kMaxTocEntries = 8000;
constexpr size_t kMaxTocTitleBytes = 96;
// 单章解压合计上限：与 tools/ebook/epdbook/format.py DEVICE_CHAPTER_MAX_UNCOMP 一致。
// 超限拒绝加载；分页 OOM 软失败。转换默认拆到 DEFAULT_CHAPTER_MAX_BYTES(256KB)。
constexpr uint32_t kMaxChapterUncompForPaginate = 384 * 1024;

#pragma pack(push, 1)
struct TxtIdxHeader {
    char magic[4];  // "TXI1"
    uint32_t version;
    uint32_t file_size;
    uint16_t viewport_w;
    uint16_t viewport_h;
    uint16_t line_gap;
    uint16_t para_gap;
    uint32_t font_fp;
    uint8_t as_gbk;
    uint8_t reserved[3];
    uint32_t line_count;
    uint32_t page_count;
};

struct TxtIdxLineDisk {
    uint32_t off;
    uint16_t len;
};
#pragma pack(pop)

const lv_font_t* DefaultFont() {
#ifdef EBOOK_EMULATOR
    return nullptr;
#else
    return fontpack_lv_font_ui();
#endif
}

lv_coord_t FontLineHeight(const lv_font_t* font) {
    return font != nullptr ? font->line_height : 28;
}

lv_coord_t GlyphWidth(const lv_font_t* font, uint32_t cp) {
    if (font == nullptr) {
        return 12;
    }
    const lv_coord_t w = static_cast<lv_coord_t>(lv_font_get_glyph_width(font, cp, 0));
    return w > 0 ? w : 1;
}

bool IsTxtParaIndentWs(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == 0xA0u || cp == 0x3000u;
}

lv_coord_t TxtParaIndentWidth(const lv_font_t* font) {
    // 与渲染侧一致：优先用「一」字宽（字库必有），勿依赖 U+3000
    lv_coord_t w = GlyphWidth(font, 0x4E00u);
    if (w <= 1) {
        w = GlyphWidth(font, 0x3000u);
    }
    if (w <= 1) {
        w = FontLineHeight(font) / 2;
        if (w < 1) {
            w = 12;
        }
    }
    return w * 2;
}

void StripLeadingParaWs(std::string& s) {
    size_t i = 0;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(s.data());
    while (i < s.size()) {
        uint32_t cp = 0;
        const size_t n = Utf8Next(data + i, s.size() - i, &cp);
        if (n == 0 || !IsTxtParaIndentWs(cp)) {
            break;
        }
        i += n;
    }
    if (i > 0) {
        s.erase(0, i);
    }
}

bool EnsurePageVerticalSpace(lv_coord_t& y, Page& cur, std::vector<Page>& pages, lv_coord_t need_h,
                             lv_coord_t viewport_h) {
    if (need_h <= 0) {
        return true;
    }
    if (y + need_h > viewport_h && !cur.items.empty()) {
        try {
            pages.push_back(std::move(cur));
        } catch (const std::bad_alloc&) {
            return false;
        }
        cur = Page{};
        y = 0;
    }
    return true;
}

bool FlushLineToPage(std::string& line, std::vector<Page>& pages, Page& cur, lv_coord_t& y,
                     lv_coord_t line_h, lv_coord_t line_gap, lv_coord_t viewport_h,
                     bool para_indent = false, bool chapter_title = false,
                     bool para_gap_before = false) {
    if (line.empty()) {
        return true;
    }
    const lv_coord_t stride = line_h + line_gap;
    if (!EnsurePageVerticalSpace(y, cur, pages, stride, viewport_h)) {
        return false;
    }
    try {
        PageItem item;
        item.kind = ContentKind::kText;
        item.para_indent = para_indent && !chapter_title;
        item.para_gap_before = para_gap_before;
        item.chapter_title = chapter_title;
        item.text = std::move(line);
        line.clear();
        cur.items.push_back(std::move(item));
    } catch (const std::bad_alloc&) {
        line.clear();
        return false;
    }
    y += stride;
    return true;
}

bool IsPlaceholderChapterTitle(const std::string& t) {
    return t.empty() || t == "未知" || t == "未命名" || t == "Unknown" || t == "Untitled";
}

std::string FormatChapterOrdinalTitle(int n) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), UiChapterFmt(), n > 0 ? n : 1);
    return std::string(buf);
}

// 仅改首行：序数与章节名分行；其余正文不动
void FormatChapterTitleFirstLine(std::string& text) {
    if (text.empty()) {
        return;
    }
    const size_t nl = text.find('\n');
    if (nl == std::string::npos) {
        text = FormatChapterHeadingDisplay(text);
        return;
    }
    std::string first = text.substr(0, nl);
    text = FormatChapterHeadingDisplay(first) + text.substr(nl);
}

// 日志用：截断过长章名
const char* ChapTitleLog(const std::string& s, char* buf, size_t buf_len) {
    if (buf == nullptr || buf_len == 0) {
        return "";
    }
    if (s.empty()) {
        buf[0] = '\0';
        return buf;
    }
    const size_t n = s.size() < buf_len - 1 ? s.size() : buf_len - 1;
    std::memcpy(buf, s.data(), n);
    buf[n] = '\0';
    return buf;
}

// ebook/EPUB：去掉正文首行占位「未知」，再补目录章名加粗。
// 有明确 meta（如「第18章」）时不因正文里另一行「第N章…」而改名。
// 展示：「第N章」单独一行，有章节名则换行紧跟居中。
void EnsureChapterTitleInBlocks(std::string title, int display_1based,
                                std::vector<ContentBlock>& blocks) {
    char tlog[64];
    char flog[64];
    ESP_LOGI(TAG, "chap ensure ch=%d meta='%s' blocks=%u", display_1based,
             ChapTitleLog(title, tlog, sizeof(tlog)), static_cast<unsigned>(blocks.size()));

    if (!blocks.empty() && blocks[0].kind == ContentKind::kText) {
        std::string& text = blocks[0].text;
        StripLeadingParaWs(text);
        const size_t nl = text.find('\n');
        std::string first = (nl == std::string::npos) ? text : text.substr(0, nl);
        while (!first.empty() &&
               (first.back() == ' ' || first.back() == '\t' || first.back() == '\r')) {
            first.pop_back();
        }
        if (IsPlaceholderChapterTitle(first)) {
            ESP_LOGI(TAG, "chap drop placeholder body first='%s'",
                     ChapTitleLog(first, flog, sizeof(flog)));
            if (nl == std::string::npos) {
                blocks.erase(blocks.begin());
            } else {
                text.erase(0, nl + 1);
                StripLeadingParaWs(text);
                if (text.empty()) {
                    blocks.erase(blocks.begin());
                }
            }
        }
    }

    std::string first;
    if (!blocks.empty() && blocks[0].kind == ContentKind::kText) {
        first = blocks[0].text;
        StripLeadingParaWs(first);
        const size_t nl = first.find('\n');
        if (nl != std::string::npos) {
            first.resize(nl);
        }
    }

    // 正文首行与 meta 相同：只打标并分成序数/章名两行
    if (!first.empty() && !IsPlaceholderChapterTitle(title) && first == title) {
        blocks[0].chapter_title = true;
        FormatChapterTitleFirstLine(blocks[0].text);
        ESP_LOGI(TAG, "chap mark body first='%s' via=eq_meta", ChapTitleLog(first, flog, sizeof(flog)));
        return;
    }
    // meta 为空/占位时，才用正文「第N章…」当章名
    if (IsPlaceholderChapterTitle(title) && !first.empty() && IsTxtChapterTitleLine(first)) {
        blocks[0].chapter_title = true;
        FormatChapterTitleFirstLine(blocks[0].text);
        ESP_LOGI(TAG, "chap mark body first='%s' via=IsTxtChapterTitleLine (meta empty)",
                 ChapTitleLog(first, flog, sizeof(flog)));
        return;
    }
    if (IsPlaceholderChapterTitle(title)) {
        title = FormatChapterOrdinalTitle(display_1based);
        ESP_LOGI(TAG, "chap meta placeholder -> '%s'", ChapTitleLog(title, tlog, sizeof(tlog)));
    }
    if (title.empty()) {
        ESP_LOGI(TAG, "chap skip empty title ch=%d", display_1based);
        return;
    }
    if (!first.empty() && IsTxtChapterTitleLine(first) && first != title) {
        ESP_LOGI(TAG, "chap keep meta='%s' ignore body first='%s'",
                 ChapTitleLog(title, tlog, sizeof(tlog)), ChapTitleLog(first, flog, sizeof(flog)));
    }
    ContentBlock blk;
    blk.kind = ContentKind::kText;
    blk.text = FormatChapterHeadingDisplay(title);
    blk.chapter_title = true;
    blocks.insert(blocks.begin(), std::move(blk));
    ESP_LOGI(TAG, "chap prepend '%s' ch=%d", ChapTitleLog(blocks[0].text, tlog, sizeof(tlog)),
             display_1based);
}

// 带回填缓冲的源文件字节流，按码点推进并记录绝对文件偏移。
// 读缓冲必须在堆上：与 StreamPaginateTxt 栈上溢出变量相邻时，1KB 栈 buf 会被编译器
// 与局部/lambda 状态重叠，fread 满缓冲即踩坏分页状态（表现为 epdfont intervals UAF/空指针）。
struct TxtByteStream {
    FILE* fp = nullptr;
    bool as_gbk = false;
    uint8_t* buf = nullptr;
    size_t pos = 0;
    size_t end = 0;
    uint32_t file_base = 0;  // buf[0] 对应的文件偏移
    bool eof = false;

    TxtByteStream() = default;
    ~TxtByteStream() { FreeBuf(); }

    TxtByteStream(const TxtByteStream&) = delete;
    TxtByteStream& operator=(const TxtByteStream&) = delete;

    void FreeBuf() {
        if (buf != nullptr) {
            heap_caps_free(buf);
            buf = nullptr;
        }
    }

    bool InitBuf() {
        FreeBuf();
        buf = static_cast<uint8_t*>(
            heap_caps_malloc(kTxtReadChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (buf == nullptr) {
            buf = static_cast<uint8_t*>(std::malloc(kTxtReadChunk));
        }
        return buf != nullptr;
    }

    uint32_t Tell() const { return file_base + static_cast<uint32_t>(pos); }

    bool Fill() {
        if (eof || buf == nullptr) {
            return false;
        }
        if (pos > 0 && pos < end) {
            const size_t keep = end - pos;
            std::memmove(buf, buf + pos, keep);
            file_base += static_cast<uint32_t>(pos);
            pos = 0;
            end = keep;
        } else if (pos >= end) {
            file_base += static_cast<uint32_t>(end);
            pos = 0;
            end = 0;
        }
        const size_t n = std::fread(buf + end, 1, kTxtReadChunk - end, fp);
        end += n;
        if (n == 0) {
            eof = true;
            return end > pos;
        }
        return true;
    }

    bool Ensure(size_t need) {
        while (end - pos < need) {
            if (eof) {
                return end > pos;
            }
            if (!Fill() && end <= pos) {
                return false;
            }
            if (eof && end - pos < need) {
                return end > pos;
            }
        }
        return true;
    }

    // 跳过 UTF-8 BOM（仅文件开头）
    void SkipBomIfAny() {
        if (file_base != 0 || pos != 0) {
            return;
        }
        if (!Ensure(3)) {
            return;
        }
        if (end - pos >= 3 && buf[pos] == 0xEF && buf[pos + 1] == 0xBB && buf[pos + 2] == 0xBF) {
            pos += 3;
        }
    }

    // 取下一显示字符。skip_cr：吃掉 \r 不产出。返回 false=EOF。
    // out_off/out_len：该字符在源文件中的字节区间（不含被跳过的 \r）。
    bool NextGlyph(uint32_t* cp, uint32_t* out_off, uint16_t* out_len, bool* is_newline) {
        *is_newline = false;
        for (;;) {
            if (!Ensure(1)) {
                return false;
            }
            const uint8_t b = buf[pos];
            if (b == '\r') {
                ++pos;
                continue;
            }
            *out_off = Tell();
            if (b == '\n') {
                ++pos;
                *cp = '\n';
                *out_len = 1;
                *is_newline = true;
                return true;
            }
            if (as_gbk) {
                if (b < 0x80) {
                    ++pos;
                    *cp = b;
                    *out_len = 1;
                    return true;
                }
                if (!Ensure(2)) {
                    // 残缺双字节
                    ++pos;
                    *cp = static_cast<uint32_t>('?');
                    *out_len = 1;
                    return true;
                }
                uint32_t uni = 0;
                const size_t n = GbkNext(buf + pos, end - pos, &uni);
                pos += n;
                *cp = uni;
                *out_len = static_cast<uint16_t>(n);
                return true;
            }
            uint32_t uni = 0;
            // UTF-8 最多 4 字节
            if (!Ensure(4) && end - pos < 1) {
                return false;
            }
            const size_t n = Utf8Next(buf + pos, end - pos, &uni);
            pos += n;
            *cp = uni;
            *out_len = static_cast<uint16_t>(n);
            return true;
        }
    }
};

}  // namespace

void BookSession::ClearTxtIndex() {
    if (txt_fp_ != nullptr) {
        std::fclose(txt_fp_);
        txt_fp_ = nullptr;
    }
    if (txt_lines_ != nullptr) {
        heap_caps_free(txt_lines_);
        txt_lines_ = nullptr;
    }
    txt_lines_len_ = 0;
    txt_lines_cap_ = 0;
    if (txt_page_first_line_ != nullptr) {
        heap_caps_free(txt_page_first_line_);
        txt_page_first_line_ = nullptr;
    }
    txt_page_count_ = 0;
    txt_page_starts_cap_ = 0;
    txt_view_page_ = Page{};
    txt_view_index_ = -1;
    live_preview_active_ = false;
    live_preview_byte_off_ = 0;
    live_preview_at_para_start_ = true;
    live_preview_next_off_ = 0;
    live_preview_next_at_para_start_ = true;
    live_preview_has_next_ = false;
    live_preview_page_ = Page{};
    live_preview_hist_.clear();
    txt_mode_ = false;
    txt_as_gbk_ = false;
    txt_empty_ = false;
    ClearToc();
}

bool BookSession::GrowTxtLines(size_t need) {
    if (need <= txt_lines_cap_) {
        return true;
    }
    // 防止 ncap*=2 溢出成 0 死循环，以及 size 乘法溢出
    constexpr size_t kMaxLines = 2u * 1000u * 1000u;
    if (need > kMaxLines) {
        ESP_LOGE(TAG, "txt lines need=%u over max", static_cast<unsigned>(need));
        return false;
    }
    size_t ncap = txt_lines_cap_ == 0 ? 1024 : txt_lines_cap_;
    while (ncap < need) {
        if (ncap > kMaxLines / 2) {
            ncap = kMaxLines;
            break;
        }
        ncap *= 2;
    }
    if (ncap < need) {
        return false;
    }
    const size_t bytes = ncap * sizeof(TxtLineRef);
    if (bytes / sizeof(TxtLineRef) != ncap) {
        return false;
    }
    auto* np = static_cast<TxtLineRef*>(
        heap_caps_realloc(txt_lines_, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (np == nullptr) {
        np = static_cast<TxtLineRef*>(heap_caps_realloc(txt_lines_, bytes, MALLOC_CAP_8BIT));
    }
    if (np == nullptr) {
        ESP_LOGE(TAG,
                 "txt lines OOM need=%u ncap=%u bytes=%u len=%u int=%u spiram=%u",
                 static_cast<unsigned>(need), static_cast<unsigned>(ncap),
                 static_cast<unsigned>(bytes), static_cast<unsigned>(txt_lines_len_),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        return false;
    }
    txt_lines_ = np;
    txt_lines_cap_ = ncap;
    return true;
}

bool BookSession::GrowTxtPageStarts(size_t need) {
    if (need <= txt_page_starts_cap_) {
        return true;
    }
    constexpr size_t kMaxStarts = 500u * 1000u + 1u;
    if (need > kMaxStarts) {
        ESP_LOGE(TAG, "txt page starts need=%u over max", static_cast<unsigned>(need));
        return false;
    }
    size_t ncap = txt_page_starts_cap_ == 0 ? 64 : txt_page_starts_cap_;
    while (ncap < need) {
        if (ncap > kMaxStarts / 2) {
            ncap = kMaxStarts;
            break;
        }
        ncap *= 2;
    }
    if (ncap < need) {
        return false;
    }
    const size_t bytes = ncap * sizeof(uint32_t);
    if (bytes / sizeof(uint32_t) != ncap) {
        return false;
    }
    auto* np = static_cast<uint32_t*>(
        heap_caps_realloc(txt_page_first_line_, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (np == nullptr) {
        np = static_cast<uint32_t*>(heap_caps_realloc(txt_page_first_line_, bytes, MALLOC_CAP_8BIT));
    }
    if (np == nullptr) {
        ESP_LOGE(TAG,
                 "txt page starts OOM need=%u ncap=%u bytes=%u int=%u spiram=%u",
                 static_cast<unsigned>(need), static_cast<unsigned>(ncap),
                 static_cast<unsigned>(bytes),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        return false;
    }
    txt_page_first_line_ = np;
    txt_page_starts_cap_ = ncap;
    return true;
}

bool BookSession::EnsureTxtFile() const {
    if (sd_handles_detached_) {
        return false;
    }
    if (txt_fp_ != nullptr) {
        return true;
    }
    if (info_.path.empty()) {
        return false;
    }
    txt_fp_ = std::fopen(info_.path.c_str(), "rb");
    return txt_fp_ != nullptr;
}

std::string BookSession::TxtIndexPath(const char* txt_path) {
    if (txt_path == nullptr) {
        return {};
    }
    return std::string(txt_path) + ".idx";
}

std::string BookSession::ProgressPath(const char* book_path) {
    if (book_path == nullptr) {
        return {};
    }
    return std::string(book_path) + ".pos";
}

std::string BookSession::ChapterPageCachePath(const char* book_path) {
    if (book_path == nullptr) {
        return {};
    }
    return std::string(book_path) + ".cpg";
}

namespace {

bool IsProgressMagic(const char magic[4]) {
    return std::memcmp(magic, "POS1", 4) == 0 || std::memcmp(magic, "POS2", 4) == 0 ||
           std::memcmp(magic, "POS3", 4) == 0;
}

bool MagicHasLifetime(const char magic[4]) {
    return std::memcmp(magic, "POS2", 4) == 0 || std::memcmp(magic, "POS3", 4) == 0;
}

bool MagicHasDaily(const char magic[4]) {
    return std::memcmp(magic, "POS3", 4) == 0;
}

/** 单次 fold 上限：防 checkpoint 挂掉 / 浅睡异常导致一次灌入数天秒数 */
constexpr int64_t kMaxFoldDeltaUs = 6LL * 3600LL * 1000000LL;

#pragma pack(push, 1)
struct Pos3Blob {
    char magic[4];
    uint32_t chapter;
    uint32_t page;
    uint32_t pct_x10;
    uint32_t reading_seconds;
    uint32_t day_id;
    uint32_t daily_seconds;
};
#pragma pack(pop)
static_assert(sizeof(Pos3Blob) == 28, "POS3 on-disk size");

}  // namespace

BookSession::PosLoadStatus BookSession::LoadPosFileOnce(const char* pos_path, PosFileData& out) {
    out = {};
    if (pos_path == nullptr || pos_path[0] == '\0') {
        return PosLoadStatus::kMissing;
    }
    FILE* fp = std::fopen(pos_path, "rb");
    if (fp == nullptr) {
        return PosLoadStatus::kMissing;
    }
    const bool head_ok =
        std::fread(out.magic, 1, 4, fp) == 4 &&
        std::fread(&out.chapter, sizeof(out.chapter), 1, fp) == 1 &&
        std::fread(&out.page, sizeof(out.page), 1, fp) == 1;
    if (!head_ok || !IsProgressMagic(out.magic)) {
        std::fclose(fp);
        return PosLoadStatus::kInvalid;
    }
    // 可选尾部：缺字段不视为整文件坏（兼容 POS1 短文件）
    out.has_pct = std::fread(&out.pct_x10, sizeof(out.pct_x10), 1, fp) == 1;
    if (out.has_pct) {
        out.has_seconds = std::fread(&out.reading_seconds, sizeof(out.reading_seconds), 1, fp) == 1;
    }
    if (out.has_seconds) {
        out.has_daily = std::fread(&out.day_id, sizeof(out.day_id), 1, fp) == 1 &&
                        std::fread(&out.daily_seconds, sizeof(out.daily_seconds), 1, fp) == 1;
    }
    std::fclose(fp);

    // 按 magic 语义收窄：旧版即使误读到多字节也不采用
    if (!MagicHasLifetime(out.magic)) {
        out.has_seconds = false;
        out.reading_seconds = 0;
        out.has_daily = false;
        out.day_id = 0;
        out.daily_seconds = 0;
    } else if (!MagicHasDaily(out.magic)) {
        out.has_daily = false;
        out.day_id = 0;
        out.daily_seconds = 0;
    }
    return PosLoadStatus::kOk;
}

BookSession::PosLoadStatus BookSession::LoadPosFile(const char* pos_path, PosFileData& out) {
    const PosLoadStatus st = LoadPosFileOnce(pos_path, out);
    if (st != PosLoadStatus::kMissing) {
        return st;
    }
    // 原子写失败窗口：.pos 已 unlink、.pos.tmp 尚未 rename → 从 .tmp 恢复
    if (pos_path == nullptr || pos_path[0] == '\0') {
        return PosLoadStatus::kMissing;
    }
    const std::string tmp = std::string(pos_path) + ".tmp";
    return LoadPosFileOnce(tmp.c_str(), out);
}

uint32_t BookSession::LocalCalendarDayId() {
    // 与首页时钟门禁一致：未对时（年 < 2025）不可信，返回 0
    const time_t now = time(nullptr);
    if (now == static_cast<time_t>(-1)) {
        return 0;
    }
    struct tm tm_info = {};
    if (localtime_r(&now, &tm_info) == nullptr) {
        return 0;
    }
    if (tm_info.tm_year < (2025 - 1900)) {
        return 0;
    }
    const int year = tm_info.tm_year + 1900;
    const int mon = tm_info.tm_mon + 1;
    const int day = tm_info.tm_mday;
    if (year < 2025 || year > 2099 || mon < 1 || mon > 12 || day < 1 || day > 31) {
        return 0;
    }
    return static_cast<uint32_t>(year * 10000 + mon * 100 + day);
}

uint32_t BookSession::DailySecondsIfToday(const PosFileData& data) {
    if (!data.has_daily) {
        return 0;
    }
    const uint32_t today = LocalCalendarDayId();
    if (today == 0 || data.day_id != today) {
        return 0;
    }
    return data.daily_seconds;
}

void BookSession::AddReadingSecondsSaturated(uint32_t delta_sec) {
    if (delta_sec == 0) {
        return;
    }
    if (reading_seconds_ > UINT32_MAX - delta_sec) {
        reading_seconds_ = UINT32_MAX;
    } else {
        reading_seconds_ += delta_sec;
    }
}

void BookSession::AddDailySecondsSaturated(uint32_t delta_sec) {
    if (delta_sec == 0) {
        return;
    }
    if (daily_seconds_ > UINT32_MAX - delta_sec) {
        daily_seconds_ = UINT32_MAX;
    } else {
        daily_seconds_ += delta_sec;
    }
}

void BookSession::EnsureDailyBucket() {
    const uint32_t today = LocalCalendarDayId();
    if (today == 0) {
        // 时钟无效：不换日、不改 day_id，避免把有效日戳冲成 0
        return;
    }
    if (reading_day_id_ != today) {
        reading_day_id_ = today;
        daily_seconds_ = 0;
    }
}

void BookSession::FoldReadingClock() {
    if (!reading_clock_active_) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    int64_t delta_us = now - reading_clock_since_us_;
    if (delta_us < 0) {
        // 时钟回拨：丢弃异常段，从 now 重新计
        reading_clock_since_us_ = now;
        return;
    }
    if (delta_us > kMaxFoldDeltaUs) {
        ESP_LOGW(TAG, "reading clock fold clamp %lld us (max %lld)",
                 static_cast<long long>(delta_us), static_cast<long long>(kMaxFoldDeltaUs));
        delta_us = kMaxFoldDeltaUs;
    }
    const uint32_t sec = static_cast<uint32_t>(delta_us / 1000000LL);
    if (sec > 0) {
        // 先换日再累加：跨午夜的一小段（≤ checkpoint）记入新日，可接受
        const uint32_t today = LocalCalendarDayId();
        if (today != 0) {
            if (reading_day_id_ != today) {
                reading_day_id_ = today;
                daily_seconds_ = 0;
            }
            AddReadingSecondsSaturated(sec);
            AddDailySecondsSaturated(sec);
        } else {
            // 未对时：只累终身，不动当日桶
            AddReadingSecondsSaturated(sec);
        }
        // 保留不足 1 秒的余量，避免多次 fold 丢精度
        reading_clock_since_us_ = now - (delta_us % 1000000LL);
    }
}

void BookSession::StartReadingClock() {
    if (!open_) {
        return;
    }
    if (reading_clock_active_) {
        return;
    }
    // 开钟前对齐当日桶，避免昨日内存日戳带到今日首段
    EnsureDailyBucket();
    reading_clock_active_ = true;
    reading_clock_since_us_ = esp_timer_get_time();
}

void BookSession::StopReadingClock() {
    FoldReadingClock();
    reading_clock_active_ = false;
    reading_clock_since_us_ = 0;
}

bool BookSession::SaveProgress() {
    if (!open_ || info_.path.empty()) {
        return false;
    }
    FoldReadingClock();
    EnsureDailyBucket();
    const std::string path = ProgressPath(info_.path.c_str());
    if (path.empty()) {
        return false;
    }

    // 整块写出 + WriteFileAtomic：避免 fopen("wb") 先截断再写失败导致进度/时长归零
    Pos3Blob blob{};
    blob.magic[0] = 'P';
    blob.magic[1] = 'O';
    blob.magic[2] = 'S';
    blob.magic[3] = '3';
    blob.chapter = static_cast<uint32_t>(std::max(0, chapter_index_));
    blob.page = static_cast<uint32_t>(std::max(0, page_index_));
    blob.pct_x10 = static_cast<uint32_t>(std::max(0, ReadingProgressX10()));
    blob.reading_seconds = reading_seconds_;
    blob.day_id = reading_day_id_;
    blob.daily_seconds = daily_seconds_;

    const bool ok =
        WriteFileAtomic(path.c_str(), reinterpret_cast<const uint8_t*>(&blob), sizeof(blob));
    if (!ok) {
        ESP_LOGW(TAG, "progress save fail: %s", path.c_str());
        return false;
    }
#ifndef EBOOK_EMULATOR
    // 首页「最近阅读」MRU：仅进度真正落盘后轮换；已在队首则内部跳过写 NVS
    book_home_snapshot::NoteOpenedBook(info_.path.c_str());
    {
        BookSession::ProgressPeek snap;
        snap.progress_x10 = static_cast<int>(blob.pct_x10);
        snap.reading_seconds = blob.reading_seconds;
        snap.daily_seconds = blob.daily_seconds;
        book_progress_cache::Upsert(info_.path.c_str(), snap);
    }
#endif
    ESP_LOGD(TAG, "progress saved %s ch=%u pg=%u pct=%u.%u sec=%u day=%u daily=%u", path.c_str(),
             static_cast<unsigned>(blob.chapter), static_cast<unsigned>(blob.page),
             static_cast<unsigned>(blob.pct_x10 / 10), static_cast<unsigned>(blob.pct_x10 % 10),
             static_cast<unsigned>(blob.reading_seconds), static_cast<unsigned>(blob.day_id),
             static_cast<unsigned>(blob.daily_seconds));
    return true;
}

int BookSession::PeekReadingProgressX10(const char* book_path) {
    if (book_path == nullptr || book_path[0] == '\0') {
        return -1;
    }
    PosFileData data;
    if (LoadPosFile(ProgressPath(book_path).c_str(), data) != PosLoadStatus::kOk) {
        return -1;
    }
    // 旧版 .pos 无百分比字段：视为尚无可展示进度
    if (!data.has_pct) {
        return -1;
    }
    uint32_t pct_x10 = data.pct_x10;
    if (pct_x10 > 1000) {
        pct_x10 = 1000;
    }
    return static_cast<int>(pct_x10);
}

uint32_t BookSession::PeekReadingSeconds(const char* book_path) {
    if (book_path == nullptr || book_path[0] == '\0') {
        return 0;
    }
    PosFileData data;
    if (LoadPosFile(ProgressPath(book_path).c_str(), data) != PosLoadStatus::kOk) {
        return 0;
    }
    if (!data.has_seconds) {
        return 0;
    }
    return data.reading_seconds;
}

uint32_t BookSession::PeekReadingDailySeconds(const char* book_path) {
    if (book_path == nullptr || book_path[0] == '\0') {
        return 0;
    }
    PosFileData data;
    if (LoadPosFile(ProgressPath(book_path).c_str(), data) != PosLoadStatus::kOk) {
        return 0;
    }
    return DailySecondsIfToday(data);
}

BookSession::ProgressPeek BookSession::PeekProgress(const char* book_path) {
    ProgressPeek out;
    if (book_path == nullptr || book_path[0] == '\0') {
        return out;
    }
    PosFileData data;
    if (LoadPosFile(ProgressPath(book_path).c_str(), data) != PosLoadStatus::kOk) {
        return out;
    }
    if (data.has_pct) {
        uint32_t pct_x10 = data.pct_x10;
        if (pct_x10 > 1000) {
            pct_x10 = 1000;
        }
        out.progress_x10 = static_cast<int>(pct_x10);
    }
    if (data.has_seconds) {
        out.reading_seconds = data.reading_seconds;
    }
    out.daily_seconds = DailySecondsIfToday(data);
    return out;
}

bool BookSession::ReadProgressPos(uint32_t& chapter, uint32_t& page, uint32_t* seconds_out,
                                  uint32_t* day_id_out, uint32_t* daily_out) {
    chapter = 0;
    page = 0;
    if (seconds_out != nullptr) {
        *seconds_out = 0;
    }
    if (day_id_out != nullptr) {
        *day_id_out = 0;
    }
    if (daily_out != nullptr) {
        *daily_out = 0;
    }
    if (info_.path.empty()) {
        return false;
    }
    const std::string path = ProgressPath(info_.path.c_str());
    PosFileData data;
    const PosLoadStatus st = LoadPosFile(path.c_str(), data);
    if (st == PosLoadStatus::kMissing) {
        return false;
    }
    if (st != PosLoadStatus::kOk) {
        ESP_LOGW(TAG, "progress bad file: %s", path.c_str());
        return false;
    }
    chapter = data.chapter;
    page = data.page;
    if (seconds_out != nullptr && data.has_seconds) {
        *seconds_out = data.reading_seconds;
    }
    if (day_id_out != nullptr && data.has_daily) {
        *day_id_out = data.day_id;
    }
    if (daily_out != nullptr && data.has_daily) {
        *daily_out = data.daily_seconds;
    }
    return true;
}

uint32_t BookSession::FontFingerprint() const {
    // 行高 + 若干样例字宽：换字体/字号后自动失效缓存
    uint32_t h = static_cast<uint32_t>(FontLineHeight(font_));
    static const uint32_t kSamples[] = {
        0x4E2Du,  // 中
        0x6587u,  // 文
        static_cast<uint32_t>('a'),
        static_cast<uint32_t>('0'),
        0xFF0Cu,  // ，
        0x3002u,  // 。
        0x201Cu,  // “
    };
    for (uint32_t cp : kSamples) {
        h = h * 131u + static_cast<uint32_t>(GlyphWidth(font_, cp));
    }
    h = h * 131u + static_cast<uint32_t>(viewport_w_);
    h = h * 131u + static_cast<uint32_t>(viewport_h_);
    return h;
}

bool BookSession::TryLoadTxtIndex(const char* txt_path, size_t file_size) {
    const std::string idx_path = TxtIndexPath(txt_path);
    FILE* ifp = std::fopen(idx_path.c_str(), "rb");
    if (ifp == nullptr) {
        return false;
    }

    TxtIdxHeader hdr{};
    if (std::fread(&hdr, 1, sizeof(hdr), ifp) != sizeof(hdr)) {
        std::fclose(ifp);
        return false;
    }
    if (std::memcmp(hdr.magic, "TXI1", 4) != 0 || hdr.version != kTxtIdxVersion) {
        std::fclose(ifp);
        ESP_LOGW(TAG, "txt idx bad magic/ver: %s", idx_path.c_str());
        return false;
    }
    if (hdr.file_size != static_cast<uint32_t>(file_size) ||
        hdr.viewport_w != static_cast<uint16_t>(viewport_w_) ||
        hdr.viewport_h != static_cast<uint16_t>(viewport_h_) ||
        hdr.line_gap != static_cast<uint16_t>(line_gap_) ||
        hdr.para_gap != static_cast<uint16_t>(para_gap_) ||
        hdr.font_fp != FontFingerprint() ||
        hdr.as_gbk != static_cast<uint8_t>(txt_as_gbk_ ? 1 : 0) ||
        hdr.reserved[0] != 0 ||
        hdr.line_count == 0 || hdr.page_count == 0) {
        std::fclose(ifp);
        ESP_LOGI(TAG, "txt idx stale, will rebuild: %s", idx_path.c_str());
        return false;
    }
    // 合理上限，防止坏文件拖垮内存
    if (hdr.line_count > 2 * 1000 * 1000 || hdr.page_count > 500 * 1000) {
        std::fclose(ifp);
        ESP_LOGW(TAG, "txt idx absurd counts L=%u P=%u", static_cast<unsigned>(hdr.line_count),
                 static_cast<unsigned>(hdr.page_count));
        return false;
    }

    auto fail_cleanup = [&]() {
        std::fclose(ifp);
        // 半加载状态不可用，清长度避免误用；容量留给后续 StreamPaginate 复用
        txt_lines_len_ = 0;
        txt_page_count_ = 0;
        ClearToc();
    };

    if (!GrowTxtLines(hdr.line_count) || !GrowTxtPageStarts(hdr.page_count + 1)) {
        fail_cleanup();
        ESP_LOGE(TAG, "txt idx OOM L=%u P=%u", static_cast<unsigned>(hdr.line_count),
                 static_cast<unsigned>(hdr.page_count));
        return false;
    }

    for (uint32_t i = 0; i < hdr.line_count; ++i) {
        TxtIdxLineDisk rec{};
        if (std::fread(&rec, 1, sizeof(rec), ifp) != sizeof(rec)) {
            fail_cleanup();
            return false;
        }
        txt_lines_[i].off = rec.off;
        txt_lines_[i].len = rec.len;
    }
    txt_lines_len_ = hdr.line_count;

    if (std::fread(txt_page_first_line_, sizeof(uint32_t), hdr.page_count + 1, ifp) !=
        hdr.page_count + 1) {
        fail_cleanup();
        return false;
    }
    // 校验哨兵
    if (txt_page_first_line_[0] != 0 ||
        txt_page_first_line_[hdr.page_count] != hdr.line_count) {
        fail_cleanup();
        ESP_LOGW(TAG, "txt idx page table mismatch");
        return false;
    }
    txt_page_count_ = hdr.page_count;

    ClearToc();
    uint32_t toc_count = 0;
    if (std::fread(&toc_count, sizeof(toc_count), 1, ifp) != 1) {
        fail_cleanup();
        return false;
    }
    if (toc_count > kMaxTocEntries) {
        fail_cleanup();
        ESP_LOGW(TAG, "txt idx toc absurd %u", static_cast<unsigned>(toc_count));
        return false;
    }
    toc_.reserve(toc_count);
    for (uint32_t i = 0; i < toc_count; ++i) {
        uint32_t first_line = 0;
        uint16_t title_len = 0;
        if (std::fread(&first_line, sizeof(first_line), 1, ifp) != 1 ||
            std::fread(&title_len, sizeof(title_len), 1, ifp) != 1 || title_len > kMaxTocTitleBytes) {
            fail_cleanup();
            return false;
        }
        if (first_line >= hdr.line_count) {
            fail_cleanup();
            return false;
        }
        std::string title(title_len, '\0');
        if (title_len > 0 &&
            std::fread(title.data(), 1, title_len, ifp) != title_len) {
            fail_cleanup();
            return false;
        }
        TocEntry ent;
        ent.title = std::move(title);
        ent.first_line = first_line;
        toc_.push_back(std::move(ent));
    }
    FinalizeTocPages();

    std::fclose(ifp);
    ESP_LOGI(TAG, "txt idx hit %s lines=%u pages=%u toc=%u", idx_path.c_str(),
             static_cast<unsigned>(txt_lines_len_), static_cast<unsigned>(txt_page_count_),
             static_cast<unsigned>(toc_.size()));
    return true;
}

bool BookSession::SaveTxtIndex(const char* txt_path, size_t file_size) const {
    if (txt_path == nullptr || txt_lines_ == nullptr || txt_page_first_line_ == nullptr ||
        txt_page_count_ == 0 || txt_lines_len_ == 0) {
        return false;
    }
    const std::string idx_path = TxtIndexPath(txt_path);
    FILE* ofp = std::fopen(idx_path.c_str(), "wb");
    if (ofp == nullptr) {
        ESP_LOGW(TAG, "txt idx write open fail: %s", idx_path.c_str());
        return false;
    }

    TxtIdxHeader hdr{};
    std::memcpy(hdr.magic, "TXI1", 4);
    hdr.version = kTxtIdxVersion;
    hdr.file_size = static_cast<uint32_t>(file_size);
    hdr.viewport_w = static_cast<uint16_t>(viewport_w_);
    hdr.viewport_h = static_cast<uint16_t>(viewport_h_);
    hdr.line_gap = static_cast<uint16_t>(line_gap_);
    hdr.para_gap = static_cast<uint16_t>(para_gap_);
    hdr.font_fp = FontFingerprint();
    hdr.as_gbk = txt_as_gbk_ ? 1 : 0;
    hdr.reserved[0] = 0;
    hdr.line_count = static_cast<uint32_t>(txt_lines_len_);
    hdr.page_count = static_cast<uint32_t>(txt_page_count_);

    auto fail_write = [&]() {
        std::fclose(ofp);
        std::remove(idx_path.c_str());  // 避免留下半截 .idx 被误加载
    };

    if (std::fwrite(&hdr, 1, sizeof(hdr), ofp) != sizeof(hdr)) {
        fail_write();
        return false;
    }
    for (size_t i = 0; i < txt_lines_len_; ++i) {
        TxtIdxLineDisk rec{txt_lines_[i].off, txt_lines_[i].len};
        if (std::fwrite(&rec, 1, sizeof(rec), ofp) != sizeof(rec)) {
            fail_write();
            return false;
        }
    }
    if (std::fwrite(txt_page_first_line_, sizeof(uint32_t), txt_page_count_ + 1, ofp) !=
        txt_page_count_ + 1) {
        fail_write();
        return false;
    }
    const uint32_t toc_count = static_cast<uint32_t>(toc_.size());
    if (std::fwrite(&toc_count, sizeof(toc_count), 1, ofp) != 1) {
        fail_write();
        return false;
    }
    for (const auto& ent : toc_) {
        uint16_t title_len = static_cast<uint16_t>(
            ent.title.size() > kMaxTocTitleBytes ? kMaxTocTitleBytes : ent.title.size());
        if (std::fwrite(&ent.first_line, sizeof(ent.first_line), 1, ofp) != 1 ||
            std::fwrite(&title_len, sizeof(title_len), 1, ofp) != 1) {
            fail_write();
            return false;
        }
        if (title_len > 0 &&
            std::fwrite(ent.title.data(), 1, title_len, ofp) != title_len) {
            fail_write();
            return false;
        }
    }
    std::fclose(ofp);
    ESP_LOGI(TAG, "txt idx saved %s lines=%u pages=%u toc=%u", idx_path.c_str(),
             static_cast<unsigned>(txt_lines_len_), static_cast<unsigned>(txt_page_count_),
             static_cast<unsigned>(toc_count));
    return true;
}

void BookSession::ReportOpenProgress(int percent) {
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    // 节流：至少 +10% 或到 100，避免墨水屏刷新过密
    if (last_progress_pct_ >= 0 && percent < 100 && percent < last_progress_pct_ + 10) {
        return;
    }
    last_progress_pct_ = percent;
    if (progress_fn_ != nullptr) {
        progress_fn_(percent, progress_user_);
    }
}

BookSession::~BookSession() {
    Close();
}

void BookSession::DetachSdHandles() {
    sd_handles_detached_ = true;
    if (txt_fp_ != nullptr) {
        std::fclose(txt_fp_);
        txt_fp_ = nullptr;
    }
    txt_view_index_ = -1; // 醒后须重新 Materialize
    if (epub_) {
        epub_->DetachSd();
    }
    if (ebook_) {
        ebook_->DetachSd();
    }
}

bool BookSession::ReattachSdHandles() {
    sd_handles_detached_ = false;
    bool ok = true;
    if (txt_mode_ && !info_.path.empty()) {
        if (!EnsureTxtFile()) {
            ESP_LOGW(TAG, "reattach txt fail path=%s", info_.path.c_str());
            ok = false;
        }
    }
    if (epub_ != nullptr && !epub_->ReattachSd()) {
        ESP_LOGW(TAG, "reattach epub fail");
        ok = false;
    }
    if (ebook_ != nullptr && !ebook_->ReattachSd()) {
        ESP_LOGW(TAG, "reattach ebook fail");
        ok = false;
    }
    return ok;
}

void BookSession::Close() {
    if (open_) {
        SaveProgress();  // fold 活跃时钟并写 POS3
    }
    StopReadingClock();
    reading_seconds_ = 0;
    daily_seconds_ = 0;
    reading_day_id_ = 0;
    if (epub_) {
        epub_->Close();
        epub_.reset();
    }
    if (ebook_) {
        ebook_->Close();
        ebook_.reset();
    }
    ebook_spine_.clear();
    page_images_mode_ = false;
    ClearTxtIndex();
    ClearToc();
    pages_.clear();
    pages_.shrink_to_fit();
    page_index_ = 0;
    chapter_index_ = 0;
    chapter_count_ = 1;
    chapter_page_counts_.clear();
    chapter_page_counts_.shrink_to_fit();
    chapter_pages_ready_ = false;
    chapter_pages_gen_ = 0;
    cover_.Reset();
    cover_tried_ = false;
    cached_href_.clear();
    cached_image_.Reset();
    live_preview_active_ = false;
    live_preview_byte_off_ = 0;
    live_preview_at_para_start_ = true;
    live_preview_next_off_ = 0;
    live_preview_next_at_para_start_ = true;
    live_preview_has_next_ = false;
    live_preview_page_ = Page{};
    live_preview_hist_.clear();
    open_ = false;
    info_ = {};
    display_title_.clear();
    display_author_.clear();
}

bool BookSession::Open(const BookInfo& info) {
    Close();
    last_progress_pct_ = -1;
    ReportOpenProgress(0);
    if (font_ == nullptr) {
        font_ = DefaultFont();
    }
    info_ = info;
    display_title_ = info.title;
    bool ok = false;
    if (info.format == BookFormat::kTxt) {
        ok = OpenTxt(info.path.c_str(), info.file_size);
    } else if (info.format == BookFormat::kEpub) {
        ok = OpenEpub(info.path.c_str());
    } else if (info.format == BookFormat::kEbook) {
        ok = OpenEbook(info.path.c_str());
    } else {
        ESP_LOGE(TAG, "unknown format");
    }
    open_ = ok;
    if (!ok) {
        Close();
        return false;
    }
    ReportOpenProgress(95);
    uint32_t prog_ch = 0;
    uint32_t prog_page = 0;
    uint32_t prog_seconds = 0;
    uint32_t prog_day_id = 0;
    uint32_t prog_daily = 0;
    const bool has_prog =
        ReadProgressPos(prog_ch, prog_page, &prog_seconds, &prog_day_id, &prog_daily);
    // 尽早恢复时长，避免后续章加载失败走 Close 时把秒数写成 0
    reading_seconds_ = prog_seconds;
    reading_day_id_ = prog_day_id;
    daily_seconds_ = prog_daily;
    // 懒换日：跨日打开只清内存当日；下次 Save 落盘。终身累计不动。
    EnsureDailyBucket();
    reading_clock_active_ = false;
    reading_clock_since_us_ = 0;
    if (!txt_mode_) {
        int ch = has_prog ? static_cast<int>(prog_ch) : 0;
        if (ch < 0 || ch >= chapter_count_) {
            ch = 0;
        }
        bool loaded = false;
        if (epub_ != nullptr) {
            loaded = LoadEpubChapter(ch);
        } else if (ebook_ != nullptr) {
            loaded = LoadEbookChapter(ch);
        }
        if (!loaded) {
            Close();
            return false;
        }
        // 全书页表：打开只读 .cpg；未命中则估算，后台 FinishChapterPageTable 补齐
        WarmChapterPageTableFromCache();
    }
    if (has_prog) {
        const int pc = PageCount();
        if (pc > 0) {
            if (prog_page >= static_cast<uint32_t>(pc)) {
                prog_page = static_cast<uint32_t>(pc - 1);
            }
            page_index_ = static_cast<int>(prog_page);
            ESP_LOGI(TAG, "progress restored chapter=%d page=%d/%d book=%d/%d", chapter_index_ + 1,
                     page_index_ + 1, PageCount(), BookCurrentPage() + 1, BookPageCount());
        }
    }
    ReportOpenProgress(100);
    return true;
}

int BookSession::PageCount() const {
    if (txt_mode_) {
        return static_cast<int>(txt_page_count_);
    }
    return static_cast<int>(pages_.size());
}

int BookSession::BookPageCount() const {
    if (!IsChapteredBook() || chapter_count_ <= 1) {
        return std::max(1, PageCount());
    }
    if (chapter_page_counts_.size() == static_cast<size_t>(chapter_count_)) {
        int64_t sum = 0;
        for (uint16_t n : chapter_page_counts_) {
            sum += std::max<uint16_t>(1, n);
        }
        if (sum > 0) {
            return static_cast<int>(sum);
        }
    }
    // 未就绪：沿用「本章页数 × 章数」估算
    return std::max(1, ChapterCount()) * std::max(1, PageCount());
}

int BookSession::BookCurrentPage() const {
    if (!IsChapteredBook() || chapter_count_ <= 1) {
        return CurrentPage();
    }
    if (chapter_page_counts_.size() != static_cast<size_t>(chapter_count_)) {
        // 估算：假定每章页数=本章
        const int pc = std::max(1, PageCount());
        return chapter_index_ * pc + CurrentPage();
    }
    int64_t before = 0;
    const int ch = std::clamp(chapter_index_, 0, chapter_count_ - 1);
    for (int i = 0; i < ch; ++i) {
        before += std::max<uint16_t>(1, chapter_page_counts_[static_cast<size_t>(i)]);
    }
    return static_cast<int>(before + CurrentPage());
}

void BookSession::MaterializeTxtPage(int page) const {
    txt_view_page_.items.clear();
    if (!txt_mode_ || page < 0 || static_cast<size_t>(page) >= txt_page_count_ ||
        txt_page_first_line_ == nullptr || txt_lines_ == nullptr) {
        txt_view_index_ = -1;
        return;
    }
    if (txt_empty_) {
        PageItem item;
        item.kind = ContentKind::kText;
        item.text = UiBlank();
        txt_view_page_.items.push_back(std::move(item));
        txt_view_index_ = page;
        return;
    }
    if (!EnsureTxtFile()) {
        PageItem item;
        item.kind = ContentKind::kText;
        item.text = UiReadFileFail();
        txt_view_page_.items.push_back(std::move(item));
        txt_view_index_ = page;
        return;
    }

    const uint32_t begin = txt_page_first_line_[static_cast<size_t>(page)];
    const uint32_t end = txt_page_first_line_[static_cast<size_t>(page) + 1];
    if (end < begin || end > txt_lines_len_) {
        ESP_LOGW(TAG, "txt page table OOB page=%d begin=%u end=%u lines=%u", page,
                 static_cast<unsigned>(begin), static_cast<unsigned>(end),
                 static_cast<unsigned>(txt_lines_len_));
        txt_view_index_ = -1;
        return;
    }
    txt_view_page_.items.reserve(end > begin ? (end - begin) : 0);

    auto read_line_utf8 = [&](uint32_t idx, std::string& out) -> bool {
        const TxtLineRef& lr = txt_lines_[idx];
        const uint16_t raw_len = static_cast<uint16_t>(lr.len & kTxtLineLenMask);
        out.clear();
        if (raw_len == 0) {
            return true;
        }
        if (std::fseek(txt_fp_, static_cast<long>(lr.off), SEEK_SET) != 0) {
            out = "?";
            return false;
        }
        uint8_t raw[kMaxLineSourceBytes + 4];
        const size_t n = std::fread(raw, 1, raw_len, txt_fp_);
        if (!BytesToUtf8(raw, n, txt_as_gbk_, out)) {
            out = "?";
            return false;
        }
        return true;
    };

    // 跨页续行：若本页首行非段首，回看段首是否章名
    bool in_chap_title = false;
    if (begin > 0) {
        uint32_t j = begin;
        while (j > 0 && (txt_lines_[j].len & kTxtLineParaStart) == 0) {
            --j;
        }
        if ((txt_lines_[j].len & kTxtLineParaStart) != 0) {
            std::string head;
            if (read_line_utf8(j, head)) {
                StripLeadingParaWs(head);
                in_chap_title = !head.empty() && IsTxtChapterTitleLine(head);
            }
        }
    }

    for (uint32_t i = begin; i < end; ++i) {
        const TxtLineRef& lr = txt_lines_[i];
        PageItem item;
        item.kind = ContentKind::kText;
        const bool para_start = (lr.len & kTxtLineParaStart) != 0;
        if ((lr.len & kTxtLineLenMask) == 0) {
            continue;
        }
        if (!read_line_utf8(i, item.text)) {
            // keep "?"
        } else if (para_start) {
            StripLeadingParaWs(item.text);
            item.para_gap_before = true;
            if (!item.text.empty() && IsTxtChapterTitleLine(item.text)) {
                item.chapter_title = true;
                in_chap_title = true;
                // 折行续段并入同一 item，避免章名只显示半截（底栏 TOC 是整段）
                while (i + 1 < end && (txt_lines_[i + 1].len & kTxtLineParaStart) == 0) {
                    ++i;
                    std::string cont;
                    if (!read_line_utf8(i, cont) || cont.empty()) {
                        continue;
                    }
                    item.text += cont;
                }
                item.text = FormatChapterHeadingDisplay(item.text);
                ESP_LOGI(TAG, "chap txt page=%d mark len=%u '%.*s'", page + 1,
                         static_cast<unsigned>(item.text.size()), 64, item.text.c_str());
            } else {
                in_chap_title = false;
                if (!item.text.empty()) {
                    item.para_indent = true;
                }
            }
        } else if (in_chap_title) {
            // 章名跨页：本页续行并入（同段）后按序数/章名重排
            item.chapter_title = true;
            while (i + 1 < end && (txt_lines_[i + 1].len & kTxtLineParaStart) == 0) {
                ++i;
                std::string cont;
                if (!read_line_utf8(i, cont) || cont.empty()) {
                    continue;
                }
                item.text += cont;
            }
            item.text = FormatChapterHeadingDisplay(item.text);
        }
        txt_view_page_.items.push_back(std::move(item));
    }
    txt_view_index_ = page;
}

bool BookSession::StreamPaginateTxt(FILE* fp, size_t file_size) {
    txt_lines_len_ = 0;
    txt_page_count_ = 0;
    txt_view_index_ = -1;
    txt_view_page_ = Page{};
    // 编排中保留旧 toc_ 供底栏/目录；新目录写入临时表，结束时再替换
    std::vector<TocEntry> built_toc;
    struct TocBuildClear {
        BookSession* self;
        ~TocBuildClear() { self->toc_build_ = nullptr; }
    } toc_build_clear{this};
    toc_build_ = &built_toc;

    if (!GrowTxtPageStarts(2)) {
        ESP_LOGE(TAG, "txt page index OOM int=%u spiram=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        return false;
    }

    const size_t est_lines = file_size / 24 + 256;
    if (!GrowTxtLines(est_lines) || !GrowTxtPageStarts(est_lines / 16 + 32)) {
        ESP_LOGW(TAG, "txt index prealloc partial need_lines=%u int=%u spiram=%u",
                 static_cast<unsigned>(est_lines),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    }

    txt_page_first_line_[0] = 0;
    size_t page_starts_len = 1;

    const lv_coord_t line_h = FontLineHeight(font_);
    const lv_coord_t indent_w = TxtParaIndentWidth(font_);
    lv_coord_t y = 0;
    uint32_t line_off = 0;
    uint32_t line_len = 0;
    lv_coord_t line_w = 0;
    bool line_started = false;
    bool line_para_start = false;  // 当前行是否为段首行（写入 len 高位）
    bool at_para_start = true;     // 下一非空内容为段首（按 \\n 分段）
    bool para_saw_indent_ws = false;
    bool para_flush_left = false;  // 本段源文顶格 → 可能是章名
    bool prev_para_was_chap = false;  // 上一段为章名 → 下一段多加章后距
    uint32_t para_first_line = 0;
    std::string para_title_acc;

    auto push_page_start = [&](uint32_t line_index) -> bool {
        if (!GrowTxtPageStarts(page_starts_len + 1)) {
            return false;
        }
        txt_page_first_line_[page_starts_len++] = line_index;
        return true;
    };

    const lv_coord_t line_stride = line_h + line_gap_;
    auto flush_line = [&](uint32_t next_off) -> bool {
        if (line_len == 0) {
            line_off = next_off;
            line_w = 0;
            line_started = false;
            line_para_start = false;
            return true;
        }
        // 章名另起一页（与 ebook/EPUB 一致）；首行即可用 IsTxtChapterTitleLine 判断
        if (line_para_start && para_flush_left && !para_title_acc.empty() &&
            IsTxtChapterTitleLine(para_title_acc) &&
            txt_lines_len_ > txt_page_first_line_[page_starts_len - 1]) {
            if (!push_page_start(static_cast<uint32_t>(txt_lines_len_))) {
                return false;
            }
            y = 0;
            ESP_LOGI(TAG, "chap txt page-break before '%.*s'", 48, para_title_acc.c_str());
        }
        if (y + line_stride > viewport_h_ &&
            txt_lines_len_ > txt_page_first_line_[page_starts_len - 1]) {
            if (!push_page_start(static_cast<uint32_t>(txt_lines_len_))) {
                return false;
            }
            y = 0;
        }
        if (!GrowTxtLines(txt_lines_len_ + 1)) {
            return false;
        }
        uint16_t store_len = line_len > kTxtLineLenMask ? kTxtLineLenMask : static_cast<uint16_t>(line_len);
        if (line_para_start && store_len > 0) {
            store_len = static_cast<uint16_t>(store_len | kTxtLineParaStart);
        }
        txt_lines_[txt_lines_len_++] = TxtLineRef{line_off, store_len};
        y += line_stride;
        line_off = next_off;
        line_len = 0;
        line_w = 0;
        line_started = false;
        line_para_start = false;
        return true;
    };

    TxtByteStream stream;
    if (!stream.InitBuf()) {
        ESP_LOGE(TAG, "txt stream buf OOM int=%u spiram=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        return false;
    }
    stream.fp = fp;
    stream.as_gbk = txt_as_gbk_;
    std::rewind(fp);
    stream.SkipBomIfAny();

    const uint32_t paginate_gen = txt_paginate_gen_;
    uint32_t cp = 0;
    uint32_t goff = 0;
    uint16_t glen = 0;
    bool is_nl = false;
    size_t glyph_n = 0;
    while (stream.NextGlyph(&cp, &goff, &glen, &is_nl)) {
        if ((++glyph_n & 0x1FFu) == 0u) {
            if (paginate_gen != txt_paginate_gen_) {
                ESP_LOGI(TAG, "txt paginate aborted");
                return false;
            }
#ifndef EBOOK_EMULATOR
            // 全量扫描让出时间片，避免饿死 LVGL / 设置卡交互
            vTaskDelay(1);
#endif
        }
        if (file_size > 0) {
            const int pct = 10 + static_cast<int>(stream.Tell() * 80 / file_size);
            ReportOpenProgress(pct > 90 ? 90 : pct);
        }
        if (is_nl) {
            if (line_started) {
                const uint32_t chap_line = para_first_line;
                const bool try_chap = para_flush_left && !para_title_acc.empty();

                if (!flush_line(goff + glen)) {
                    ESP_LOGE(TAG, "txt paginate OOM");
                    return false;
                }
                if (try_chap) {
                    const std::string chap_title = para_title_acc;
                    prev_para_was_chap = TryPushTxtChapter(std::move(para_title_acc), chap_line);
                    // 展示拆成两行时，源文若只占一行则补一行高度
                    if (prev_para_was_chap) {
                        std::string ord;
                        std::string name;
                        if (SplitChapterHeading(chap_title, ord, name) && !name.empty()) {
                            const uint32_t nlines =
                                static_cast<uint32_t>(txt_lines_len_) - chap_line;
                            if (nlines < 2) {
                                const lv_coord_t stride = FontLineHeight(font_) + line_gap_;
                                if (y + stride <= viewport_h_) {
                                    y += stride;
                                }
                            }
                        }
                    }
                } else {
                    prev_para_was_chap = false;
                }
                para_title_acc.clear();
            }
            at_para_start = true;
            para_saw_indent_ws = false;
            para_flush_left = false;
            para_title_acc.clear();
            continue;
        }

        if (at_para_start && !line_started && IsTxtParaIndentWs(cp)) {
            para_saw_indent_ws = true;
            continue;
        }

        const lv_coord_t cw = GlyphWidth(font_, cp);
        if (line_started && (line_w + cw > viewport_w_ || line_len + glen > kMaxLineSourceBytes)) {
            if (!flush_line(goff)) {
                ESP_LOGE(TAG, "txt paginate OOM");
                return false;
            }
        }
        if (!line_started) {
            line_off = goff;
            line_started = true;
            if (at_para_start) {
                // 段间距计入分页高度（与渲染段首 pad_top 一致）；页顶不加
                // 紧跟章名时再加 kReaderChapterTitleAfterGap
                const lv_coord_t gap =
                    para_gap_ + (prev_para_was_chap ? kReaderChapterTitleAfterGap : 0);
                prev_para_was_chap = false;
                if (y > 0) {
                    if (y + gap + line_stride > viewport_h_ &&
                        txt_lines_len_ > txt_page_first_line_[page_starts_len - 1]) {
                        if (!push_page_start(static_cast<uint32_t>(txt_lines_len_))) {
                            return false;
                        }
                        y = 0;
                    } else {
                        y += gap;
                    }
                }
                para_flush_left = !para_saw_indent_ws;
                para_first_line = static_cast<uint32_t>(txt_lines_len_);
                para_title_acc.clear();
                line_w = indent_w;
                line_para_start = true;
                at_para_start = false;
            }
        }
        line_len += glen;
        line_w += cw;
        // 顶格段累加 UTF-8 供章名识别（不回 seek，避免打乱字节流）
        if (para_flush_left && para_title_acc.size() < kMaxTocTitleBytes) {
            char ubuf[8];
            size_t un = 0;
            if (cp < 0x80u) {
                ubuf[0] = static_cast<char>(cp);
                un = 1;
            } else if (cp < 0x800u) {
                ubuf[0] = static_cast<char>(0xC0u | (cp >> 6));
                ubuf[1] = static_cast<char>(0x80u | (cp & 0x3Fu));
                un = 2;
            } else if (cp < 0x10000u) {
                ubuf[0] = static_cast<char>(0xE0u | (cp >> 12));
                ubuf[1] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
                ubuf[2] = static_cast<char>(0x80u | (cp & 0x3Fu));
                un = 3;
            } else {
                ubuf[0] = static_cast<char>(0xF0u | (cp >> 18));
                ubuf[1] = static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
                ubuf[2] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
                ubuf[3] = static_cast<char>(0x80u | (cp & 0x3Fu));
                un = 4;
            }
            if (para_title_acc.size() + un <= kMaxTocTitleBytes) {
                para_title_acc.append(ubuf, un);
            }
        }
    }

    if (line_started) {
        const uint32_t end_off = stream.Tell();
        const uint32_t chap_line = para_first_line;
        const bool try_chap = para_flush_left && !para_title_acc.empty();
        if (!flush_line(end_off)) {
            ESP_LOGE(TAG, "txt paginate OOM");
            return false;
        }
        if (try_chap) {
            TryPushTxtChapter(std::move(para_title_acc), chap_line);
        }
        para_title_acc.clear();
    } else if (txt_lines_len_ == 0) {
        if (!flush_line(stream.Tell())) {
            ESP_LOGE(TAG, "txt paginate OOM");
            return false;
        }
    }
    if (!push_page_start(static_cast<uint32_t>(txt_lines_len_))) {
        return false;
    }
    txt_page_count_ = page_starts_len - 1;
    toc_ = std::move(built_toc);
    FinalizeTocPages();

    ESP_LOGI(TAG, "txt stream index lines=%u pages=%u file=%u toc=%u",
             static_cast<unsigned>(txt_lines_len_), static_cast<unsigned>(txt_page_count_),
             static_cast<unsigned>(file_size),
             static_cast<unsigned>(toc_.size()));
    return true;
}

bool BookSession::OpenTxt(const char* path, size_t known_size) {
    if (path == nullptr) {
        ESP_LOGE(TAG, "txt open: path null");
        return false;
    }
    ESP_LOGI(TAG, "txt open begin int=%u spiram=%u known_size=%u path=%s",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(known_size), path);
    ReportOpenProgress(5);

    // FatFS 上部分大文件/长文件名 fseek(END) 会 EIO；优先 stat / 书库已扫到的大小
    size_t sz = 0;
    const char* size_src = "none";
    struct stat st {};
    if (stat(path, &st) == 0 && st.st_size >= 0) {
        sz = static_cast<size_t>(st.st_size);
        size_src = "stat";
    } else if (known_size > 0) {
        sz = known_size;
        size_src = "bookinfo";
        ESP_LOGW(TAG, "txt stat fail errno=%d, use known_size=%u", errno,
                 static_cast<unsigned>(known_size));
    }

    FILE* fp = std::fopen(path, "rb");
    if (fp == nullptr) {
        ESP_LOGE(TAG, "txt fopen fail errno=%d int=%u spiram=%u path=%s", errno,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)), path);
        return false;
    }

    if (sz == 0) {
        if (std::fseek(fp, 0, SEEK_END) != 0) {
            ESP_LOGE(TAG, "txt size unknown: fseek END fail errno=%d path=%s", errno, path);
            std::fclose(fp);
            return false;
        }
        const long sz_l = std::ftell(fp);
        if (sz_l < 0) {
            ESP_LOGE(TAG, "txt size unknown: ftell fail errno=%d path=%s", errno, path);
            std::fclose(fp);
            return false;
        }
        sz = static_cast<size_t>(sz_l);
        size_src = "fseek";
        std::rewind(fp);
    }
    ESP_LOGI(TAG, "txt size=%u via %s", static_cast<unsigned>(sz), size_src);

    txt_mode_ = true;
    chapter_count_ = 1;
    chapter_index_ = 0;

    if (sz == 0) {
        std::fclose(fp);
        txt_empty_ = true;
        if (!GrowTxtPageStarts(2) || !GrowTxtLines(1)) {
            ESP_LOGE(TAG, "txt empty index OOM");
            return false;
        }
        txt_page_first_line_[0] = 0;
        txt_lines_[0] = TxtLineRef{0, 0};
        txt_lines_len_ = 1;
        txt_page_first_line_[1] = 1;
        txt_page_count_ = 1;
        page_index_ = 0;
        ESP_LOGI(TAG, "txt empty ok: %s", path);
        ReportOpenProgress(90);
        return true;
    }
    if (sz > kMaxTxtBytes) {
        ESP_LOGE(TAG, "txt too large: %u > %u (%s)", static_cast<unsigned>(sz),
                 static_cast<unsigned>(kMaxTxtBytes), path);
        std::fclose(fp);
        return false;
    }

    // fopen 后已在文件头；勿再 SEEK_END（已证实部分 TXT 会 EIO）
    uint8_t peek[4096];
    const size_t npeek = std::fread(peek, 1, sizeof(peek), fp);
    txt_as_gbk_ = !LooksLikeUtf8(peek, npeek);

    const size_t est_lines = sz / 24 + 256;
    ESP_LOGI(TAG,
             "txt %s size=%u encoding=%s est_lines=%u est_idx~%uKB int=%u spiram=%u",
             path, static_cast<unsigned>(sz), txt_as_gbk_ ? "GBK" : "UTF-8",
             static_cast<unsigned>(est_lines),
             static_cast<unsigned>((est_lines * sizeof(TxtLineRef)) / 1024),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    ReportOpenProgress(10);
    if (TryLoadTxtIndex(path, sz)) {
        txt_fp_ = fp;
        page_index_ = 0;
        ESP_LOGI(TAG, "txt ready pages=%d (from SD cache) lines=%u int=%u spiram=%u",
                 PageCount(), static_cast<unsigned>(txt_lines_len_),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        ReportOpenProgress(90);
        return true;
    }

    ESP_LOGI(TAG, "txt idx miss, stream paginate...");
    if (!StreamPaginateTxt(fp, sz)) {
        ESP_LOGE(TAG, "txt stream paginate fail lines=%u pages=%u int=%u spiram=%u",
                 static_cast<unsigned>(txt_lines_len_), static_cast<unsigned>(txt_page_count_),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        std::fclose(fp);
        ClearTxtIndex();
        return false;
    }

    ReportOpenProgress(92);
    // 索引落盘；失败不影响本次阅读
    if (!SaveTxtIndex(path, sz)) {
        ESP_LOGW(TAG, "txt idx save skipped/failed");
    }

    // 保留文件句柄供翻页物化；失败则 Materialize 时重开
    txt_fp_ = fp;
    page_index_ = 0;
    ESP_LOGI(TAG, "txt ready pages=%d (no full-text in RAM)", PageCount());
    return true;
}

bool BookSession::OpenEpub(const char* path) {
    ReportOpenProgress(10);
    epub_ = std::make_unique<EpubDocument>();
    if (!epub_->Open(path)) {
        epub_.reset();
        return false;
    }
    ReportOpenProgress(40);
    if (!epub_->Title().empty()) {
        display_title_ = epub_->Title();
    }
    display_author_ = epub_->Author();
    chapter_count_ = std::max(1, epub_->SpineCount());
    chapter_index_ = 0;
    ClearToc();
    for (int i = 0; i < chapter_count_; ++i) {
        TocEntry ent;
        char buf[48];
        std::snprintf(buf, sizeof(buf), UiChapterFmt(), i + 1);
        ent.title = buf;
        toc_.push_back(std::move(ent));
    }
    return true;
}

bool BookSession::OpenEbook(const char* path) {
    ReportOpenProgress(10);
    ebook_ = std::make_unique<EbookDocument>();
    if (!ebook_->Open(path)) {
        ebook_.reset();
        return false;
    }
    ReportOpenProgress(40);
    if (!ebook_->Title().empty()) {
        display_title_ = ebook_->Title();
    }
    display_author_ = ebook_->Author();
    page_images_mode_ = ebook_->HasPageImages();
    ebook_spine_.clear();
    const int raw_n = ebook_->ChapterCount();
    for (int i = 0; i < raw_n; ++i) {
        if (ebook_->ChapterLikelyHasContent(i)) {
            ebook_spine_.push_back(i);
        }
        if (raw_n > 0 && (i % 8 == 0 || i + 1 == raw_n)) {
            ReportOpenProgress(40 + (20 * (i + 1)) / raw_n);
        }
    }
    if (ebook_spine_.empty()) {
        ESP_LOGW(TAG, "ebook has no non-empty chapters");
        ebook_.reset();
        ebook_spine_.clear();
        return false;
    }
    chapter_count_ = static_cast<int>(ebook_spine_.size());
    chapter_index_ = 0;
    ClearToc();
    // 仅当 FLAG_HAS_TOC 时填目录；无原生目录 → TocCount()==0，UI 显示「暂无目录」
    if (ebook_->HasToc()) {
        for (size_t si = 0; si < ebook_spine_.size(); ++si) {
            TocEntry ent;
            ent.title = ebook_->ChapterTitle(ebook_spine_[si]);
            if (IsPlaceholderChapterTitle(ent.title)) {
                ent.title = FormatChapterOrdinalTitle(static_cast<int>(si + 1));
            }
            toc_.push_back(std::move(ent));
        }
    }
    return true;
}

bool BookSession::LoadEpubChapter(int chapter) {
    std::lock_guard<std::mutex> lock(chapter_io_mu_);
    if (!epub_ || chapter < 0 || chapter >= epub_->SpineCount()) {
        return false;
    }
    ReportOpenProgress(55);
    std::vector<ContentBlock> blocks;
    try {
        if (!epub_->LoadChapterBlocks(chapter, blocks)) {
            ContentBlock b;
            b.kind = ContentKind::kText;
            b.text = UiChapterNoText();
            blocks.push_back(std::move(b));
        }
        std::string title;
        if (static_cast<size_t>(chapter) < toc_.size()) {
            title = toc_[static_cast<size_t>(chapter)].title;
        }
        EnsureChapterTitleInBlocks(std::move(title), chapter + 1, blocks);
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "epub LoadChapterBlocks OOM chapter=%d", chapter);
        return false;
    }
    ReportOpenProgress(75);
    // 先分页到临时缓冲；失败则不改 chapter_index_/pages_，避免跨章翻页半更新
    if (!BuildPagesFromBlocks(blocks)) {
        return false;
    }
    chapter_index_ = chapter;
    NoteChapterPageCount(chapter, static_cast<int>(pages_.size()));
    ReportOpenProgress(90);
    return true;
}

bool BookSession::LoadEbookChapter(int chapter) {
    std::lock_guard<std::mutex> lock(chapter_io_mu_);
    if (!ebook_ || chapter < 0 || static_cast<size_t>(chapter) >= ebook_spine_.size()) {
        return false;
    }
    const int file_ch = ebook_spine_[static_cast<size_t>(chapter)];
    const uint32_t uncomp = ebook_->ChapterUncompressedTotal(file_ch);
    if (uncomp > kMaxChapterUncompForPaginate) {
        ESP_LOGE(TAG, "ebook chapter too large file_idx=%d uncomp=%u max=%u (re-convert with size split)",
                 file_ch, static_cast<unsigned>(uncomp),
                 static_cast<unsigned>(kMaxChapterUncompForPaginate));
        return false;
    }
    ReportOpenProgress(55);
    std::vector<ContentBlock> blocks;
    try {
        if (!ebook_->LoadChapterBlocks(file_ch, blocks)) {
            ESP_LOGW(TAG, "empty ebook chapter file_idx=%d", file_ch);
            return false;
        }
        EnsureChapterTitleInBlocks(ebook_->ChapterTitle(file_ch), chapter + 1, blocks);
        if (blocks.empty()) {
            ESP_LOGW(TAG, "empty ebook chapter file_idx=%d", file_ch);
            return false;
        }
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "ebook LoadChapterBlocks OOM file_idx=%d", file_ch);
        return false;
    }
    ReportOpenProgress(75);
    if (!BuildPagesFromBlocks(blocks)) {
        return false;
    }
    chapter_index_ = chapter;
    NoteChapterPageCount(chapter, static_cast<int>(pages_.size()));
    ESP_LOGI(TAG, "ebook chapter ready %d/%d pages=%u", chapter_index_ + 1, chapter_count_,
             static_cast<unsigned>(pages_.size()));
    ReportOpenProgress(90);
    return true;
}

bool BookSession::PaginateTextBlock(const std::string& text, std::vector<Page>& pages, Page& cur,
                                    lv_coord_t& y, bool chapter_title) {
    // .ebook / EPUB：每 ContentBlock 一段；段首 strip 源文空白 + pad_left 两字；段内 \\n 软换行顶格
    // chapter_title：章名段顶格加粗，不缩进（正文仍保留）
    const lv_coord_t line_h = FontLineHeight(font_);
    const lv_coord_t indent_w = chapter_title ? 0 : TxtParaIndentWidth(font_);
    std::string line;
    lv_coord_t line_w = 0;
    bool first_line_of_para = true;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(text.data());
    size_t i = 0;
    while (i < text.size() && text[i] == '\n') {
        ++i;
    }

    auto flush_line = [&](bool indent_first) -> bool {
        if (line.empty()) {
            return true;
        }
        if (indent_first) {
            StripLeadingParaWs(line);
        }
        const bool indent = indent_first && !line.empty() && !chapter_title;
        const bool gap_before = indent_first && !line.empty();
        if (!FlushLineToPage(line, pages, cur, y, line_h, line_gap_, viewport_h_, indent,
                             chapter_title, gap_before)) {
            return false;
        }
        first_line_of_para = false;
        return true;
    };

    while (i < text.size()) {
        if (text[i] == '\n') {
            if (!flush_line(first_line_of_para)) {
                return false;
            }
            line_w = 0;
            ++i;
            continue;
        }
        uint32_t cp = 0;
        const size_t n = Utf8Next(data + i, text.size() - i, &cp);
        if (n == 0) {
            break;
        }
        // 段首：吞掉源文前导空白，不参与折行/渲染；缩进仅由 pad_left 两字承担
        if (first_line_of_para && line.empty() && IsTxtParaIndentWs(cp)) {
            i += n;
            continue;
        }
        lv_coord_t cw = GlyphWidth(font_, cp);
        if ((cp == 0x20 || cp == 0xA0) && cw <= 0) {
            cw = std::max<lv_coord_t>(1, FontLineHeight(font_) / 3);
        }
        if (!line.empty() && line_w + cw > viewport_w_) {
            if (!flush_line(first_line_of_para)) {
                return false;
            }
            line_w = 0;
        }
        if (line.empty()) {
            line_w = first_line_of_para ? indent_w : 0;
        }
        try {
            line.append(text.data() + i, n);
        } catch (const std::bad_alloc&) {
            return false;
        }
        line_w += cw;
        i += n;
    }
    if (!line.empty()) {
        if (!flush_line(first_line_of_para)) {
            return false;
        }
    }
    return true;
}

bool BookSession::BuildPagesFromBlocks(const std::vector<ContentBlock>& blocks) {
    // 写入临时 built，成功后再 swap 进 pages_：
    // - 失败：pages_/page_index_ 不变（跨章翻页可回退到旧章）
    // - 峰值：切换章瞬间 ≈ 旧 pages_ + 新 built + blocks（靠转换拆章把单章压在预算内）
    std::vector<Page> built;
    Page cur;
    lv_coord_t y = 0;
    const lv_coord_t line_h = FontLineHeight(font_);

    auto fail_oom = []() -> bool {
        ESP_LOGE(TAG, "ebook/epub paginate OOM");
        return false;
    };

    try {
        for (size_t bi = 0; bi < blocks.size(); ++bi) {
            const auto& block = blocks[bi];
            if (block.kind == ContentKind::kText) {
                // ebook/EPUB：章名另起一页（页首）；TXT 走 StreamPaginate，不经过此路径
                if (block.chapter_title && !cur.items.empty()) {
                    ESP_LOGI(TAG, "chap page-break before title='%.*s'", 48, block.text.c_str());
                    built.push_back(std::move(cur));
                    cur = Page{};
                    y = 0;
                }
                if (!PaginateTextBlock(block.text, built, cur, y, block.chapter_title)) {
                    return fail_oom();
                }
                if (!EnsurePageVerticalSpace(y, cur, built, para_gap_, viewport_h_)) {
                    return fail_oom();
                }
                y += para_gap_;
                if (block.chapter_title) {
                    y += kReaderChapterTitleAfterGap;
                }
                continue;
            }
            lv_coord_t img_w = viewport_w_;
            lv_coord_t img_h = viewport_w_ * 3 / 4;
            if (page_images_mode_) {
                // 整页转图：每图独占一页，占满视口
                if (!cur.items.empty()) {
                    built.push_back(std::move(cur));
                    cur = Page{};
                    y = 0;
                }
                img_h = viewport_h_;
                PageItem item;
                item.kind = ContentKind::kImage;
                item.image_href = block.image_href;
                item.image_w = img_w;
                item.image_h = img_h;
                cur.items.push_back(std::move(item));
                built.push_back(std::move(cur));
                cur = Page{};
                y = 0;
                continue;
            }
            if (img_h > viewport_h_ - line_h) {
                img_h = viewport_h_ - line_h;
            }
            if (img_h < 40) {
                img_h = 40;
            }
            if (y + img_h + kImageGap > viewport_h_ && !cur.items.empty()) {
                built.push_back(std::move(cur));
                cur = Page{};
                y = 0;
            }
            if (img_h + kImageGap > viewport_h_ / 2 && !cur.items.empty()) {
                built.push_back(std::move(cur));
                cur = Page{};
                y = 0;
            }
            PageItem item;
            item.kind = ContentKind::kImage;
            item.image_href = block.image_href;
            item.image_w = img_w;
            item.image_h = img_h;
            cur.items.push_back(std::move(item));
            y += img_h + kImageGap;
        }

        if (!cur.items.empty()) {
            built.push_back(std::move(cur));
        }
        if (built.empty()) {
            Page empty;
            PageItem item;
            item.kind = ContentKind::kText;
            item.text = UiBlank();
            empty.items.push_back(std::move(item));
            built.push_back(std::move(empty));
        }
    } catch (const std::bad_alloc&) {
        return fail_oom();
    }

    pages_.swap(built);
    page_index_ = 0;
    ESP_LOGI(TAG, "paginated %u pages", static_cast<unsigned>(pages_.size()));
    return true;
}

void BookSession::NoteChapterPageCount(int chapter, int pages) {
    if (!IsChapteredBook() || chapter < 0 || chapter >= chapter_count_) {
        return;
    }
    if (chapter_page_counts_.size() != static_cast<size_t>(chapter_count_)) {
        try {
            chapter_page_counts_.assign(static_cast<size_t>(chapter_count_), 0);
        } catch (const std::bad_alloc&) {
            return;
        }
        chapter_pages_ready_ = false;
    }
    if (pages < 1) {
        pages = 1;
    }
    if (pages > 65535) {
        pages = 65535;
    }
    chapter_page_counts_[static_cast<size_t>(chapter)] = static_cast<uint16_t>(pages);
}

bool BookSession::CountPagesFromBlocks(const std::vector<ContentBlock>& blocks, int& out_pages) const {
    // 与 BuildPagesFromBlocks 同口径，只计页、不存正文（扫全书页表用）
    out_pages = 0;
    int pages = 0;
    bool cur_has = false;
    lv_coord_t y = 0;
    const lv_coord_t line_h = FontLineHeight(font_);
    const lv_coord_t indent_w = TxtParaIndentWidth(font_);
    const lv_coord_t stride = line_h + line_gap_;

    auto break_page = [&]() {
        if (cur_has) {
            ++pages;
            cur_has = false;
            y = 0;
        }
    };

    auto ensure_space = [&](lv_coord_t need_h) {
        if (need_h > 0 && y + need_h > viewport_h_ && cur_has) {
            break_page();
        }
    };

    auto flush_line = [&](std::string& line, bool indent_first) {
        if (line.empty()) {
            return;
        }
        if (indent_first) {
            StripLeadingParaWs(line);
        }
        if (line.empty()) {
            return;
        }
        ensure_space(stride);
        cur_has = true;
        y += stride;
        line.clear();
    };

    auto paginate_text = [&](const std::string& text, bool chapter_title) {
        std::string line;
        lv_coord_t line_w = 0;
        bool first_line_of_para = true;
        const lv_coord_t first_indent = chapter_title ? 0 : indent_w;
        const uint8_t* data = reinterpret_cast<const uint8_t*>(text.data());
        size_t i = 0;
        while (i < text.size() && text[i] == '\n') {
            ++i;
        }
        while (i < text.size()) {
            if (text[i] == '\n') {
                flush_line(line, first_line_of_para);
                first_line_of_para = false;
                line_w = 0;
                ++i;
                continue;
            }
            uint32_t cp = 0;
            const size_t n = Utf8Next(data + i, text.size() - i, &cp);
            if (n == 0) {
                break;
            }
            if (first_line_of_para && line.empty() && IsTxtParaIndentWs(cp)) {
                i += n;
                continue;
            }
            lv_coord_t cw = GlyphWidth(font_, cp);
            if ((cp == 0x20 || cp == 0xA0) && cw <= 0) {
                cw = std::max<lv_coord_t>(1, FontLineHeight(font_) / 3);
            }
            if (!line.empty() && line_w + cw > viewport_w_) {
                flush_line(line, first_line_of_para);
                first_line_of_para = false;
                line_w = 0;
            }
            if (line.empty()) {
                line_w = first_line_of_para ? first_indent : 0;
            }
            line.append(text.data() + i, n);
            line_w += cw;
            i += n;
        }
        if (!line.empty()) {
            flush_line(line, first_line_of_para);
        }
    };

    for (const auto& block : blocks) {
        if (block.kind == ContentKind::kText) {
            // 与 BuildPagesFromBlocks 一致：章名另起一页
            if (block.chapter_title) {
                break_page();
            }
            paginate_text(block.text, block.chapter_title);
            ensure_space(para_gap_);
            y += para_gap_;
            if (block.chapter_title) {
                y += kReaderChapterTitleAfterGap;
            }
            continue;
        }
        lv_coord_t img_w = viewport_w_;
        lv_coord_t img_h = viewport_w_ * 3 / 4;
        (void)img_w;
        if (page_images_mode_) {
            break_page();
            ++pages;
            cur_has = false;
            y = 0;
            continue;
        }
        if (img_h > viewport_h_ - line_h) {
            img_h = viewport_h_ - line_h;
        }
        if (img_h < 40) {
            img_h = 40;
        }
        if (y + img_h + kImageGap > viewport_h_ && cur_has) {
            break_page();
        }
        if (img_h + kImageGap > viewport_h_ / 2 && cur_has) {
            break_page();
        }
        cur_has = true;
        y += img_h + kImageGap;
    }

    if (cur_has) {
        ++pages;
    }
    if (pages <= 0) {
        pages = 1;
    }
    out_pages = pages;
    return true;
}

bool BookSession::CountChapterPages(int chapter, int& out_pages) const {
    out_pages = 1;
    if (!IsChapteredBook() || chapter < 0 || chapter >= chapter_count_) {
        return false;
    }
    std::lock_guard<std::mutex> lock(chapter_io_mu_);
    // 当前章已物化：直接用 pages_，避免重复解压
    if (chapter == chapter_index_ && !pages_.empty()) {
        out_pages = static_cast<int>(pages_.size());
        return true;
    }

    std::vector<ContentBlock> blocks;
    try {
        if (ebook_ != nullptr) {
            if (static_cast<size_t>(chapter) >= ebook_spine_.size()) {
                return false;
            }
            const int file_ch = ebook_spine_[static_cast<size_t>(chapter)];
            const uint32_t uncomp = ebook_->ChapterUncompressedTotal(file_ch);
            if (uncomp > kMaxChapterUncompForPaginate) {
                out_pages = 1;
                return true;
            }
            if (!ebook_->LoadChapterBlocks(file_ch, blocks)) {
                out_pages = 1;
                return true;
            }
            EnsureChapterTitleInBlocks(ebook_->ChapterTitle(file_ch), chapter + 1, blocks);
            if (blocks.empty()) {
                out_pages = 1;
                return true;
            }
        } else if (epub_ != nullptr) {
            if (!epub_->LoadChapterBlocks(chapter, blocks)) {
                ContentBlock b;
                b.kind = ContentKind::kText;
                b.text = UiChapterNoText();
                blocks.push_back(std::move(b));
            }
            std::string title;
            if (static_cast<size_t>(chapter) < toc_.size()) {
                title = toc_[static_cast<size_t>(chapter)].title;
            }
            EnsureChapterTitleInBlocks(std::move(title), chapter + 1, blocks);
        } else {
            return false;
        }
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "CountChapterPages OOM chapter=%d", chapter);
        return false;
    }
    return CountPagesFromBlocks(blocks, out_pages);
}

namespace {

constexpr uint32_t kChapterPageCacheVersion = 1;

#pragma pack(push, 1)
struct ChapterPageHdr {
    char magic[4];  // "CPG1"
    uint32_t version;
    uint32_t chapter_count;
    uint16_t viewport_w;
    uint16_t viewport_h;
    uint16_t line_gap;
    uint16_t para_gap;
    uint32_t font_fp;
    uint8_t page_images_mode;
    uint8_t reserved[3];
};
#pragma pack(pop)

}  // namespace

bool BookSession::TryLoadChapterPageCache() {
    if (!IsChapteredBook() || info_.path.empty() || chapter_count_ <= 0) {
        return false;
    }
    const std::string path = ChapterPageCachePath(info_.path.c_str());
    FILE* ifp = std::fopen(path.c_str(), "rb");
    if (ifp == nullptr) {
        return false;
    }
    ChapterPageHdr hdr{};
    if (std::fread(&hdr, 1, sizeof(hdr), ifp) != sizeof(hdr)) {
        std::fclose(ifp);
        return false;
    }
    if (std::memcmp(hdr.magic, "CPG1", 4) != 0 || hdr.version != kChapterPageCacheVersion ||
        hdr.chapter_count != static_cast<uint32_t>(chapter_count_) ||
        hdr.viewport_w != static_cast<uint16_t>(viewport_w_) ||
        hdr.viewport_h != static_cast<uint16_t>(viewport_h_) ||
        hdr.line_gap != static_cast<uint16_t>(line_gap_) ||
        hdr.para_gap != static_cast<uint16_t>(para_gap_) || hdr.font_fp != FontFingerprint() ||
        hdr.page_images_mode != static_cast<uint8_t>(page_images_mode_ ? 1 : 0)) {
        std::fclose(ifp);
        return false;
    }
    std::vector<uint16_t> counts;
    try {
        counts.resize(static_cast<size_t>(chapter_count_));
    } catch (const std::bad_alloc&) {
        std::fclose(ifp);
        return false;
    }
    if (std::fread(counts.data(), sizeof(uint16_t), counts.size(), ifp) != counts.size()) {
        std::fclose(ifp);
        return false;
    }
    std::fclose(ifp);
    for (uint16_t n : counts) {
        if (n == 0) {
            return false;
        }
    }
    chapter_page_counts_.swap(counts);
    chapter_pages_ready_ = true;
    // 与当前已加载章对齐（缓存可能略旧于二次分页）
    if (!pages_.empty() && chapter_index_ >= 0 &&
        static_cast<size_t>(chapter_index_) < chapter_page_counts_.size()) {
        NoteChapterPageCount(chapter_index_, static_cast<int>(pages_.size()));
    }
    ESP_LOGI(TAG, "chapter page cache hit %s chapters=%d book_pages=%d", path.c_str(), chapter_count_,
             BookPageCount());
    return true;
}

bool BookSession::SaveChapterPageCache() const {
    if (!IsChapteredBook() || !chapter_pages_ready_ || info_.path.empty() ||
        chapter_page_counts_.size() != static_cast<size_t>(chapter_count_)) {
        return false;
    }
    const std::string path = ChapterPageCachePath(info_.path.c_str());
    FILE* ofp = std::fopen(path.c_str(), "wb");
    if (ofp == nullptr) {
        return false;
    }
    ChapterPageHdr hdr{};
    std::memcpy(hdr.magic, "CPG1", 4);
    hdr.version = kChapterPageCacheVersion;
    hdr.chapter_count = static_cast<uint32_t>(chapter_count_);
    hdr.viewport_w = static_cast<uint16_t>(viewport_w_);
    hdr.viewport_h = static_cast<uint16_t>(viewport_h_);
    hdr.line_gap = static_cast<uint16_t>(line_gap_);
    hdr.para_gap = static_cast<uint16_t>(para_gap_);
    hdr.font_fp = FontFingerprint();
    hdr.page_images_mode = static_cast<uint8_t>(page_images_mode_ ? 1 : 0);
    if (std::fwrite(&hdr, 1, sizeof(hdr), ofp) != sizeof(hdr) ||
        std::fwrite(chapter_page_counts_.data(), sizeof(uint16_t), chapter_page_counts_.size(),
                    ofp) != chapter_page_counts_.size()) {
        std::fclose(ofp);
        return false;
    }
    std::fclose(ofp);
    ESP_LOGI(TAG, "chapter page cache saved %s chapters=%d", path.c_str(), chapter_count_);
    return true;
}

bool BookSession::RebuildChapterPageTable() {
    if (!IsChapteredBook() || chapter_count_ <= 0) {
        chapter_pages_ready_ = true;
        return true;
    }
    const uint32_t gen = chapter_pages_gen_;
    std::vector<uint16_t> built;
    try {
        built.resize(static_cast<size_t>(chapter_count_), 0);
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "RebuildChapterPageTable OOM");
        return false;
    }
    for (int ch = 0; ch < chapter_count_; ++ch) {
        if (gen != chapter_pages_gen_) {
            chapter_pages_ready_ = false;
            ESP_LOGI(TAG, "chapter page table abort at %d/%d gen=%u", ch + 1, chapter_count_,
                     static_cast<unsigned>(chapter_pages_gen_));
            return true;
        }
        int n = 1;
        if (!CountChapterPages(ch, n)) {
            n = 1;
        }
        if (n < 1) {
            n = 1;
        }
        if (n > 65535) {
            n = 65535;
        }
        built[static_cast<size_t>(ch)] = static_cast<uint16_t>(n);
    }
    chapter_page_counts_.swap(built);
    if (gen != chapter_pages_gen_) {
        // 扫表期间又换了排版：作废本次结果，留给 again 再跑
        chapter_pages_ready_ = false;
        ESP_LOGI(TAG, "chapter page table stale gen=%u now=%u", static_cast<unsigned>(gen),
                 static_cast<unsigned>(chapter_pages_gen_));
        return true;
    }
    chapter_pages_ready_ = true;
    SaveChapterPageCache();
    ESP_LOGI(TAG, "chapter page table ready chapters=%d book_pages=%d", chapter_count_,
             BookPageCount());
    return true;
}

bool BookSession::WarmChapterPageTableFromCache() {
    if (!IsChapteredBook()) {
        chapter_pages_ready_ = true;
        return true;
    }
    if (TryLoadChapterPageCache()) {
        return true;
    }
    NoteChapterPageCount(chapter_index_, std::max(1, PageCount()));
    chapter_pages_ready_ = false;
    return false;
}

bool BookSession::FinishChapterPageTable() {
    if (!IsChapteredBook()) {
        chapter_pages_ready_ = true;
        return true;
    }
    OpenProgressFn saved_fn = progress_fn_;
    void* saved_user = progress_user_;
    progress_fn_ = nullptr;
    progress_user_ = nullptr;
    const bool ok = RebuildChapterPageTable();
    progress_fn_ = saved_fn;
    progress_user_ = saved_user;
    return ok;
}

bool BookSession::EnsureCover(int max_w, int max_h) {
    if (cover_tried_) {
        return !cover_.empty();
    }
    cover_tried_ = true;
    cover_.Reset();

    if (info_.format == BookFormat::kEpub && epub_) {
        if (epub_->DecodeCoverImageToL8(max_w, max_h, cover_)) {
            return true;
        }
    }
    if (info_.format == BookFormat::kEbook && ebook_) {
        std::vector<uint8_t> bytes;
        if (ebook_->LoadCoverBytes(bytes) &&
            DecodeImageToL8(bytes.data(), bytes.size(), max_w, max_h, cover_)) {
            return true;
        }
    }
    return false;
}

bool BookSession::TryCopyCachedPageImage(const std::string& href, RasterImage& out) const {
    out.Reset();
    if (href.empty() || cached_href_ != href || cached_image_.empty()) {
        return false;
    }
    out.pixels = cached_image_.pixels;
    out.width = cached_image_.width;
    out.height = cached_image_.height;
    out.BindDsc();
    return true;
}

void BookSession::SetCachedPageImage(const std::string& href, RasterImage&& img) {
    if (href.empty() || img.empty()) {
        return;
    }
    cached_href_ = href;
    cached_image_ = std::move(img);
    cached_image_.BindDsc();
}

bool BookSession::LoadPageImage(const std::string& href, int max_w, int max_h, RasterImage& out) {
    out.Reset();
    if (href.empty()) {
        return false;
    }
    if (cached_href_ == href && !cached_image_.empty()) {
        out.pixels = cached_image_.pixels;
        out.width = cached_image_.width;
        out.height = cached_image_.height;
        out.BindDsc();
        return true;
    }
    if (info_.format == BookFormat::kEpub && epub_) {
        if (!epub_->DecodeItemImageToL8(href.c_str(), max_w, max_h, out)) {
            return false;
        }
    } else if (info_.format == BookFormat::kEbook && ebook_) {
        uint32_t off = 0;
        if (!EbookDocument::ParseImageHref(href, off)) {
            return false;
        }
        std::vector<uint8_t> bytes;
        if (!ebook_->LoadImageBlockBytes(off, bytes)) {
            return false;
        }
        if (!DecodeImageToL8(bytes.data(), bytes.size(), max_w, max_h, out)) {
            return false;
        }
    } else {
        if (!DecodeImageFileToL8(href.c_str(), max_w, max_h, out)) {
            return false;
        }
    }
    cached_href_ = href;
    cached_image_.pixels = out.pixels;
    cached_image_.width = out.width;
    cached_image_.height = out.height;
    cached_image_.BindDsc();
    return true;
}

const Page* BookSession::CurrentPageData() const {
    if (live_preview_active_) {
        return &live_preview_page_;
    }
    if (txt_mode_) {
        if (page_index_ < 0 || page_index_ >= PageCount()) {
            return nullptr;
        }
        if (txt_view_index_ != page_index_) {
            MaterializeTxtPage(page_index_);
        }
        return &txt_view_page_;
    }
    if (page_index_ < 0 || page_index_ >= PageCount()) {
        return nullptr;
    }
    return &pages_[static_cast<size_t>(page_index_)];
}

bool BookSession::GoToPage(int page) {
    if (live_preview_active_) {
        return false;
    }
    if (page < 0 || page >= PageCount()) {
        return false;
    }
    page_index_ = page;
    SaveProgress();
    return true;
}

bool BookSession::IsChapteredBook() const {
    return (info_.format == BookFormat::kEpub && epub_) ||
           (info_.format == BookFormat::kEbook && ebook_);
}

bool BookSession::NeedsChapterPageRebuild() const {
    return IsChapteredBook() && !chapter_pages_ready_;
}

void BookSession::AbortTxtPaginate() {
    txt_paginate_gen_++;
}

void BookSession::InvalidateChapterPageTable() {
    if (!IsChapteredBook()) {
        return;
    }
    chapter_pages_gen_++;
    chapter_pages_ready_ = false;
}

bool BookSession::HasNextPage() const {
    if (live_preview_active_) {
        return live_preview_has_next_;
    }
    if (page_index_ + 1 < PageCount()) {
        return true;
    }
    return IsChapteredBook() && chapter_index_ + 1 < chapter_count_;
}

bool BookSession::HasPrevPage() const {
    if (live_preview_active_) {
        return !live_preview_hist_.empty();
    }
    if (page_index_ > 0) {
        return true;
    }
    return IsChapteredBook() && chapter_index_ > 0;
}

bool BookSession::NextPage() {
    if (live_preview_active_) {
        if (!live_preview_has_next_) {
            return false;
        }
        if (live_preview_hist_.size() < kLivePreviewHistMax) {
            try {
                live_preview_hist_.push_back(
                    LivePreviewHistEnt{live_preview_byte_off_, live_preview_at_para_start_});
            } catch (const std::bad_alloc&) {
                // 栈满则仍翻下一页，只是更早无法回退
            }
        } else if (!live_preview_hist_.empty()) {
            // ponytail: 满则丢最旧，保近期回退
            live_preview_hist_.erase(live_preview_hist_.begin());
            live_preview_hist_.push_back(
                LivePreviewHistEnt{live_preview_byte_off_, live_preview_at_para_start_});
        }
        const uint32_t off = live_preview_next_off_;
        const bool para = live_preview_next_at_para_start_;
        if (!FillTxtLivePreview(off, para)) {
            if (!live_preview_hist_.empty()) {
                live_preview_hist_.pop_back();
            }
            return false;
        }
        return true;
    }
    if (page_index_ + 1 < PageCount()) {
        ++page_index_;
        SaveProgress();
        return true;
    }
    if (IsChapteredBook() && chapter_index_ + 1 < chapter_count_) {
        const bool ok = (ebook_ != nullptr) ? LoadEbookChapter(chapter_index_ + 1)
                                            : LoadEpubChapter(chapter_index_ + 1);
        if (ok) {
            SaveProgress();
            return true;
        }
    }
    return false;
}

bool BookSession::PrevPage() {
    if (live_preview_active_) {
        if (live_preview_hist_.empty()) {
            return false;
        }
        const LivePreviewHistEnt ent = live_preview_hist_.back();
        live_preview_hist_.pop_back();
        if (!FillTxtLivePreview(ent.byte_off, ent.at_para_start)) {
            try {
                live_preview_hist_.push_back(ent);
            } catch (const std::bad_alloc&) {
            }
            return false;
        }
        return true;
    }
    if (page_index_ > 0) {
        --page_index_;
        SaveProgress();
        return true;
    }
    if (IsChapteredBook() && chapter_index_ > 0) {
        const bool ok = (ebook_ != nullptr) ? LoadEbookChapter(chapter_index_ - 1)
                                            : LoadEpubChapter(chapter_index_ - 1);
        if (ok) {
            page_index_ = std::max(0, PageCount() - 1);
            SaveProgress();
            return true;
        }
    }
    return false;
}


void BookSession::ClearToc() {
    toc_.clear();
}

int BookSession::PageIndexForLine(uint32_t line) const {
    if (!txt_mode_ || txt_page_first_line_ == nullptr || txt_page_count_ == 0) {
        return 0;
    }
    // 最大的 p 使得 first_line[p] <= line < first_line[p+1]
    size_t lo = 0;
    size_t hi = txt_page_count_;
    while (lo + 1 < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (txt_page_first_line_[mid] <= line) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return static_cast<int>(lo);
}

int BookSession::PageIndexForTxtByteOff(uint32_t byte_off) const {
    if (!txt_mode_ || txt_page_first_line_ == nullptr || txt_lines_ == nullptr ||
        txt_page_count_ == 0 || txt_lines_len_ == 0) {
        return 0;
    }
    int ans = 0;
    int lo = 0;
    int hi = static_cast<int>(txt_page_count_) - 1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const uint32_t line = txt_page_first_line_[static_cast<size_t>(mid)];
        if (line >= txt_lines_len_) {
            hi = mid - 1;
            continue;
        }
        const uint32_t moff = txt_lines_[line].off;
        if (moff <= byte_off) {
            ans = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return ans;
}

void BookSession::ClearLivePreview() {
    live_preview_active_ = false;
    live_preview_byte_off_ = 0;
    live_preview_at_para_start_ = true;
    live_preview_next_off_ = 0;
    live_preview_next_at_para_start_ = true;
    live_preview_has_next_ = false;
    live_preview_page_ = Page{};
    live_preview_hist_.clear();
}

BookSession::LayoutAnchor BookSession::CaptureLayoutAnchor() const {
    LayoutAnchor a;
    if (!open_) {
        return a;
    }
    a.valid = true;
    a.txt = txt_mode_;
    a.chapter = chapter_index_;
    a.page = std::max(0, page_index_);
    a.page_count = std::max(1, PageCount());
    if (txt_mode_) {
        if (live_preview_active_) {
            a.txt_byte_off = live_preview_byte_off_;
            a.txt_at_para_start = live_preview_at_para_start_;
        } else if (!txt_empty_ && txt_lines_ != nullptr && txt_page_first_line_ != nullptr &&
                   page_index_ >= 0 && static_cast<size_t>(page_index_) < txt_page_count_) {
            const uint32_t line = txt_page_first_line_[static_cast<size_t>(page_index_)];
            if (line < txt_lines_len_) {
                a.txt_byte_off = txt_lines_[line].off;
                a.txt_at_para_start = (txt_lines_[line].len & kTxtLineParaStart) != 0;
            }
        }
    }
    return a;
}

bool BookSession::FillTxtLivePreview(uint32_t byte_off, bool at_para_start) {
    live_preview_page_ = Page{};
    live_preview_byte_off_ = byte_off;
    live_preview_at_para_start_ = at_para_start;
    live_preview_active_ = false;
    live_preview_next_off_ = byte_off;
    live_preview_next_at_para_start_ = at_para_start;
    live_preview_has_next_ = false;

    if (txt_empty_) {
        PageItem item;
        item.kind = ContentKind::kText;
        item.text = UiBlank();
        live_preview_page_.items.push_back(std::move(item));
        live_preview_active_ = true;
        return true;
    }
    if (!EnsureTxtFile() || font_ == nullptr) {
        return false;
    }
    if (std::fseek(txt_fp_, static_cast<long>(byte_off), SEEK_SET) != 0) {
        return false;
    }

    TxtByteStream stream;
    if (!stream.InitBuf()) {
        return false;
    }
    stream.fp = txt_fp_;
    stream.as_gbk = txt_as_gbk_;
    stream.file_base = byte_off;
    stream.pos = 0;
    stream.end = 0;
    stream.eof = false;
    if (byte_off == 0) {
        stream.SkipBomIfAny();
    }

    const lv_coord_t line_h = FontLineHeight(font_);
    const lv_coord_t indent_w = TxtParaIndentWidth(font_);
    const lv_coord_t line_stride = line_h + line_gap_;
    lv_coord_t y = 0;
    std::string line;
    lv_coord_t line_w = 0;
    bool line_started = false;
    bool line_para_start = false;
    bool para_at_start = at_para_start;
    bool para_saw_indent_ws = false;
    uint32_t line_start_off = byte_off;
    bool page_full = false;

    auto append_cp = [&](uint32_t cp) -> bool {
        char ubuf[8];
        size_t un = 0;
        if (cp < 0x80u) {
            ubuf[0] = static_cast<char>(cp);
            un = 1;
        } else if (cp < 0x800u) {
            ubuf[0] = static_cast<char>(0xC0u | (cp >> 6));
            ubuf[1] = static_cast<char>(0x80u | (cp & 0x3Fu));
            un = 2;
        } else if (cp < 0x10000u) {
            ubuf[0] = static_cast<char>(0xE0u | (cp >> 12));
            ubuf[1] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
            ubuf[2] = static_cast<char>(0x80u | (cp & 0x3Fu));
            un = 3;
        } else {
            ubuf[0] = static_cast<char>(0xF0u | (cp >> 18));
            ubuf[1] = static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
            ubuf[2] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
            ubuf[3] = static_cast<char>(0x80u | (cp & 0x3Fu));
            un = 4;
        }
        try {
            line.append(ubuf, un);
        } catch (const std::bad_alloc&) {
            return false;
        }
        return true;
    };

    auto flush_line = [&]() -> int {
        // 1=ok, 0=page full, -1=oom；标志与 MaterializeTxtPage 对齐
        if (line.empty()) {
            line_started = false;
            line_para_start = false;
            line_w = 0;
            return 1;
        }
        if (line_para_start) {
            StripLeadingParaWs(line);
        }
        if (line.empty()) {
            line_started = false;
            line_para_start = false;
            line_w = 0;
            return 1;
        }
        // 与 Materialize 一致：章名顶格加粗；折行续段并入上一项；章名另起一页
        const bool is_chap =
            line_para_start && !para_saw_indent_ws && IsTxtChapterTitleLine(line);
        if (is_chap && !live_preview_page_.items.empty()) {
            return 0;
        }
        const bool chap_cont =
            !line_para_start && !live_preview_page_.items.empty() &&
            live_preview_page_.items.back().chapter_title;
        if (chap_cont) {
            try {
                live_preview_page_.items.back().text += line;
            } catch (const std::bad_alloc&) {
                line.clear();
                return -1;
            }
            line.clear();
            y += line_stride;
            line_started = false;
            line_para_start = false;
            para_saw_indent_ws = false;
            line_w = 0;
            return 1;
        }
        const bool indent = line_para_start && !is_chap &&
                            (para_saw_indent_ws || !IsTxtChapterTitleLine(line));
        const bool after_chap =
            line_para_start && !live_preview_page_.items.empty() &&
            live_preview_page_.items.back().chapter_title;
        const lv_coord_t gap =
            para_gap_ + (after_chap ? kReaderChapterTitleAfterGap : 0);
        lv_coord_t need_y = y;
        if (line_para_start && y > 0) {
            if (y + gap + line_stride > viewport_h_ && !live_preview_page_.items.empty()) {
                return 0;
            }
            need_y += gap;
        }
        // 章名有副标题时展示两行，预留高度
        lv_coord_t chap_extra = 0;
        if (is_chap) {
            std::string ord;
            std::string name;
            if (SplitChapterHeading(line, ord, name) && !name.empty()) {
                chap_extra = line_stride;
            }
        }
        if (need_y + line_stride + chap_extra > viewport_h_ && !live_preview_page_.items.empty()) {
            return 0;
        }
        if (line_para_start && y > 0) {
            y += gap;
        }
        try {
            PageItem item;
            item.kind = ContentKind::kText;
            item.para_indent = indent;
            item.para_gap_before = line_para_start;
            item.chapter_title = is_chap;
            item.text = is_chap ? FormatChapterHeadingDisplay(line) : std::move(line);
            live_preview_page_.items.push_back(std::move(item));
        } catch (const std::bad_alloc&) {
            line.clear();
            return -1;
        }
        line.clear();
        y += line_stride + chap_extra;
        line_started = false;
        line_para_start = false;
        para_saw_indent_ws = false;
        line_w = 0;
        return 1;
    };

    uint32_t cp = 0;
    uint32_t goff = 0;
    uint16_t glen = 0;
    bool is_nl = false;
    while (stream.NextGlyph(&cp, &goff, &glen, &is_nl)) {
        if (is_nl) {
            if (line_started) {
                const int fr = flush_line();
                if (fr == 0) {
                    live_preview_next_off_ = line_start_off;
                    live_preview_next_at_para_start_ = line_para_start;
                    live_preview_has_next_ = true;
                    page_full = true;
                    break;
                }
                if (fr < 0) {
                    return false;
                }
            }
            para_at_start = true;
            para_saw_indent_ws = false;
            continue;
        }
        if (para_at_start && !line_started && IsTxtParaIndentWs(cp)) {
            para_saw_indent_ws = true;
            continue;
        }
        lv_coord_t cw = GlyphWidth(font_, cp);
        if ((cp == 0x20 || cp == 0xA0) && cw <= 0) {
            cw = std::max<lv_coord_t>(1, FontLineHeight(font_) / 3);
        }
        if (line_started && (line_w + cw > viewport_w_ || line.size() + 4 > kMaxLineSourceBytes)) {
            const int fr = flush_line();
            if (fr == 0) {
                live_preview_next_off_ = line_start_off;
                live_preview_next_at_para_start_ = line_para_start;
                live_preview_has_next_ = true;
                page_full = true;
                break;
            }
            if (fr < 0) {
                return false;
            }
        }
        if (!line_started) {
            line_started = true;
            line_start_off = goff;
            if (para_at_start) {
                line_w = indent_w;
                line_para_start = true;
                para_at_start = false;
            } else {
                line_w = 0;
            }
        }
        if (!append_cp(cp)) {
            return false;
        }
        line_w += cw;
    }
    if (!page_full && line_started) {
        const int fr = flush_line();
        if (fr < 0) {
            return false;
        }
        if (fr == 0) {
            live_preview_next_off_ = line_start_off;
            live_preview_next_at_para_start_ = line_para_start;
            live_preview_has_next_ = true;
            page_full = true;
        }
    }
    if (!page_full) {
        live_preview_next_off_ = stream.Tell();
        live_preview_next_at_para_start_ = para_at_start;
        live_preview_has_next_ = false;
    }
    if (live_preview_page_.items.empty()) {
        PageItem item;
        item.kind = ContentKind::kText;
        item.text = UiBlank();
        live_preview_page_.items.push_back(std::move(item));
    }
    live_preview_active_ = true;
    return true;
}

bool BookSession::RelayoutCurrentChapter(const LayoutAnchor& anchor) {
    if (!IsChapteredBook()) {
        return false;
    }
    const int ch = anchor.valid ? anchor.chapter : chapter_index_;
    const int old_page = anchor.valid ? anchor.page : page_index_;
    const int old_pc = anchor.valid ? std::max(1, anchor.page_count) : std::max(1, PageCount());

    // 先读盘再分页：读失败时勿走 Load*Chapter（会写成「本章无文本」冲掉内存里仍完好的页）
    std::vector<ContentBlock> blocks;
    OpenProgressFn saved_fn = progress_fn_;
    void* saved_user = progress_user_;
    progress_fn_ = nullptr;
    progress_user_ = nullptr;
    bool loaded = false;
    try {
        if (ebook_ != nullptr) {
            if (ch < 0 || static_cast<size_t>(ch) >= ebook_spine_.size()) {
                progress_fn_ = saved_fn;
                progress_user_ = saved_user;
                return false;
            }
            const int file_ch = ebook_spine_[static_cast<size_t>(ch)];
            const uint32_t uncomp = ebook_->ChapterUncompressedTotal(file_ch);
            if (uncomp > kMaxChapterUncompForPaginate) {
                progress_fn_ = saved_fn;
                progress_user_ = saved_user;
                return false;
            }
            loaded = ebook_->LoadChapterBlocks(file_ch, blocks);
            if (loaded) {
                EnsureChapterTitleInBlocks(ebook_->ChapterTitle(file_ch), ch + 1, blocks);
                loaded = !blocks.empty();
            }
        } else if (epub_ != nullptr) {
            if (ch < 0 || ch >= epub_->SpineCount()) {
                progress_fn_ = saved_fn;
                progress_user_ = saved_user;
                return false;
            }
            loaded = epub_->LoadChapterBlocks(ch, blocks) && !blocks.empty();
            if (loaded) {
                std::string title;
                if (static_cast<size_t>(ch) < toc_.size()) {
                    title = toc_[static_cast<size_t>(ch)].title;
                }
                EnsureChapterTitleInBlocks(std::move(title), ch + 1, blocks);
            }
        }
    } catch (const std::bad_alloc&) {
        loaded = false;
        ESP_LOGE(TAG, "relayout chapter OOM ch=%d", ch);
    }
    progress_fn_ = saved_fn;
    progress_user_ = saved_user;

    if (!loaded) {
        // 内部 RAM 紧时 SD DMA 会失败；保留当前 pages_，避免设置行距/边距后闪「无文本」
        ESP_LOGW(TAG, "relayout: chapter read fail ch=%d, keep current pages (int=%u)", ch,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        InvalidateChapterPageTable();
        return true;
    }
    if (!BuildPagesFromBlocks(blocks)) {
        return false;
    }
    chapter_index_ = ch;
    NoteChapterPageCount(ch, static_cast<int>(pages_.size()));

    const int new_pc = std::max(1, PageCount());
    int page = (old_page * new_pc) / old_pc;
    if (page >= new_pc) {
        page = new_pc - 1;
    }
    if (page < 0) {
        page = 0;
    }
    page_index_ = page;
    InvalidateChapterPageTable();
    SaveProgress();
    return true;
}

bool BookSession::BeginLiveRelayout(LayoutAnchor* anchor_inout) {
    if (!open_ || font_ == nullptr) {
        return false;
    }
    LayoutAnchor anchor = CaptureLayoutAnchor();
    if (anchor_inout != nullptr && anchor_inout->valid) {
        anchor = *anchor_inout;
    } else if (anchor_inout != nullptr) {
        *anchor_inout = anchor;
    }
    if (!anchor.valid) {
        return false;
    }

    if (txt_mode_) {
        // 全量重建前把章首行映成字节锚点，编排中底栏/目录仍可用旧 toc_
        for (auto& ent : toc_) {
            if (ent.first_line < txt_lines_len_ && txt_lines_ != nullptr) {
                ent.first_byte_off = txt_lines_[ent.first_line].off;
            }
        }
        live_preview_hist_.clear();
        if (!FillTxtLivePreview(anchor.txt_byte_off, anchor.txt_at_para_start)) {
            ClearLivePreview();
            return false;
        }
        if (anchor_inout != nullptr) {
            *anchor_inout = anchor;
            anchor_inout->txt_byte_off = live_preview_byte_off_;
            anchor_inout->txt_at_para_start = live_preview_at_para_start_;
        }
        return true;
    }
    if (IsChapteredBook()) {
        ClearLivePreview();
        return RelayoutCurrentChapter(anchor);
    }
    return false;
}

bool BookSession::FinishTxtRelayout(const LayoutAnchor& anchor) {
    if (!open_ || !txt_mode_) {
        ClearLivePreview();
        return true;
    }
    if (txt_empty_) {
        ClearLivePreview();
        page_index_ = 0;
        SaveProgress();
        return true;
    }
    if (info_.path.empty()) {
        return false;
    }
    FILE* fp = std::fopen(info_.path.c_str(), "rb");
    if (fp == nullptr) {
        return false;
    }
    if (std::fseek(fp, 0, SEEK_END) != 0) {
        std::fclose(fp);
        return false;
    }
    const long sz_l = std::ftell(fp);
    if (sz_l < 0) {
        std::fclose(fp);
        return false;
    }
    const size_t sz = static_cast<size_t>(sz_l);
    OpenProgressFn saved_fn = progress_fn_;
    void* saved_user = progress_user_;
    progress_fn_ = nullptr;
    progress_user_ = nullptr;
    const bool ok = StreamPaginateTxt(fp, sz);
    progress_fn_ = saved_fn;
    progress_user_ = saved_user;
    std::fclose(fp);
    if (!ok) {
        return false;
    }
    if (!SaveTxtIndex(info_.path.c_str(), sz)) {
        ESP_LOGW(TAG, "txt idx save after relayout skipped");
    }
    // 优先用当前预览锚点（编排中可能已翻页/换参）
    const uint32_t off = live_preview_active_
                             ? live_preview_byte_off_
                             : (anchor.valid ? anchor.txt_byte_off : 0);
    page_index_ = PageIndexForTxtByteOff(off);
    if (page_index_ < 0) {
        page_index_ = 0;
    }
    if (page_index_ >= PageCount()) {
        page_index_ = std::max(0, PageCount() - 1);
    }
    txt_view_index_ = -1;
    ClearLivePreview();
    SaveProgress();
    ESP_LOGI(TAG, "txt relayout done pages=%d at=%d off=%u", PageCount(), page_index_ + 1,
             static_cast<unsigned>(off));
    return true;
}

void BookSession::FinalizeTocPages() {
    for (auto& ent : toc_) {
        ent.first_page = PageIndexForLine(ent.first_line);
        if (ent.first_line < txt_lines_len_ && txt_lines_ != nullptr) {
            ent.first_byte_off = txt_lines_[ent.first_line].off;
        }
    }
}

bool BookSession::TryPushTxtChapter(std::string title, uint32_t first_line) {
    auto& dest = toc_build_ != nullptr ? *toc_build_ : toc_;
    if (dest.size() >= kMaxTocEntries) {
        return false;
    }
    while (!title.empty() && (title.back() == '\r' || title.back() == '\n' || title.back() == ' ' ||
                              title.back() == '\t')) {
        title.pop_back();
    }
    StripLeadingParaWs(title);
    if (!IsTxtChapterTitleLine(title)) {
        return false;
    }
    if (title.size() > kMaxTocTitleBytes) {
        title.resize(kMaxTocTitleBytes);
    }
    if (!dest.empty() && dest.back().first_line == first_line) {
        return false;
    }
    TocEntry ent;
    ent.title = std::move(title);
    ent.first_line = first_line;
    if (first_line < txt_lines_len_ && txt_lines_ != nullptr) {
        ent.first_byte_off = txt_lines_[first_line].off;
    }
    ESP_LOGI(TAG, "chap toc+#%u line=%u '%.*s'", static_cast<unsigned>(dest.size() + 1),
             static_cast<unsigned>(first_line), 48, ent.title.c_str());
    dest.push_back(std::move(ent));
    return true;
}


int BookSession::TocCount() const {
    if (txt_mode_) {
        return static_cast<int>(toc_.size());
    }
    if (ebook_) {
        // 无原生目录标记时不回退成「按章数造目录」
        return ebook_->HasToc() ? static_cast<int>(toc_.size()) : 0;
    }
    if (IsChapteredBook()) {
        return !toc_.empty() ? static_cast<int>(toc_.size()) : chapter_count_;
    }
    return 0;
}

const TocEntry* BookSession::TocAt(int index) const {
    if (index < 0) {
        return nullptr;
    }
    if (static_cast<size_t>(index) >= toc_.size()) {
        return nullptr;
    }
    // TXT / EPUB / EBOOK 均可能填 toc_
    if (txt_mode_ || IsChapteredBook()) {
        return &toc_[static_cast<size_t>(index)];
    }
    return nullptr;
}

int BookSession::CurrentTocIndex() const {
    if (txt_mode_) {
        if (toc_.empty()) {
            return -1;
        }
        // 编排预览：按字节锚点；页码在全量重建中不可靠
        if (live_preview_active_) {
            const uint32_t off = live_preview_byte_off_;
            int best = 0;
            for (size_t i = 0; i < toc_.size(); ++i) {
                if (toc_[i].first_byte_off <= off) {
                    best = static_cast<int>(i);
                } else {
                    break;
                }
            }
            return best;
        }
        const int page = page_index_;
        int best = 0;
        for (size_t i = 0; i < toc_.size(); ++i) {
            if (toc_[i].first_page <= page) {
                best = static_cast<int>(i);
            } else {
                break;
            }
        }
        return best;
    }
    if (IsChapteredBook() && chapter_count_ > 0) {
        return chapter_index_;
    }
    return -1;
}

bool BookSession::GoToToc(int index) {
    if (txt_mode_) {
        if (index < 0 || static_cast<size_t>(index) >= toc_.size()) {
            return false;
        }
        if (live_preview_active_) {
            live_preview_hist_.clear();
            const uint32_t off = toc_[static_cast<size_t>(index)].first_byte_off;
            return FillTxtLivePreview(off, true);
        }
        return GoToPage(toc_[static_cast<size_t>(index)].first_page);
    }
    if (live_preview_active_) {
        return false;
    }
    if (ebook_ && !ebook_->HasToc()) {
        return false;
    }
    if (IsChapteredBook()) {
        if (index < 0 || index >= chapter_count_) {
            return false;
        }
        const bool ok = (ebook_ != nullptr) ? LoadEbookChapter(index) : LoadEpubChapter(index);
        if (!ok) {
            return false;
        }
        SaveProgress();
        return true;
    }
    return false;
}

std::string BookSession::CurrentTocTitle() const {
    if (txt_mode_) {
        const int i = CurrentTocIndex();
        if (i < 0 || static_cast<size_t>(i) >= toc_.size()) {
            return {};
        }
        return toc_[static_cast<size_t>(i)].title;
    }
    if (IsChapteredBook() && chapter_count_ > 0) {
        if (static_cast<size_t>(chapter_index_) < toc_.size()) {
            const std::string& t = toc_[static_cast<size_t>(chapter_index_)].title;
            if (!IsPlaceholderChapterTitle(t)) {
                return t;
            }
        }
        return FormatChapterOrdinalTitle(chapter_index_ + 1);
    }
    return {};
}

int BookSession::ReadingProgressX10() const {
    if (IsChapteredBook() && ChapterCount() > 1) {
        const int page = BookCurrentPage() + 1;
        const int pages = BookPageCount();
        if (pages <= 0) {
            return 0;
        }
        int pct_x10 = page * 1000 / pages;
        if (pct_x10 < 0) {
            pct_x10 = 0;
        }
        if (pct_x10 > 1000) {
            pct_x10 = 1000;
        }
        return pct_x10;
    }
    const int page = CurrentPage() + 1;
    const int pages = PageCount();
    if (pages <= 0) {
        return 0;
    }
    int pct_x10 = page * 1000 / pages;
    if (pct_x10 < 0) {
        pct_x10 = 0;
    }
    if (pct_x10 > 1000) {
        pct_x10 = 1000;
    }
    return pct_x10;
}


}  // namespace reader
