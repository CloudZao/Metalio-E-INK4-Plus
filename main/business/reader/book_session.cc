#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <new>
#ifndef EBOOK_EMULATOR
#include <deque>
#endif
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#ifndef EBOOK_EMULATOR
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif
#include <sys/stat.h>
#include <unistd.h>
#include "book_session.h"
#include "book_library.h"
#include "book_storage_paths.h"
#ifndef EBOOK_EMULATOR
#include "assets/lang_config.h"
#include "assets/lang_reader.h"
#include "infra/diagnostics/firmware_memory.h"
#include "boards/common/storage/sd_access.h"
#include "infra/storage/sd_data_layout.h"
#include "book_home_snapshot.h"
#include "book_progress_cache.h"
#endif
#include "infra/image/image_geometry.h"
#include "infra/image/image_decode.h"
#include "infra/storage/file_write.h"
#include "sysmgmt/tasks/task_executor.h"
#include "text_encoding.h"
#include "text_spacing.h"
#include "txt_chapter.h"
#include "vertical_text.h"

#ifndef EBOOK_EMULATOR
static const char* UiBlank() {
    return LangString_BOOK_BLANK;
}

static const char* UiChapterNoText() {
    return LangString_BOOK_CHAPTER_NO_TEXT;
}

static const char* UiChapterFmt() {
    return LangString_BOOK_CHAPTER_FMT;
}

static const char* UiPageFmt() {
    return LangString_BOOK_PAGE_FMT;
}

static const char* UiBookmarkLayoutBusy() {
    return LangString_BOOK_BOOKMARK_LAYOUT_BUSY;
}

static const char* UiBookmarkAddMemory() {
    return LangString_BOOK_BOOKMARK_ADD_MEMORY;
}

static const char* UiBookmarkImageSuffix() {
    return LangString_BOOK_BOOKMARK_IMAGE_SUFFIX;
}

static const char* UiBookmarkImage() {
    return LangString_BOOK_BOOKMARK_IMAGE;
}
static const char* UiIllustration() {
    return LangString_BOOK_ILLUSTRATION;
}
#else
static const char* UiBlank() {
    return "(Blank)";
}

static const char* UiChapterNoText() {
    return "(No text in chapter)";
}

static const char* UiChapterFmt() {
    return "Ch.%d";
}

static const char* UiPageFmt() {
    return "Page %d";
}

static const char* UiBookmarkLayoutBusy() {
    return "Laying out text. Add the bookmark shortly.";
}

static const char* UiBookmarkAddMemory() {
    return "Not enough memory to add a bookmark";
}

static const char* UiBookmarkImageSuffix() {
    return " (Image or chapter page)";
}

static const char* UiBookmarkImage() {
    return "Image or chapter page";
}
static const char* UiIllustration() {
    return "[Illustration]";
}
#endif

static constexpr const char* TAG = "BookSession";
static constexpr size_t kMaxTxtBytes = static_cast<size_t>(INT32_MAX) - 1;  // 源偏移和 fseek 的表示边界
static constexpr size_t kTxtReadChunk = 1024;
static constexpr uint32_t kMaxLineSourceBytes = 512;
static constexpr uint32_t kTxtIdxVersion = 24;  // 中英文标题与副标题样式写入行索引
static constexpr uint16_t kTxtLineParaStart = 0x8000u;
// 单章解压合计上限：与 tools/ebook/epdbook/format.py DEVICE_CHAPTER_MAX_UNCOMP 一致。
// 超限拒绝加载；分页 OOM 软失败。转换默认拆到 DEFAULT_CHAPTER_MAX_BYTES(256KB)。

#pragma pack(push, 1)

struct BookSessionTxtIdxHeader {
    char magic[4];  // "TXI1"
    uint32_t version;
    uint32_t file_size;
    uint16_t viewport_w;
    uint16_t viewport_h;
    uint16_t line_spacing_percent;
    uint16_t para_gap;
    uint32_t font_fp;
    uint8_t as_gb18030;
    uint8_t reserved[3];
    uint32_t line_count;
    uint32_t page_count;
};

struct BookSessionTxtIdxLineDisk {
    uint32_t off;
    uint16_t len;
};

#pragma pack(pop)

static lv_coord_t FontLineHeight(const lv_font_t* font) {
    return font != nullptr ? font->line_height : 28;
}

static lv_coord_t GlyphWidth(const lv_font_t* font, uint32_t cp) {
    if (font == nullptr) {
        return 12;
    }
    const lv_coord_t w = static_cast<lv_coord_t>(lv_font_get_glyph_width(font, cp, 0));
    return w > 0 ? w : 1;
}

static bool IsTxtParaIndentWs(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == 0xA0u || cp == 0x3000u;
}

static lv_coord_t TxtParaIndentWidth(const lv_font_t* font) {
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

static void StripLeadingParaWs(std::string& s) {
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

static bool FlushLineToPage(std::string& line, BookPageStore& pages, Page& cur, lv_coord_t& y,
                            const TextLineLayout& layout, int& row, lv_coord_t gap_before, bool para_indent = false,
                            bool chapter_title = false, bool para_gap_before = false,
                            uint32_t source_block = UINT32_MAX, uint32_t source_begin = 0, uint32_t source_end = 0,
                            bool justify = false, bool hyphenated = false, bool centered = false,
                            uint32_t source_chapter = 0, uint64_t source_progress = 0) {
    if (line.empty()) {
        return true;
    }
    if (layout.count <= 0) {
        return false;
    }
    if ((row >= layout.count || y + gap_before + layout.CellHeight(row) > layout.extent) && !cur.items.empty()) {
        if (!pages.Push(std::move(cur))) {
            return false;
        }
        cur = Page{};
        y = 0;
        row = 0;
    }
    if (cur.items.empty()) {
        gap_before = 0;
    }
    try {
        PageItem item;
        item.source_chapter = source_chapter;
        item.source_progress = source_progress;
        item.source_block = source_block;
        item.source_begin = source_begin;
        item.source_end = source_end;
        item.kind = ContentKind::kText;
        item.para_indent = para_indent && !chapter_title;
        item.para_gap_before = para_gap_before;
        item.chapter_title = chapter_title;
        item.justify = justify && !chapter_title && !centered;
        item.hyphenated = hyphenated;
        item.centered = centered;
        item.text = std::move(line);
        line.clear();
        cur.items.push_back(std::move(item));
    } catch (const std::bad_alloc&) {
        line.clear();
        return false;
    }
    y += gap_before + layout.CellHeight(row++);
    return true;
}

static bool IsPlaceholderChapterTitle(const std::string& t) {
    return t.empty() || t == "未知" || t == "未命名" || t == "Unknown" || t == "Untitled";
}

static std::string FormatChapterOrdinalTitle(int n) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), UiChapterFmt(), n > 0 ? n : 1);
    return std::string(buf);
}

// 仅改首行：序数与章节名分行；其余正文不动
static void FormatChapterTitleFirstLine(std::string& text) {
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

// EBOOK 首段兼容处理；EPUB 仅复用旧源位置映射，不显示生成的标题。
// 有明确 meta（如「第18章」）时不因正文里另一行「第N章…」而改名。
// 展示：「第N章」单独一行，有章节名则换行紧跟居中。
static void EnsureChapterTitleInBlocks(std::string title, int display_1based, std::vector<ContentBlock>& blocks,
                                       bool* inserted = nullptr) {
    if (inserted) {
        *inserted = false;
    }

    if (!blocks.empty() && blocks[0].kind == ContentKind::kText) {
        std::string& text = blocks[0].text;
        StripLeadingParaWs(text);
        const size_t nl = text.find('\n');
        std::string first = (nl == std::string::npos) ? text : text.substr(0, nl);
        while (!first.empty() && (first.back() == ' ' || first.back() == '\t' || first.back() == '\r')) {
            first.pop_back();
        }
        if (IsPlaceholderChapterTitle(first)) {
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
        return;
    }
    // meta 为空/占位时，才用正文「第N章…」当章名
    if (IsPlaceholderChapterTitle(title) && !first.empty() && IsTxtChapterTitleLine(first)) {
        blocks[0].chapter_title = true;
        FormatChapterTitleFirstLine(blocks[0].text);
        return;
    }
    if (IsPlaceholderChapterTitle(title)) {
        title = FormatChapterOrdinalTitle(display_1based);
    }
    if (title.empty()) {
        return;
    }
    ContentBlock blk;
    blk.kind = ContentKind::kText;
    blk.text = FormatChapterHeadingDisplay(title);
    blk.chapter_title = true;
    blocks.insert(blocks.begin(), std::move(blk));
    if (inserted) {
        *inserted = true;
    }
}

static uint32_t EpubFirstSourceOffset(const std::string& raw, const std::string& displayed, uint32_t offset) {
    if (offset >= raw.size()) {
        return displayed.size() + offset - raw.size();
    }
    const std::string prefix = displayed.substr(0, displayed.find('\n'));
    size_t source = prefix.empty() ? 0 : raw.find(prefix), target = 0;
    if (source == std::string::npos) {
        source = 0;
    }
    while (source < offset && target < displayed.size()) {
        if (raw[source] == displayed[target]) {
            ++source;
            ++target;
        } else if (std::isspace(static_cast<unsigned char>(raw[source]))) {
            ++source;
        } else if (std::isspace(static_cast<unsigned char>(displayed[target]))) {
            ++target;
        } else {
            break;
        }
    }
    return static_cast<uint32_t>(target);
}

// 过滤缺少正文且后续重复出现的密集目录；保留不重复的卷章层级和短章节。
static void FilterDenseTxtToc(std::vector<TocEntry>& entries, const std::vector<uint32_t>& title_end_chars,
                              uint32_t total_chars) {
    auto body_chars = [&](size_t index) {
        if (index + 1 == entries.size()) {
            return total_chars - title_end_chars[index];
        }
        uint32_t next_title_chars = 0;
        const auto& title = entries[index + 1].title;
        for (size_t i = 0; i < title.size();) {
            uint32_t cp = 0;
            i += Utf8Next(reinterpret_cast<const uint8_t*>(title.data()) + i, title.size() - i, &cp);
            if (!IsTxtParaIndentWs(cp)) {
                ++next_title_chars;
            }
        }
        return title_end_chars[index + 1] - title_end_chars[index] - next_title_chars;
    };
    auto repeated_later = [&](size_t index, size_t begin) {
        std::string ordinal, name;
        const bool numbered = SplitChapterHeading(entries[index].title, ordinal, name);
        for (size_t i = begin; i < entries.size(); ++i) {
            if (!numbered) {
                if (entries[i].title == entries[index].title) {
                    return true;
                }
            } else {
                std::string next_ordinal, next_name;
                if (SplitChapterHeading(entries[i].title, next_ordinal, next_name) && next_ordinal == ordinal) {
                    return true;
                }
            }
        }
        return false;
    };
    size_t write = 0;
    for (size_t read = 0; read < entries.size();) {
        size_t end = read;
        while (end < entries.size() && body_chars(end) == 0) {
            ++end;
        }
        if (end - read >= 2 && end < entries.size()) {
            bool repeated = true;
            for (size_t i = read; repeated && i < end; ++i) {
                repeated = repeated_later(i, end);
            }
            if (repeated) {
                // 末项可能带有卷名等短附文，仍须确认后文存在对应章首。
                read = end;
                if (body_chars(end) < 80 && repeated_later(end, end + 1)) {
                    ++read;
                }
                continue;
            }
        }
        if (write != read) {
            entries[write] = std::move(entries[read]);
        }
        ++write;
        ++read;
    }
    entries.resize(write);
}

// 带回填缓冲的源文件字节流，按码点推进并记录绝对文件偏移。
// 解码缓冲使用堆内存，避免在阅读工作任务栈上长期保留大数组。
struct BookSessionTxtByteStream {
    FILE* fp = nullptr;
    bool as_gb18030 = false;
    uint8_t* buf = nullptr;
    size_t pos = 0;
    size_t end = 0;
    uint32_t file_base = 0;  // buf[0] 对应的文件偏移
    bool eof = false;

    BookSessionTxtByteStream() = default;

    ~BookSessionTxtByteStream() {
        FreeBuf();
    }

    BookSessionTxtByteStream(const BookSessionTxtByteStream&) = delete;
    BookSessionTxtByteStream& operator=(const BookSessionTxtByteStream&) = delete;

    void FreeBuf() {
        if (buf != nullptr) {
            heap_caps_free(buf);
            buf = nullptr;
        }
    }

    bool InitBuf() {
        FreeBuf();
        buf = static_cast<uint8_t*>(heap_caps_malloc(kTxtReadChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (buf == nullptr) {
#ifdef EBOOK_EMULATOR
            buf = static_cast<uint8_t*>(std::malloc(kTxtReadChunk));
#else
            const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
            if (heap_caps_get_free_size(caps) >= kFirmwareInternalFloorBytes + kTxtReadChunk) {
                buf = static_cast<uint8_t*>(heap_caps_malloc(kTxtReadChunk, caps));
            }
#endif
        }
        return buf != nullptr;
    }

    uint32_t Tell() const {
        return file_base + static_cast<uint32_t>(pos);
    }

    bool Seek(uint32_t offset) {
        if (offset >= file_base && offset <= file_base + end) {
            pos = offset - file_base;
            return true;
        }
        if (std::fseek(fp, static_cast<long>(offset), SEEK_SET) != 0) {
            return false;
        }
        file_base = offset;
        pos = end = 0;
        eof = false;
        return true;
    }

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
            if (as_gb18030) {
                if (b < 0x80) {
                    ++pos;
                    *cp = b;
                    *out_len = 1;
                    return true;
                }
                if (!Ensure(4) && end <= pos) {
                    return false;
                }
                uint32_t uni = 0;
                const size_t n = Gb18030Next(buf + pos, end - pos, &uni);
                pos += n;
                *cp = uni;
                *out_len = static_cast<uint32_t>(n);
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

// 探测一行容量及足以判断阈值的尾词，避免为异常长单词扫描整段正文。
static BookReadError ProbeTxtEnglishWord(BookSessionTxtByteStream& stream, uint32_t start, uint32_t end,
                                         std::string& word) {
    const uint32_t saved = stream.Tell();
    word.clear();
    if (!stream.Seek(start)) {
        ESP_LOGE(TAG, "operation=english_probe stage=seek errno=%d", errno);
        return BookReadError::kFileIo;
    }
    constexpr size_t limit = kMaxLineSourceBytes + 2 * (kReaderEnglishSuffixLimit + 1) + 2;
    bool apostrophe = false;
    BookReadError error = BookReadError::kNone;
    try {
        uint32_t cp = 0, offset = 0;
        uint16_t length = 0;
        bool newline = false;
        while (stream.NextGlyph(&cp, &offset, &length, &newline) && offset < end && !newline) {
            if (ReaderEnglishLetter(cp)) {
                if (apostrophe) {
                    word.push_back('\'');
                    apostrophe = false;
                }
                word.push_back(static_cast<char>(cp));
                if (word.size() >= limit) {
                    break;
                }
            } else if (cp == '\'' && !word.empty() && !apostrophe) {
                apostrophe = true;
            } else {
                break;
            }
        }
    } catch (const std::bad_alloc&) {
        error = BookReadError::kNoMemory;
        ESP_LOGE(TAG, "operation=english_probe stage=buffer result=no_memory");
    }
    const bool read_ok = !std::ferror(stream.fp);
    const int read_error = errno;
    const bool restored = stream.Seek(saved);
    if (!read_ok || !restored) {
        ESP_LOGE(TAG, "operation=english_probe stage=restore read=%d errno=%d", read_ok,
                 read_ok ? errno : read_error);
        error = BookReadError::kFileIo;
    }
    return error;
}

// 在现有回填缓冲内有界探测逻辑行，结束后恢复当前位置，不另开源文件。
static bool ProbeTxtParagraph(BookSessionTxtByteStream& stream, uint32_t start, std::string& text, bool& complete) {
    const uint32_t saved = stream.Tell();
    text.clear();
    complete = true;
    if (!stream.Seek(start)) {
        ESP_LOGE(TAG, "txt heading probe seek failed code=%d", errno);
        return false;
    }
    uint32_t cp = 0, offset = 0;
    uint16_t length = 0;
    bool newline = false;
    while (stream.NextGlyph(&cp, &offset, &length, &newline)) {
        if (newline) {
            break;
        }
        if (cp < 0x80) {
            text += static_cast<char>(cp);
        } else if (cp < 0x800) {
            text += static_cast<char>(0xC0 | (cp >> 6));
            text += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            text += static_cast<char>(0xE0 | (cp >> 12));
            text += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            text += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            text += static_cast<char>(0xF0 | (cp >> 18));
            text += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            text += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            text += static_cast<char>(0x80 | (cp & 0x3F));
        }
        if (text.size() > kMaxTxtChapterTitleBytes) {
            complete = false;
            text.clear();
            break;
        }
    }
    const bool read_ok = !std::ferror(stream.fp);
    const bool restored = stream.Seek(saved);
    if (!read_ok || !restored) {
        ESP_LOGE(TAG, "txt heading probe restore failed read=%d code=%d", read_ok, errno);
        return false;
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
    }
    return true;
}

static TxtHeadingKind KnownTxtHeading(const std::vector<TocEntry>& toc, uint32_t offset, const std::string& text) {
    const auto after = std::upper_bound(toc.begin(), toc.end(), offset, [](uint32_t value, const TocEntry& entry) {
        return value < entry.first_byte_off;
    });
    if (after == toc.begin()) {
        return TxtHeadingKind::kNone;
    }
    const auto& entry = *std::prev(after);
    if (offset == entry.first_byte_off) {
        return TxtHeadingKind::kChapter;
    }
    if (offset - entry.first_byte_off > entry.title.size() + 32) {
        return TxtHeadingKind::kNone;
    }
    std::string ordinal, name;
    return SplitChapterHeading(entry.title, ordinal, name) && !name.empty() && (text.empty() || text == name)
               ? TxtHeadingKind::kSubtitle
               : TxtHeadingKind::kNone;
}

void BookSession::ClearTxtIndex() {
    if (txt_fp_ != nullptr) {
        std::fclose(txt_fp_);
        txt_fp_ = nullptr;
    }
    txt_lines_.Reset();
    txt_lines_len_ = 0;
    txt_page_first_line_.Reset();
    txt_page_count_ = 0;
    live_preview_active_ = false;
    txt_preview_relayout_pending_ = false;
    live_preview_byte_off_ = 0;
    live_preview_at_para_start_ = true;
    live_preview_next_off_ = 0;
    live_preview_next_at_para_start_ = true;
    live_preview_has_next_ = false;
    live_preview_page_ = Page{};
    live_preview_hist_.clear();
    txt_mode_ = false;
    txt_as_gb18030_ = false;
    txt_empty_ = false;
    ClearToc();
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
    return BookStoragePath(txt_path, ".idx");
}

std::string BookSession::ProgressPath(const char* book_path) {
    if (book_path == nullptr) {
        return {};
    }
    return BookStoragePath(book_path, ".pos");
}

static bool IsProgressMagic(const char magic[4]) {
    return std::memcmp(magic, "POS1", 4) == 0 || std::memcmp(magic, "POS2", 4) == 0 ||
           std::memcmp(magic, "POS3", 4) == 0 || std::memcmp(magic, "POS4", 4) == 0;
}

static bool MagicHasLifetime(const char magic[4]) {
    return std::memcmp(magic, "POS2", 4) == 0 || std::memcmp(magic, "POS3", 4) == 0 || std::memcmp(magic, "POS4", 4) == 0;
}

static bool MagicHasDaily(const char magic[4]) {
    return std::memcmp(magic, "POS3", 4) == 0 || std::memcmp(magic, "POS4", 4) == 0;
}

static constexpr int64_t kMaxFoldDeltaUs =
    6LL * 3600LL * 1000000LL;  // 单次 fold 上限：防 checkpoint 挂掉 / 浅睡异常导致一次灌入数天秒数

#pragma pack(push, 1)

struct BookSessionProgressBlob {
    char magic[4];             // 进度格式标识
    uint32_t chapter;          // 章节序号
    uint32_t page;             // 章节内页码
    uint32_t pct_x10;          // 千分比阅读进度
    uint32_t reading_seconds;  // 累计阅读秒数
    uint32_t day_id;           // 本地日期编号
    uint32_t daily_seconds;    // 对应日期内的阅读秒数
    uint32_t anchor_magic;     // TXT 锚点扩展标识
    uint32_t txt_anchor;       // TXT 字节偏移
    uint32_t txt_para;         // 锚点是否位于段首
    BookSourcePosition source; // 原始正文锚点
    uint64_t file_size;         // 书籍身份
    int64_t mtime;              // 书籍修改时间
    uint32_t format;            // 书籍格式
    uint32_t parser_version;    // 源位置解释版本
    uint32_t checksum;          // 完整快照校验
};

#pragma pack(pop)
static_assert(sizeof(BookSessionProgressBlob) == 80, "POS4 snapshot size");

static uint32_t ProgressChecksum(const BookSessionProgressBlob& blob) {
    uint32_t hash = 2166136261u;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&blob);
    for (size_t i = 0; i < offsetof(BookSessionProgressBlob, checksum); ++i) {
        hash = (hash ^ bytes[i]) * 16777619u;
    }
    return hash;
}

static FILE* OpenProgressRecord(const char* path, bool* recovered = nullptr, const char* temporary_path = nullptr) {
    if (recovered) {
        *recovered = false;
    }
    FILE* fp = std::fopen(path, "rb");
    if (fp || errno != ENOENT) {
        return fp;
    }
    // 正式记录缺失时，读取替换失败后保留的临时快照。
    std::string temporary;
    if (!temporary_path) {
        temporary = std::string(path) + ".tmp";
        temporary_path = temporary.c_str();
    }
    fp = std::fopen(temporary_path, "rb");
    if (fp && recovered) {
        *recovered = true;
    }
    return fp;
}

static void LogProgressFailure(const char* stage) {
    static std::atomic<int64_t> last_us{0};
    static std::atomic<unsigned> suppressed{0};
    const int error = errno;
    const auto now = esp_timer_get_time();
    auto previous = last_us.load();
    if ((previous == 0 || now - previous >= 5000000) && last_us.compare_exchange_strong(previous, now)) {
        ESP_LOGE(TAG, "progress %s failed errno=%d suppressed=%u", stage, error, suppressed.exchange(0));
    } else {
        ++suppressed;
    }
}

#ifndef EBOOK_EMULATOR
struct BookSessionProgressWrite {
    SdAccessDeferredTransaction access;  // 排队期间阻止卸卡
    std::string path;                    // 进度记录路径
    std::string book;                    // 对应书籍路径
    BookSessionProgressBlob blob{};          // 排队时捕获的完整快照
    std::atomic<bool> cancelled{false};  // 删除书籍后禁止已出队任务重新写回进度
};

static std::mutex book_file_mu;
static std::vector<std::pair<const BookSession*, std::string>> open_books;
static std::mutex progress_write_mu;
static std::deque<std::unique_ptr<BookSessionProgressWrite>> progress_writes;
static BookSessionProgressWrite* progress_inflight = nullptr;
static bool progress_writer_running = false;

struct BookSessionProgressInFlightGuard {
    ~BookSessionProgressInFlightGuard() {
        std::lock_guard<std::mutex> lock(progress_write_mu);
        progress_inflight = nullptr;
    }
};

static bool ReadProgressBaseline(const char* path, BookSessionProgressBlob& previous) {
    FILE* fp = OpenProgressRecord(path);
    if (!fp) {
        if (errno == ENOENT) {
            return true;
        }
        ESP_LOGW(TAG, "progress statistics baseline unavailable stage=open errno=%d", errno);
        return false;
    }
    const size_t bytes = std::fread(&previous, 1, sizeof(previous), fp);
    const bool read_failed = std::ferror(fp) != 0;
    const bool close_failed = std::fclose(fp) != 0;
    if (read_failed || close_failed || bytes < 12 || !IsProgressMagic(previous.magic)) {
        ESP_LOGW(TAG, "progress statistics baseline unavailable stage=read bytes=%u read_error=%d close_error=%d",
                 static_cast<unsigned>(bytes), read_failed, close_failed);
        return false;
    }
    if (std::memcmp(previous.magic, "POS4", 4) == 0 &&
        (bytes != sizeof(previous) || previous.checksum != ProgressChecksum(previous))) {
        ESP_LOGW(TAG, "progress statistics baseline unavailable stage=checksum");
        return false;
    }
    if (bytes < 16) {
        previous.pct_x10 = 0;
    }
    if (bytes < 20 || !MagicHasLifetime(previous.magic)) {
        previous.reading_seconds = 0;
    }
    return true;
}

static bool HasRecoverableProgressWrite(const BookSessionProgressWrite& job, const char* temporary_path) {
    bool recovered = false;
    FILE* fp = OpenProgressRecord(job.path.c_str(), &recovered, temporary_path);
    if (!fp) {
        return false;
    }
    BookSessionProgressBlob temporary{};
    const size_t bytes = std::fread(&temporary, 1, sizeof(temporary), fp);
    const bool complete = bytes == sizeof(temporary) && std::fgetc(fp) == EOF && !std::ferror(fp);
    const bool closed = std::fclose(fp) == 0;
    return recovered && complete && closed && std::memcmp(&temporary, &job.blob, sizeof(temporary)) == 0;
}

static void PersistProgressWrite(BookSessionProgressWrite& job) {
    SdAccessTransaction transaction(job.access);
    if (!transaction) {
        errno = EBUSY;
        LogProgressFailure("storage_access");
        return;
    }
    BookSessionProgressBlob previous{};
    bool baseline_loaded = false;
    bool previous_known = false;
    bool delivered = false;
    bool aggregate_applied = false;
    std::string temporary_path;
    const char* stage = "baseline";
    // 重试只覆盖本次快照；已经交付的文件和统计不得重复写入或累加。
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        try {
            if (!baseline_loaded) {
                stage = "baseline";
                previous_known = ReadProgressBaseline(job.path.c_str(), previous);
                baseline_loaded = true;
            }
            if (!delivered) {
                // 在改动文件前准备恢复路径，避免落盘后因路径分配失败而漏记聚合增量。
                stage = "prepare";
                temporary_path = job.path + ".tmp";
                stage = "write";
                const bool committed =
                    WriteFileAtomic(job.path.c_str(), reinterpret_cast<const uint8_t*>(&job.blob), sizeof(job.blob));
                if (!committed && !HasRecoverableProgressWrite(job, temporary_path.c_str())) {
                    return;
                }
                delivered = true;
                if (committed) {
                    ESP_LOGD(TAG, "operation=save_progress result=committed");
                } else {
                    ESP_LOGW(TAG, "operation=save_progress result=recoverable_temporary_record");
                }
            }
            if (previous_known && !aggregate_applied) {
                stage = "aggregate";
                BookHomeSnapshotUpdateAggregateProgress(previous.reading_seconds, previous.pct_x10 >= 1000,
                                                        job.blob.reading_seconds, job.blob.pct_x10 >= 1000);
                aggregate_applied = true;
            }
            stage = "recent";
            BookHomeSnapshotNoteOpenedBook(job.book.c_str());
            BookSession::ProgressPeek snapshot;
            snapshot.progress_x10 = job.blob.pct_x10;
            snapshot.reading_seconds = job.blob.reading_seconds;
            snapshot.daily_seconds = job.blob.daily_seconds;
            stage = "cache";
            BookProgressCacheUpsert(job.book.c_str(), snapshot);
            return;
        } catch (const std::bad_alloc&) {
            if (attempt == 2) {
                ESP_LOGE(TAG, "operation=save_progress stage=%s result=no_memory delivered=%d; continuing queue", stage,
                         delivered);
                return;
            }
            if (attempt == 0) {
                ESP_LOGW(TAG, "operation=save_progress stage=%s result=no_memory; retrying current snapshot", stage);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void DrainProgressWrites() {
    for (;;) {
        std::unique_ptr<BookSessionProgressWrite> job;
        {
            std::lock_guard<std::mutex> lock(progress_write_mu);
            if (progress_writes.empty()) {
                progress_writer_running = false;
                return;
            }
            job = std::move(progress_writes.front());
            progress_writes.pop_front();
            progress_inflight = job.get();
        }
        BookSessionProgressInFlightGuard inflight_guard;
        std::lock_guard<std::mutex> file_lock(book_file_mu);
        if (!job->cancelled.load()) {
            PersistProgressWrite(*job);
        }
    }
}

static bool QueueProgressWrite(const std::string& path, const std::string& book, const BookSessionProgressBlob& blob) try {
    auto job = std::make_unique<BookSessionProgressWrite>();
    if (!job->access) {
        errno = EBUSY;
        return false;
    }
    job->path = path;
    job->book = book;
    job->blob = blob;
    std::lock_guard<std::mutex> lock(progress_write_mu);
    bool merged = false;
    for (auto& queued : progress_writes) {
        if (queued->path == path) {
            queued->blob = blob;
            merged = true;
            break;
        }
    }
    if (!merged) {
        if (progress_writes.size() >= 8) {
            errno = ENOBUFS;
            return false;
        }
        progress_writes.push_back(std::move(job));
    }
    if (!progress_writer_running) {
        progress_writer_running = true;
        if (!TaskExecutor::Schedule(DrainProgressWrites, TaskDomain::Storage)) {
            progress_writer_running = false;
            if (!merged) {
                progress_writes.pop_back();
            }
            errno = EAGAIN;
            return false;
        }
    }
    return true;
} catch (const std::bad_alloc&) {
    errno = ENOMEM;
    return false;
}

static bool ReadQueuedProgress(const char* path, BookSessionProgressBlob& blob) {
    std::lock_guard<std::mutex> lock(progress_write_mu);
    for (auto it = progress_writes.rbegin(); it != progress_writes.rend(); ++it) {
        if ((*it)->path == path) {
            blob = (*it)->blob;
            return true;
        }
    }
    if (progress_inflight && progress_inflight->path == path) {
        blob = progress_inflight->blob;
        return true;
    }
    return false;
}
#endif

BookSession::PosLoadStatus BookSession::LoadPosFileOnce(const char* pos_path, PosFileData& out) {
    out = {};
    if (pos_path == nullptr || pos_path[0] == '\0') {
        return PosLoadStatus::kMissing;
    }
    FILE* fp = OpenProgressRecord(pos_path);
    if (fp == nullptr) {
        return errno == ENOENT ? PosLoadStatus::kMissing : PosLoadStatus::kInvalid;
    }
    const bool head_ok = std::fread(out.magic, 1, 4, fp) == 4 &&
                         std::fread(&out.chapter, sizeof(out.chapter), 1, fp) == 1 &&
                         std::fread(&out.page, sizeof(out.page), 1, fp) == 1;
    if (!head_ok || !IsProgressMagic(out.magic)) {
        std::fclose(fp);
        return PosLoadStatus::kInvalid;
    }
    if (std::memcmp(out.magic, "POS4", 4) == 0) {
        BookSessionProgressBlob blob{};
        std::rewind(fp);
        const bool complete = std::fread(&blob, 1, sizeof(blob), fp) == sizeof(blob) &&
                              std::fgetc(fp) == EOF && !std::ferror(fp);
        const bool closed = std::fclose(fp) == 0;
        if (!complete || !closed || blob.checksum != ProgressChecksum(blob) || blob.txt_para > 1) {
            return PosLoadStatus::kInvalid;
        }
        out.pct_x10 = blob.pct_x10;
        out.reading_seconds = blob.reading_seconds;
        out.day_id = blob.day_id;
        out.daily_seconds = blob.daily_seconds;
        out.has_pct = out.has_seconds = out.has_daily = true;
        out.has_source_anchor = true;
        out.source_anchor = blob.source;
        out.file_size = blob.file_size;
        out.mtime = blob.mtime;
        out.format = blob.format;
        out.parser_version = blob.parser_version;
        out.has_txt_anchor = blob.anchor_magic == 0x31585442;
        out.txt_anchor = blob.txt_anchor;
        out.txt_para_start = blob.txt_para != 0;
        return PosLoadStatus::kOk;
    }
    // 旧逻辑兼容：POS1/2/3 允许缺少可选尾部；原页码只用于恢复旧正文位置。
    out.has_pct = std::fread(&out.pct_x10, sizeof(out.pct_x10), 1, fp) == 1;
    if (out.has_pct) {
        out.has_seconds = std::fread(&out.reading_seconds, sizeof(out.reading_seconds), 1, fp) == 1;
    }
    if (out.has_seconds) {
        out.has_daily = std::fread(&out.day_id, sizeof(out.day_id), 1, fp) == 1 &&
                        std::fread(&out.daily_seconds, sizeof(out.daily_seconds), 1, fp) == 1;
    }
    uint32_t marker = 0, para = 0;
    if (out.has_daily && std::fread(&marker, sizeof(marker), 1, fp) == 1 && marker == 0x31585442 &&
        std::fread(&out.txt_anchor, sizeof(out.txt_anchor), 1, fp) == 1 &&
        std::fread(&para, sizeof(para), 1, fp) == 1 && para <= 1) {
        out.has_txt_anchor = true;
        out.txt_para_start = para != 0;
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
    #ifndef EBOOK_EMULATOR
    BookSessionProgressBlob queued{};
    if (pos_path && ReadQueuedProgress(pos_path, queued)) {
        out = {};
        std::memcpy(out.magic, queued.magic, sizeof(out.magic));
        out.chapter = queued.chapter;
        out.page = queued.page;
        out.pct_x10 = queued.pct_x10;
        out.reading_seconds = queued.reading_seconds;
        out.day_id = queued.day_id;
        out.daily_seconds = queued.daily_seconds;
        out.has_pct = out.has_seconds = out.has_daily = true;
        out.has_txt_anchor = queued.anchor_magic == 0x31585442;
        out.txt_anchor = queued.txt_anchor;
        out.txt_para_start = queued.txt_para != 0;
        out.has_source_anchor = true;
        out.source_anchor = queued.source;
        out.file_size = queued.file_size;
        out.mtime = queued.mtime;
        out.format = queued.format;
        out.parser_version = queued.parser_version;
        return PosLoadStatus::kOk;
    }
    #endif

    return LoadPosFileOnce(pos_path, out);
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
        ESP_LOGW(TAG, "reading clock fold clamp %lld us (max %lld)", static_cast<long long>(delta_us),
                 static_cast<long long>(kMaxFoldDeltaUs));
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
            // 未对时仍累计时长；首页快照保存待归属秒数。
            AddReadingSecondsSaturated(sec);
        }
#ifndef EBOOK_EMULATOR
        BookHomeSnapshotAddDailyReadingSeconds(today, sec);
#endif
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
    BookSessionProgressBlob blob{};
    blob.magic[0] = 'P';
    blob.magic[1] = 'O';
    blob.magic[2] = 'S';
    blob.magic[3] = '4';
    blob.chapter = static_cast<uint32_t>(std::max(0, chapter_index_));
    blob.page = static_cast<uint32_t>(std::max(0, page_index_));
    blob.pct_x10 = static_cast<uint32_t>(std::max(0, ReadingProgressX10()));
    blob.reading_seconds = reading_seconds_;
    blob.day_id = reading_day_id_;
    blob.daily_seconds = daily_seconds_;
    if (info_.format == BookFormat::kTxt) {
        const auto anchor = CaptureLayoutAnchor();
        blob.anchor_magic = 0x31585442;
        blob.txt_anchor = anchor.txt_byte_off;
        blob.txt_para = anchor.txt_at_para_start;
    }

    BookSourcePosition end;
    if (!CaptureBookmarkRange(blob.source, end) && info_.file_size != 0) {
        ESP_LOGW(TAG, "progress snapshot unavailable stage=source_anchor");
        return false;
    }
    blob.file_size = info_.file_size;
    blob.mtime = info_.mtime;
    blob.format = static_cast<uint32_t>(info_.format);
    blob.parser_version = kSourcePositionVersion;
    blob.checksum = ProgressChecksum(blob);
#ifndef EBOOK_EMULATOR
    const bool ok = QueueProgressWrite(path, info_.path, blob);
#else
    const bool ok = WriteFileAtomic(path.c_str(), reinterpret_cast<const uint8_t*>(&blob), sizeof(blob));
#endif
    if (!ok) {
        LogProgressFailure("enqueue");
    }
    return ok;
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

lv_coord_t BookSession::LayoutLineHeight() const {
    return FontLineHeight(font_);
}

TextLineLayout BookSession::LineLayout() const {
    return MakeTextLineLayout(font_, viewport_h_, line_spacing_percent_, vertical_writing_, text_style_.line_gap);
}

lv_coord_t BookSession::LayoutGlyphWidth(uint32_t cp) const {
    if (!vertical_writing_) {
        return TextAdvanceWithGap(GlyphWidth(font_, cp), text_style_.letter_gap);
    }
    const int space_width = IsTxtParaIndentWs(cp) && cp != 0x3000u ? GlyphWidth(font_, cp) : 0;
    return TextAdvanceWithGap(VerticalAdvance(cp, LayoutLineHeight(), space_width), text_style_.letter_gap);
}

lv_coord_t BookSession::LayoutIndentWidth() const {
    return vertical_writing_ ? std::min<lv_coord_t>(text_style_.indent_chars * LayoutLineHeight(),
                                                    std::max<lv_coord_t>(0, viewport_w_ - LayoutLineHeight()))
                             : TxtParaIndentWidth(font_) * text_style_.indent_chars / 2;
}

uint32_t BookSession::FontFingerprint() const {
    // 行高、步进及字形边界共同决定断行和安全字距。
    uint32_t h = static_cast<uint32_t>(LayoutLineHeight());
    h = h * 131u + static_cast<uint32_t>(LineLayout().count);
    static const uint32_t kSamples[] = {
        0x4E2Du,  // 中
        0x6587u,  // 文
        static_cast<uint32_t>('a'),
        static_cast<uint32_t>('0'),
        0xFF0Cu,   // ，
        0x3002u,   // 。
        0x201Cu,   // “
        0x3400u,   // 扩展 A 区：扩展字库可用性参与分页指纹
        0x20000u,  // 扩展 B 区
    };
    for (uint32_t cp : kSamples) {
        h = h * 131u + static_cast<uint32_t>(GlyphWidth(font_, cp));
        lv_font_glyph_dsc_t glyph = {};
        if (glyph_font_ && lv_font_get_glyph_dsc(glyph_font_, &glyph, cp, 0)) {
            h = h * 131u + glyph.box_w;
            h = h * 131u + static_cast<uint32_t>(glyph.ofs_x);
        }
    }
    h = h * 131u + static_cast<uint32_t>(text_style_.line_gap + 1);
    h = h * 131u + static_cast<uint32_t>(text_style_.letter_gap);
    h = h * 131u + static_cast<uint32_t>(text_style_.indent_chars);
    h = h * 131u + static_cast<uint32_t>(text_style_.justify);
    if (!text_style_.inline_images && info_.format != BookFormat::kTxt) {
        for (const char* p = UiIllustration(); *p; ++p) {
            h = h * 131u + static_cast<unsigned char>(*p);
        }
    }
    h = h * 131u + 0x42524B33u;
    h = h * 131u + static_cast<uint32_t>(viewport_w_);
    h = h * 131u + static_cast<uint32_t>(viewport_h_);
    if (vertical_writing_) {
        h = h * 131u + 0x56525434u;
    }
    if (info_.format == BookFormat::kEpub) {
        h = h * 131u + 0x45505433u;
    }
    return h;
}

bool BookSession::TryLoadLegacyTxtIndex(const char* txt_path, size_t file_size) {
    const std::string idx_path = TxtIndexPath(txt_path);
    FILE* ifp = std::fopen(idx_path.c_str(), "rb");
    if (ifp == nullptr) {
        return false;
    }

    BookSessionTxtIdxHeader hdr{};
    if (std::fread(&hdr, 1, sizeof(hdr), ifp) != sizeof(hdr)) {
        std::fclose(ifp);
        return false;
    }
    if (std::memcmp(hdr.magic, "TXI1", 4) != 0 || hdr.version != kTxtIdxVersion) {
        std::fclose(ifp);
        ESP_LOGW(TAG, "txt idx bad magic/ver: ");
        return false;
    }
    if (hdr.file_size != static_cast<uint32_t>(file_size) || hdr.viewport_w != static_cast<uint16_t>(viewport_w_) ||
        hdr.viewport_h != static_cast<uint16_t>(viewport_h_) ||
        hdr.line_spacing_percent != static_cast<uint16_t>(line_spacing_percent_) ||
        hdr.para_gap != static_cast<uint16_t>(para_gap_) || hdr.font_fp != FontFingerprint() ||
        hdr.as_gb18030 != static_cast<uint8_t>(txt_as_gb18030_ ? 1 : 0) || hdr.reserved[0] != 0) {
        std::fclose(ifp);
        ESP_LOGI(TAG, "legacy txt index unavailable reason=layout_mismatch");
        return false;
    }
    // 合理上限，防止坏文件拖垮内存
    if (hdr.line_count == 0 || hdr.page_count == 0 || hdr.line_count > file_size + 1 ||
        hdr.page_count > hdr.line_count) {
        std::fclose(ifp);
        ESP_LOGW(TAG, "txt idx absurd counts L=%u P=%u", static_cast<unsigned>(hdr.line_count),
                 static_cast<unsigned>(hdr.page_count));
        return false;
    }

    auto fail_cleanup = [&]() {
        std::fclose(ifp);
        txt_lines_.Reset();
        txt_page_first_line_.Reset();

        txt_lines_len_ = 0;
        txt_page_count_ = 0;
        ClearToc();
    };

    static_assert(sizeof(TxtLineRef) == sizeof(BookSessionTxtIdxLineDisk), "TXT index record layout mismatch");
    const uint64_t pages_offset =
        sizeof(hdr) + static_cast<uint64_t>(hdr.line_count) * sizeof(BookSessionTxtIdxLineDisk);
    const uint64_t toc_offset = pages_offset + (static_cast<uint64_t>(hdr.page_count) + 1) * sizeof(uint32_t);
    if (toc_offset > static_cast<uint64_t>(LONG_MAX) ||
        !txt_lines_.OpenReadOnly(idx_path, sizeof(hdr), hdr.line_count) ||
        !txt_page_first_line_.OpenReadOnly(idx_path, static_cast<size_t>(pages_offset),
                                         static_cast<size_t>(hdr.page_count) + 1)) {
        fail_cleanup();
        return false;
    }
    txt_lines_len_ = hdr.line_count;

    // 校验哨兵
    if (txt_page_first_line_[0] != 0 || txt_page_first_line_[hdr.page_count] != hdr.line_count ||
        !txt_page_first_line_.Good()) {
        fail_cleanup();
        ESP_LOGW(TAG, "txt idx page table mismatch");
        return false;
    }
    txt_page_count_ = hdr.page_count;

    const bool closed = std::fclose(ifp) == 0;
    if (!closed) {
        txt_lines_.Reset();
        txt_page_first_line_.Reset();
        txt_lines_len_ = txt_page_count_ = 0;
        return false;
    }
    ESP_LOGI(TAG, "legacy txt index ready lines=%u pages=%u", static_cast<unsigned>(txt_lines_len_),
             static_cast<unsigned>(txt_page_count_));
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

bool BookSession::CloseSdHandles() {
    bool ok = txt_lines_.Detach();
    if (!txt_page_first_line_.Detach()) {
        ok = false;
    }
    if (!pages_.Detach()) {
        ok = false;
    }
    if (!pending_pages_.Detach()) {
        ok = false;
    }
    if (txt_fp_ != nullptr) {
        if (std::fclose(txt_fp_) != 0) {
            ok = false;
        }
        txt_fp_ = nullptr;
    }
    if (!ok) {
        error_ = BookReadError::kCacheIo;
    }
    return ok;
}

void BookSession::DetachSdHandles() {
    ClearPageImageCache();
    sd_handles_detached_ = true;
    CloseSdHandles();
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
            ESP_LOGW(TAG, "reattach txt fail ");
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

bool BookSession::PrepareClose() {
    if (close_progress_queued_) {
        return true;
    }
    StopReadingClock();
    if (open_ && !SaveProgress()) {
        return false;
    }
    close_progress_queued_ = true;
    return true;
}

void BookSession::Close() {
    if (open_ && !close_progress_queued_) {
        SaveProgress();  // 折算活跃时钟并写出稳定正文锚点
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
    pending_pages_.clear();

    page_index_ = 0;
    chapter_index_ = 0;
    chapter_count_ = 1;

    chapter_relayout_pending_ = false;
    chapter_preview_pending_ = false;
    chapter_layout_generation_ = 0;
    pending_structure_.reset();
    txt_structure_scan_.reset();
    structure_scan_sizes_.clear();
    structure_scan_toc_content_.clear();
    structure_scan_source_ = 0;
    structure_cache_checked_ = false;
    structure_staging_ = false;
    structure_pending_ = false;
    structure_failed_.store(false);
    restored_progress_x10_ = 0;
    preview_toc_index_ = -1;
    source_preview_anchor_ = {};
    source_preview_next_ = {};
    source_preview_history_.clear();
    reading_chapters_.reset();
    source_sizes_.reset();
    source_chapter_count_ = 0;
    txt_range_end_ = UINT32_MAX;
    cover_.Reset();
    cover_tried_ = false;
    ClearPageImageCache();
    live_preview_active_ = false;
    txt_preview_relayout_pending_ = false;
    live_preview_byte_off_ = 0;
    live_preview_at_para_start_ = true;
    live_preview_next_off_ = 0;
    live_preview_next_at_para_start_ = true;
    live_preview_has_next_ = false;
    live_preview_page_ = Page{};
    live_preview_hist_.clear();
    open_ = false;
    navigation_anchor_ = {};
#ifndef EBOOK_EMULATOR
    {
        std::lock_guard<std::mutex> lock(book_file_mu);
        open_books.erase(std::remove_if(open_books.begin(), open_books.end(),
                                        [this](const auto& item) { return item.first == this; }),
                         open_books.end());
    }
#endif
    info_ = {};
    display_title_.clear();
    display_author_.clear();
    epub_toc_positions_.clear();
}

#ifndef EBOOK_EMULATOR
bool BookSession::DeleteFile(const char* book) {
    if (!book || DetectFormatByPath(book) == BookFormat::kUnknown) {
        errno = EINVAL;
        return false;
    }
    const std::string progress_path = ProgressPath(book);
    if (progress_path.empty()) {
        errno = EINVAL;
        return false;
    }
    std::lock_guard<std::mutex> file_lock(book_file_mu);
    for (const auto& item : open_books) {
        if (item.second == book) {
            errno = EBUSY;
            return false;
        }
    }
    SdAccessTransaction transaction;
    if (!transaction) {
        errno = EBUSY;
        return false;
    }
    struct stat st{};
    if (stat(book, &st) != 0) {
        if (errno != ENOENT) {
            ESP_LOGE(TAG, "book delete inspect failed errno=%d", errno);
        }
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        errno = EINVAL;
        return false;
    }
    // 排队快照尚未计入聚合；删除只扣除正式记录或已交付的可恢复记录。
    BookSessionProgressBlob previous{};
    const bool previous_known = ReadProgressBaseline(progress_path.c_str(), previous);
    if (unlink(book) != 0) {
        ESP_LOGE(TAG, "book delete failed stage=unlink errno=%d", errno);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(progress_write_mu);
        progress_writes.erase(std::remove_if(progress_writes.begin(), progress_writes.end(),
                                             [book](const auto& job) { return job->book == book; }),
                              progress_writes.end());
        if (progress_inflight && progress_inflight->book == book) {
            progress_inflight->cancelled.store(true);
        }
    }
    if (previous_known) {
        BookHomeSnapshotSubtractAggregateContribution(previous.reading_seconds, previous.pct_x10 >= 1000);
    }
    BookProgressCacheErase(book);
    BookHomeSnapshotRemoveBook(book);
    InvalidateBookLibraryCache();
    bool clean = false;
    int cleanup_error = EIO;
    try {
        clean = SdDataDeleteBookData(book);
    } catch (const std::bad_alloc&) {
        cleanup_error = ENOMEM;
        ESP_LOGE(TAG, "book delete failed stage=auxiliary_cleanup result=no_memory; book removed");
    }
    ESP_LOGD(TAG, "book delete done auxiliary_ok=%d", clean);
    if (!clean) {
        errno = cleanup_error;
    }
    return clean;
}
#endif

bool BookSession::Open(const BookInfo& info) {
    error_ = BookReadError::kInvalidData;
    try {
        if (OpenImpl(info)) {
            error_ = BookReadError::kNone;
            return true;
        }
    } catch (const std::bad_alloc&) {
        error_ = BookReadError::kNoMemory;
        Close();
    }
    return false;
}

bool BookSession::OpenImpl(const BookInfo& info) {
    Close();
    close_progress_queued_ = true;
    last_progress_pct_ = -1;
    ReportOpenProgress(0);
    info_ = info;
    struct stat source_stat{};
    bool have_source_stat = false;
#ifndef EBOOK_EMULATOR
    {
        std::lock_guard<std::mutex> lock(book_file_mu);
        if (stat(info.path.c_str(), &source_stat) != 0 || !S_ISREG(source_stat.st_mode)) {
            error_ = BookReadError::kFileIo;
            ESP_LOGE(TAG, "book open inspect failed errno=%d", errno);
            return false;
        }
        have_source_stat = true;
        open_books.emplace_back(this, info.path);
    }
#else
    have_source_stat = stat(info.path.c_str(), &source_stat) == 0;
#endif
    if (have_source_stat) {
        info_.file_size = source_stat.st_size;
        info_.mtime = source_stat.st_mtime;
    }
    PosFileData position;
    const auto status = LoadPosFile(ProgressPath(info_.path.c_str()).c_str(), position);
    if (status == PosLoadStatus::kInvalid) {
        error_ = BookReadError::kInvalidData;
        ESP_LOGE(TAG, "progress restore failed stage=record_validation");
        Close();
        return false;
    }
    const bool has_position = status == PosLoadStatus::kOk;
    if (position.has_source_anchor &&
        (position.file_size != info_.file_size || position.mtime != info_.mtime ||
         position.format != static_cast<uint32_t>(info_.format) ||
         position.parser_version != kSourcePositionVersion)) {
        error_ = BookReadError::kInvalidData;
        ESP_LOGW(TAG, "progress restore unavailable stage=source_identity");
        Close();
        return false;
    }
    display_title_ = info.title;
    bool ok = false;
    if (info.format == BookFormat::kTxt) {
        ok = OpenTxt(info.path.c_str(), info_.file_size, position);
    } else if (info.format == BookFormat::kEpub) {
        ok = OpenEpub(info.path.c_str());
    } else if (info.format == BookFormat::kEbook) {
        ok = OpenEbook(info.path.c_str());
    } else {
        error_ = BookReadError::kUnsupported;
        ESP_LOGE(TAG, "book open failed stage=format");
    }
    if (!ok) {
        Close();
        return false;
    }
    source_chapter_count_ = chapter_count_;
    open_ = true;
    structure_pending_ = !txt_empty_;
    restored_progress_x10_ = position.has_pct ? std::min<uint32_t>(1000, position.pct_x10) : 0;
    reading_seconds_ = position.has_seconds ? position.reading_seconds : 0;
    reading_day_id_ = position.has_daily ? position.day_id : 0;
    daily_seconds_ = position.has_daily ? position.daily_seconds : 0;
    EnsureDailyBucket();
    reading_clock_active_ = false;
    reading_clock_since_us_ = 0;
    if (!txt_mode_) {
        LayoutAnchor anchor;
        anchor.valid = true;
        anchor.source_para_start = true;
        if (has_position && position.has_source_anchor) {
            anchor.source_chapter = position.source_anchor.chapter;
            anchor.source_block = position.source_anchor.block;
            anchor.source_offset = position.source_anchor.offset;
            anchor.source_para_start = position.source_anchor.offset == 0;
        } else if (has_position) {
            // 旧逻辑兼容：无正文锚点的旧记录只排到目标页，转换完成即停止，不扫描其他章节。
            const int source = std::clamp<int>(position.chapter, 0, std::max(0, source_chapter_count_ - 1));
            BookPageStore legacy;
            if (!legacy.Open(info_.path) ||
                !PaginateSourceChapter(source, legacy, chapter_layout_generation_.load(), nullptr, nullptr,
                                       nullptr, nullptr, static_cast<size_t>(position.page) + 1) ||
                !legacy.Finish() || legacy.empty()) {
                error_ = legacy.Good() ? Error() : legacy.Error();
                Close();
                return false;
            }
            const auto& page = legacy[std::min<size_t>(position.page, legacy.size() - 1)];
            for (const auto& item : page.items) {
                if (item.source_block != UINT32_MAX) {
                    anchor.source_chapter = item.source_chapter;
                    anchor.source_block = item.source_block;
                    anchor.source_offset = item.source_begin;
                    anchor.source_para_start = item.para_indent || item.chapter_title;
                    break;
                }
            }
            if (!legacy.Good()) {
                error_ = legacy.Error();
                Close();
                return false;
            }
        } else {
            anchor.source_chapter = ebook_ ? ebook_spine_.front() : 0;
            anchor.source_block = 0;
        }
        if (!PrepareSourcePreview(anchor)) {
            ESP_LOGE(TAG, "book open failed stage=source_preview error=%d", static_cast<int>(Error()));
            Close();
            return false;
        }
    }
    struct stat completed_stat{};
    if (stat(info.path.c_str(), &completed_stat) != 0 ||
        (have_source_stat && (source_stat.st_size != completed_stat.st_size ||
                             source_stat.st_mtime != completed_stat.st_mtime))) {
        error_ = BookReadError::kFileIo;
        ESP_LOGE(TAG, "book open validation failed stage=source_identity errno=%d", errno);
        Close();
        return false;
    }
    bookmarks_.Load(info.path, info_.file_size, info_.mtime, static_cast<uint32_t>(info.format));
    close_progress_queued_ = false;
    ReportOpenProgress(100);
    return true;
}

int BookSession::PageCount() const {
    return txt_mode_ ? 1 : static_cast<int>(pages_.size());
}

int BookSession::CurrentPage() const {
    return txt_mode_ ? 0 : page_index_;
}

bool BookSession::OpenTxt(const char* path, size_t known_size, const PosFileData& pos) {
    if (path == nullptr) {
        ESP_LOGE(TAG, "txt open: path null");
        return false;
    }
    ESP_LOGI(TAG, "txt open begin int=%u spiram=%u known_size=%u ",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)), static_cast<unsigned>(known_size));
    ReportOpenProgress(5);

    // FatFS 上部分大文件/长文件名 fseek(END) 会 EIO；优先 stat / 书库已扫到的大小
    size_t sz = 0;
    const char* size_src = "none";
    struct stat st{};
    if (stat(path, &st) == 0 && st.st_size >= 0) {
        sz = static_cast<size_t>(st.st_size);
        size_src = "stat";
    } else if (known_size > 0) {
        sz = known_size;
        size_src = "bookinfo";
        ESP_LOGW(TAG, "txt stat fail errno=%d, use known_size=%u", errno, static_cast<unsigned>(known_size));
    }

    FILE* fp = std::fopen(path, "rb");
    if (fp == nullptr) {
        error_ = BookReadError::kFileIo;
        ESP_LOGE(TAG, "txt fopen fail errno=%d int=%u spiram=%u ", errno,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        return false;
    }

    if (sz == 0) {
        if (std::fseek(fp, 0, SEEK_END) != 0) {
            ESP_LOGE(TAG, "txt size unknown: fseek END fail errno=%d ", errno);
            std::fclose(fp);
            return false;
        }
        const long sz_l = std::ftell(fp);
        if (sz_l < 0) {
            ESP_LOGE(TAG, "txt size unknown: ftell fail errno=%d ", errno);
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
        txt_fp_ = fp;
        txt_empty_ = true;
        return FillTxtLivePreview(0, true);
    }
    if (sz > kMaxTxtBytes) {
        error_ = BookReadError::kUnsupported;
        ESP_LOGE(TAG, "txt too large: %u > %u ()", static_cast<unsigned>(sz), static_cast<unsigned>(kMaxTxtBytes));
        std::fclose(fp);
        return false;
    }

    // fopen 后已在文件头；勿再 SEEK_END（已证实部分 TXT 会 EIO）
    uint8_t peek[4096];
    const size_t npeek = std::fread(peek, 1, sizeof(peek), fp);
    txt_as_gb18030_ = !LooksLikeUtf8(peek, npeek);

    ESP_LOGI(TAG, "txt encoding=%s bytes=%u", txt_as_gb18030_ ? "GB18030" : "UTF-8", static_cast<unsigned>(sz));

    info_.file_size = sz;
    txt_fp_ = fp;
    uint32_t off = 0;
    bool para = true;
    if (IsProgressMagic(pos.magic)) {
        if (pos.has_txt_anchor && pos.txt_anchor < sz) {
            off = pos.txt_anchor;
            para = pos.txt_para_start;
        } else if (!pos.has_source_anchor && TryLoadLegacyTxtIndex(path, sz)) {
            // 旧逻辑兼容：仅借用匹配旧排版的全书索引将页码转换为原始字节，之后不再生成全书页表。
            const size_t page = std::min<size_t>(pos.page, txt_page_count_ - 1);
            const uint32_t line_index = txt_page_first_line_[page];
            if (!txt_page_first_line_.Good() || line_index >= txt_lines_len_) {
                error_ = BookReadError::kInvalidData;
                ESP_LOGE(TAG, "legacy progress conversion failed stage=page_index");
                return false;
            }
            const auto line = txt_lines_[line_index];
            if (!txt_lines_.Good() || line.off >= sz) {
                error_ = BookReadError::kInvalidData;
                ESP_LOGE(TAG, "legacy progress conversion failed stage=line_index");
                return false;
            }
            off = line.off;
            para = (line.len & kTxtLineParaStart) != 0;
            txt_lines_.Reset();
            txt_page_first_line_.Reset();
            txt_lines_len_ = txt_page_count_ = 0;
            toc_.clear();
        } else if (pos.has_pct && pos.pct_x10 > 0 && pos.pct_x10 <= 1000) {
            // 旧逻辑兼容：无字节锚点且旧索引不可用时，只能按百分比近似定位到前一个段落。
            ESP_LOGW(TAG, "legacy progress restored approximately reason=source_anchor_unavailable");
            const uint32_t target = static_cast<uint64_t>(sz - 1) * pos.pct_x10 / 1000;
            const uint32_t begin = target > 4096 ? target - 4096 : 0;
            if (std::fseek(fp, begin, SEEK_SET) == 0) {
                uint8_t bytes[4096];
                const size_t got = std::fread(bytes, 1, target - begin, fp);
                for (size_t i = 0; i < got; ++i) {
                    if (bytes[i] == '\n') {
                        off = begin + i + 1;
                    }
                }
            }
        }
    }
    if (!FillTxtLivePreview(off, para)) {
        return false;
    }
    ReportOpenProgress(90);
    return true;
}

bool BookSession::OpenEpub(const char* path) {
    ReportOpenProgress(10);
    epub_ = std::make_unique<EpubDocument>();
    if (!epub_->Open(path)) {
        error_ = epub_->Error();
        epub_.reset();
        return false;
    }
    ReportOpenProgress(40);
    if (!epub_->Title().empty()) {
        display_title_ = epub_->Title();
    }
    display_author_ = epub_->Author();
    page_images_mode_ = epub_->DetectPageImages();
    if (page_images_mode_) {
        SetVerticalWriting(false);
        SetLineSpacing(0, 0);
    }
    chapter_count_ = std::max(1, epub_->SpineCount());
    chapter_index_ = 0;
    ClearToc();
    epub_toc_positions_.clear();
    for (const auto& entry : epub_->Toc()) {
        TocEntry item;
        item.title = entry.title;
        item.depth = entry.depth;
        toc_.push_back(std::move(item));
        epub_toc_positions_.push_back({static_cast<uint32_t>(entry.spine_index), UINT32_MAX, 0});
    }
    if (toc_.empty() && page_images_mode_) {
        for (int i = 0; i < chapter_count_; ++i) {
            TocEntry item;
            char title[48];
            std::snprintf(title, sizeof(title), UiPageFmt(), i + 1);
            item.title = title;
            toc_.push_back(std::move(item));
        }
    }
    return true;
}

bool BookSession::OpenEbook(const char* path) {
    ReportOpenProgress(10);
    ebook_ = std::make_unique<EbookDocument>();
    if (!ebook_->Open(path)) {
        error_ = ebook_->Error();
        ebook_.reset();
        return false;
    }
    ReportOpenProgress(40);
    if (!ebook_->Title().empty()) {
        display_title_ = ebook_->Title();
    }
    display_author_ = ebook_->Author();
    page_images_mode_ = ebook_->HasPageImages();
    if (page_images_mode_) {
        SetVerticalWriting(false);
        SetLineSpacing(0, 0);
    }
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

uint32_t BookSession::StructureFingerprint() const {
    uint32_t hash = kSourcePositionVersion;
    if (info_.format != BookFormat::kTxt) {
        // 旧逻辑兼容：生成标题参与源块映射，缓存须匹配当前标题格式，但不依赖字体或视口。
        for (const char* text = UiChapterFmt(); *text; ++text) {
            hash = (hash ^ static_cast<uint8_t>(*text)) * 16777619u;
        }
    }
    return hash;
}

bool BookSession::PrepareSourcePreview(const LayoutAnchor& requested) {
    if (navigation_loading_chapter_ && requested.source_chapter != source_preview_anchor_.source_chapter) {
        navigation_loading_chapter_->store(true);
    }
    const auto* owner = navigation_source_ ? navigation_source_ : this;
    LayoutAnchor anchor = requested;
    int source = static_cast<int>(anchor.source_chapter);
    if (info_.format == BookFormat::kEbook) {
        const auto found = std::find(ebook_spine_.begin(), ebook_spine_.end(), source);
        if (found == ebook_spine_.end()) {
            error_ = BookReadError::kInvalidData;
            return false;
        }
        source = static_cast<int>(found - ebook_spine_.begin());
    }
    std::lock_guard<std::mutex> lock(owner->chapter_io_mu_);
    for (; source < source_chapter_count_; ++source) {
        BookPageStore preview;
        bool more = false;
        if (!preview.Open(info_.path) ||
            !PaginateSourceChapter(source, preview, owner->chapter_layout_generation_.load(), &anchor,
                                   navigation_cancelled_, nullptr, &more) || !preview.Finish()) {
            if (!preview.Good()) {
                error_ = preview.Error();
            }
            return false;
        }
        if (!preview.empty()) {
            Page page = preview[0];
            if (!preview.Good()) {
                error_ = preview.Error();
                return false;
            }
            bool located = false;
            LayoutAnchor next;
            for (const auto& item : page.items) {
                if (item.source_block == UINT32_MAX) {
                    continue;
                }
                if (!located) {
                    anchor.source_chapter = item.source_chapter;
                    anchor.source_block = item.source_block;
                    anchor.source_offset = item.source_begin;
                    anchor.source_para_start = item.para_indent || item.chapter_title;
                    located = true;
                }
                next.source_chapter = item.source_chapter;
                next.source_block = item.source_block;
                next.source_offset = item.source_end;
            }
            if (located) {
                anchor.valid = next.valid = true;
                anchor.chapter = source;
                anchor.page = 0;
                anchor.page_count = 1;
                if (!more && source + 1 < source_chapter_count_) {
                    next.source_chapter = info_.format == BookFormat::kEbook ? ebook_spine_[source + 1] : source + 1;
                    next.source_block = next.source_offset = 0;
                    next.source_para_start = true;
                } else {
                    next.source_para_start = false;
                }
                source_preview_anchor_ = anchor;
                source_preview_next_ = next;
                live_preview_page_ = std::move(page);
                live_preview_active_ = true;
                live_preview_has_next_ = more || source + 1 < source_chapter_count_;
                chapter_index_ = source;
                page_index_ = 0;
                navigation_anchor_ = {};
                error_ = BookReadError::kNone;
                return true;
            }
        }
        anchor.source_chapter = info_.format == BookFormat::kEbook && source + 1 < source_chapter_count_
                                    ? ebook_spine_[source + 1] : source + 1;
        anchor.source_block = anchor.source_offset = 0;
        anchor.source_para_start = true;
    }
    return false;
}

bool BookSession::ResolveTocPreview(int index) {
    if (navigation_loading_chapter_) {
        navigation_loading_chapter_->store(true);
    }
    const auto* owner = navigation_source_ ? navigation_source_ : this;
    if (index < 0 || index >= TocCount()) {
        return false;
    }
    LayoutAnchor target;
    target.valid = true;
    target.source_block = 0;
    target.source_para_start = true;
    if (info_.format == BookFormat::kEbook) {
        target.source_chapter = ebook_spine_[index];
    } else if (owner->epub_) {
        if (owner->epub_->Toc().empty()) {
            target.source_chapter = index;
        } else {
            const auto& entry = owner->epub_->Toc()[index];
            target.source_chapter = entry.spine_index;
            if (!entry.fragment.empty()) {
                std::lock_guard<std::mutex> lock(owner->chapter_io_mu_);
                bool first = true, inserted = false, removed = false, found = false;
                std::string raw, displayed;
                const auto anchors = [&](const std::string& name, uint32_t block, uint32_t offset) {
                    if (!found && name == entry.fragment) {
                        target.source_block = block;
                        target.source_offset = offset;
                        found = true;
                    }
                };
                const auto sink = [&](ContentBlock&& block) {
                    if (first && !page_images_mode_) {
                        first = false;
                        raw = block.text;
                        std::vector<ContentBlock> initial;
                        initial.push_back(std::move(block));
                        EnsureChapterTitleInBlocks(FormatChapterOrdinalTitle(entry.spine_index + 1),
                                                   entry.spine_index + 1, initial, &inserted);
                        removed = initial.size() == (inserted ? 1u : 0u);
                        if (!removed) {
                            displayed = initial[inserted ? 1 : 0].text;
                        }
                    }
                    return !found && !(navigation_cancelled_ && navigation_cancelled_->load()) &&
                           !owner->background_paused_.load();
                };
                const bool ok = owner->epub_->StreamChapterBlocks(entry.spine_index, sink, anchors, navigation_cancelled_);
                if ((navigation_cancelled_ && navigation_cancelled_->load()) || owner->background_paused_.load()) {
                    error_ = BookReadError::kCancelled;
                    return false;
                }
                if (!ok && !found) {
                    error_ = owner->epub_->Error();
                    return false;
                }
                if (!found) {
                    ESP_LOGW(TAG, "toc preview anchor missing index=%d; using source start", index);
                }
                // 旧逻辑兼容：临时目录导航沿用 BKM1 的标题块编号和首段偏移映射。
                if (!page_images_mode_) {
                    if (target.source_block > 0) {
                        target.source_block += (inserted ? 1 : 0) - (removed ? 1 : 0);
                    } else {
                        target.source_block = inserted ? 1 : 0;
                        target.source_offset = removed ? 0 : EpubFirstSourceOffset(raw, displayed, target.source_offset);
                    }
                }
            }
        }
    } else {
        return false;
    }
    if (!PrepareSourcePreview(target)) {
        return false;
    }
    preview_toc_index_ = index;
    source_preview_history_.clear();
    return true;
}

bool BookSession::StructureScanInterrupted(uint32_t generation) const {
    const auto* owner = navigation_source_ ? navigation_source_ : this;
    if (owner->background_paused_.load() || generation != owner->chapter_layout_generation_.load()) {
        error_ = BookReadError::kCancelled;
        return true;
    }
    return false;
}

struct BookSession::TxtStructureScan {
    BookSessionTxtByteStream stream; // 文件关闭后保留缓冲及精确扫描位置
    TxtChapterDetector detector; // 跨行标题识别状态
    std::vector<uint32_t> title_ends; // 标题结束处的非空白字符累计数
    uint32_t characters = 0, glyph_count = 0; // 内容计数及让出时间片的步数
    bool paragraph = true, indented = false, update_title_end = false, limit_reported = false; // 当前段落及目录状态
    bool complete = false; // 已到文件末尾并完成目录过滤

    ~TxtStructureScan() {
        if (stream.fp) {
            std::fclose(stream.fp);
        }
    }
};

bool BookSession::ScanTxtChapters(uint32_t generation) try {
    const bool first_pass = !txt_structure_scan_;
    if (first_pass) {
        auto scan = std::make_shared<TxtStructureScan>();
        if (!scan->stream.InitBuf()) {
            error_ = BookReadError::kNoMemory;
            return false;
        }
        scan->stream.as_gb18030 = txt_as_gb18030_;
        txt_structure_scan_ = std::move(scan);
    }
    auto& scan = *txt_structure_scan_;
    if (scan.complete) {
        return true;
    }
    std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(info_.path.c_str(), "rb"), &std::fclose);
    if (!file || std::fseek(file.get(), scan.stream.file_base + scan.stream.end, SEEK_SET) != 0) {
        error_ = BookReadError::kFileIo;
        return false;
    }
    auto& stream = scan.stream;
    stream.fp = file.get();
    struct ClearScanFile {
        BookSessionTxtByteStream& stream; // 文件由局部句柄持有，扫描状态只借用
        ~ClearScanFile() { stream.fp = nullptr; }
    } clear_file{stream};
    if (first_pass) {
        stream.SkipBomIfAny();
    }
    auto& detector = scan.detector;
    auto& title_ends = scan.title_ends;
    auto& characters = scan.characters;
    auto& paragraph = scan.paragraph;
    auto& indented = scan.indented;
    auto& toc_limit_reported = scan.limit_reported;
    auto& update_title_end = scan.update_title_end;
    auto& glyph_count = scan.glyph_count;
    uint32_t cp = 0, off = 0;
    uint16_t len = 0;
    bool newline = false;
    for (;;) {
        if (StructureScanInterrupted(generation)) {
            return false;
        }
        if (!stream.NextGlyph(&cp, &off, &len, &newline)) {
            break;
        }
        if ((++glyph_count & 0x7FFFu) == 0) {
#ifndef EBOOK_EMULATOR
            vTaskDelay(1);
#endif
        }
        if (newline) {
            if (update_title_end) {
                title_ends.back() = characters;
                update_title_end = false;
            }
            if (paragraph) {
                detector.BlankLine();
            }
            paragraph = true;
            indented = false;
            continue;
        }
        if (paragraph && IsTxtParaIndentWs(cp)) {
            indented = true;
            continue;
        }
        if (paragraph) {
            paragraph = false;
            std::string title;
            bool complete = false;
            if (detector.MayStart(cp) && !ProbeTxtParagraph(stream, off, title, complete)) {
                error_ = BookReadError::kFileIo;
                return false;
            }
            const auto kind = detector.Classify(title, indented, off, complete);
            if (kind == TxtHeadingKind::kChapter && toc_.size() >= kMaxTxtTocEntries && !toc_limit_reported) {
                ESP_LOGW(TAG, "txt contents limit reached entries=%u; remaining text stays in final chapter",
                         static_cast<unsigned>(kMaxTxtTocEntries));
                toc_limit_reported = true;
            }
            if (kind == TxtHeadingKind::kChapter && toc_.size() < kMaxTxtTocEntries) {
#ifndef EBOOK_EMULATOR
                const size_t metadata_bytes = sizeof(TocEntry) + kMaxTxtChapterTitleBytes + sizeof(uint32_t);
                if (!FirmwarePsramCanAllocate(metadata_bytes * (toc_.size() + 1))) {
                    error_ = BookReadError::kNoMemory;
                    ESP_LOGE(TAG, "chapter catalog scan failed stage=memory_budget");
                    return false;
                }
#endif
                TocEntry entry;
                entry.title = std::move(title);
                entry.first_byte_off = off;
                toc_.push_back(std::move(entry));
                title_ends.push_back(characters);
                update_title_end = true;
            } else if (kind == TxtHeadingKind::kSubtitle && !toc_.empty() &&
                       toc_.back().title.size() + title.size() + 1 <= kMaxTxtChapterTitleBytes) {
                toc_.back().title += " " + title;
                update_title_end = true;
            }
        }
        if (!IsTxtParaIndentWs(cp)) {
            ++characters;
        }
    }
    if (std::ferror(file.get())) {
        error_ = BookReadError::kFileIo;
        return false;
    }
    if (update_title_end) {
        title_ends.back() = characters;
    }
    FilterDenseTxtToc(toc_, title_ends, characters);
    scan.complete = true;
    return true;
} catch (const std::bad_alloc&) {
    txt_structure_scan_.reset();
    toc_.clear();
    error_ = BookReadError::kNoMemory;
    return false;
}

int BookSession::ReadingChapterForPosition(const BookSourcePosition& position) const {
    if (!reading_chapters_ || reading_chapters_->empty()) {
        return -1;
    }
    const auto& first = reading_chapters_->front().first;
    // 旧逻辑兼容：TXT 的 BOM 前字节和 EPUB 生成标题所在的零号块归入首个正文章节。
    if (position.chapter == first.chapter && position.block == 0 && SourceBefore(position, first) &&
        ((info_.format == BookFormat::kTxt && position.offset <= 3) ||
         (info_.format == BookFormat::kEpub && first.block == 1 && position.offset < 64))) {
        return 0;
    }
    for (size_t i = 0; i < reading_chapters_->size(); ++i) {
        const auto& chapter = (*reading_chapters_)[i];
        if (!SourceBefore(position, chapter.first) && SourceBefore(position, chapter.end)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool BookSession::BuildReadingChapters(uint32_t generation) {
    const auto* owner = navigation_source_ ? navigation_source_ : this;
    auto* epub = owner->epub_.get();
    auto* ebook = owner->ebook_.get();
    const size_t chapter_capacity = std::max(toc_.size() + 1, static_cast<size_t>(chapter_count_));
#ifndef EBOOK_EMULATOR
    const size_t metadata_bytes = chapter_capacity * (sizeof(ReadingChapter) + sizeof(BookSourcePosition) +
                                  sizeof(int) + sizeof(uint64_t)) + (static_cast<size_t>(chapter_count_) + 1) * sizeof(uint64_t);
    if (!FirmwarePsramCanAllocate(metadata_bytes)) {
        error_ = BookReadError::kNoMemory;
        ESP_LOGE(TAG, "chapter catalog build failed stage=memory_budget entries=%u", static_cast<unsigned>(chapter_capacity));
        return false;
    }
#endif
    auto chapters = std::make_shared<std::vector<ReadingChapter>>();
    source_chapter_count_ = chapter_count_;
    chapters->reserve(chapter_capacity);
    if (structure_scan_sizes_.empty()) {
        std::vector<uint64_t> initial_sizes;
        initial_sizes.reserve(static_cast<size_t>(source_chapter_count_) + 1);
        initial_sizes.push_back(0);
        std::vector<uint64_t> initial_positions(toc_.size(), 0);
        structure_scan_sizes_ = std::move(initial_sizes);
        structure_scan_toc_content_ = std::move(initial_positions);
    }
    auto* sizes = &structure_scan_sizes_;
    auto& toc_content = structure_scan_toc_content_;
    BookSourcePosition first_source{};
    if (info_.format == BookFormat::kTxt) {
        if (sizes->size() == 1) {
            sizes->push_back(info_.file_size);
        }
        if (toc_.empty()) {
            source_sizes_ = std::make_shared<const std::vector<uint64_t>>(std::move(*sizes));
            return true;
        }
        if (toc_.front().first_byte_off > 3) {
            chapters->push_back({{0, 0, 0}, {0, 0, toc_.front().first_byte_off}, 0, toc_.front().first_byte_off, -1});
        }
        for (size_t i = 0; i < toc_.size(); ++i) {
            const uint32_t end = i + 1 < toc_.size() ? toc_[i + 1].first_byte_off : info_.file_size;
            chapters->push_back({{0, 0, toc_[i].first_byte_off}, {0, 0, end}, toc_[i].first_byte_off, end,
                                 static_cast<int>(i)});
        }
        txt_mode_ = false;
    } else {
        // 只扫描正文结构和目录锚点，不查询字形、不生成未读章节页码。
        for (int chapter = structure_scan_source_; chapter < source_chapter_count_; ++chapter) {
            bool first = true, inserted = false, removed = false;
            std::string raw_first, displayed_first;
            uint64_t bytes = 0;
            uint32_t raw_block = UINT32_MAX, block_offset = 0;
            HtmlAnchorSink anchors;
            if (StructureScanInterrupted(generation)) {
                return false;
            }
            if (epub) {
                for (size_t i = 0; i < epub->Toc().size(); ++i) {
                    if (epub->Toc()[i].spine_index == chapter) {
                        epub_toc_positions_[i] = {static_cast<uint32_t>(chapter), UINT32_MAX, 0};
                    }
                }
                anchors = [&](const std::string& name, uint32_t block, uint32_t offset) {
                    for (size_t i = 0; i < epub->Toc().size(); ++i) {
                        const auto& entry = epub->Toc()[i];
                        if (entry.spine_index == chapter && entry.fragment == name &&
                            epub_toc_positions_[i].block == UINT32_MAX) {
                            epub_toc_positions_[i] = {static_cast<uint32_t>(chapter), block, offset};
                        }
                    }
                };
                for (size_t i = 0; i < epub->Toc().size(); ++i) {
                    const auto& entry = epub->Toc()[i];
                    if (entry.spine_index == chapter && entry.fragment.empty()) {
                        epub_toc_positions_[i] = {static_cast<uint32_t>(chapter), 0, 0};
                    }
                }
            }
            const auto count = [&](ContentBlock&& block) {
                if (!block.continuation) {
                    ++raw_block;
                    block_offset = 0;
                }
                const uint32_t length = block.kind == ContentKind::kImage ? 1 : block.text.size();
                if (epub) {
                    for (size_t i = 0; i < epub_toc_positions_.size(); ++i) {
                        const auto& position = epub_toc_positions_[i];
                        if (position.chapter != static_cast<uint32_t>(chapter) || position.block != raw_block) {
                            continue;
                        }
                        const uint32_t adjusted = raw_block == 0 && !page_images_mode_ ?
                            EpubFirstSourceOffset(raw_first, displayed_first, position.offset) : position.offset;
                        if (adjusted >= block_offset && adjusted <= block_offset + length) {
                            toc_content[i] = sizes->back() + bytes + adjusted - block_offset;
                        }
                    }
                }
                bytes += length;
                block_offset += length;
                return !StructureScanInterrupted(generation);
            };
            const auto sink = [&](ContentBlock&& block) {
                if (!first || page_images_mode_) {
                    return count(std::move(block));
                }
                first = false;
                raw_first = block.text;
                std::vector<ContentBlock> initial;
                initial.push_back(std::move(block));
                EnsureChapterTitleInBlocks(ebook ? ebook->ChapterTitle(ebook_spine_[chapter]) :
                                           FormatChapterOrdinalTitle(chapter + 1), chapter + 1, initial, &inserted);
                removed = initial.size() == (inserted ? 1u : 0u);
                if (!removed) {
                    displayed_first = initial[inserted ? 1 : 0].text;
                }
                for (size_t i = 0; i < initial.size(); ++i) {
                    if (epub && inserted && i == 0) {
                        continue;
                    }
                    if (!count(std::move(initial[i]))) {
                        return false;
                    }
                }
                return true;
            };
            const bool ok = epub ? epub->StreamChapterBlocks(chapter, sink, anchors) :
                                   ebook->StreamChapterBlocks(ebook_spine_[chapter], sink);
            if (!ok) {
                error_ = StructureScanInterrupted(generation) ? BookReadError::kCancelled :
                         (epub ? epub->Error() : ebook->Error());
                if (Error() != BookReadError::kCancelled) {
                    ESP_LOGE(TAG, "chapter catalog scan failed source=%d error=%d", chapter, static_cast<int>(Error()));
                }
                return false;
            }
            if (chapter == 0 && epub && inserted && !page_images_mode_) {
                source_preview_anchor_.source_block = 1;
            }
            if (epub && !page_images_mode_) {
                size_t missing = 0;
                for (size_t i = 0; i < epub->Toc().size(); ++i) {
                    if (epub->Toc()[i].spine_index != chapter) {
                        continue;
                    }
                    auto& position = epub_toc_positions_[i];
                    if (position.block == UINT32_MAX) {
                        ++missing;
                        position.block = 0;
                        position.offset = 0;
                        toc_content[i] = (*sizes)[chapter];
                    }
                    // 旧逻辑兼容：保留生成标题参与的块编号及首段偏移，既有 BKM1 书签无需重写。
                    if (position.block > 0) {
                        position.block += (inserted ? 1 : 0) - (removed ? 1 : 0);
                    } else {
                        position.block = inserted ? 1 : 0;
                        position.offset = removed ? 0 : EpubFirstSourceOffset(raw_first, displayed_first, position.offset);
                    }
                }
                if (missing) {
                    ESP_LOGW(TAG, "chapter catalog anchors missing source=%d count=%u; using source start", chapter,
                             static_cast<unsigned>(missing));
                }
            }
            sizes->push_back(sizes->back() + bytes);
            structure_scan_source_ = chapter + 1;
        }
        first_source.block = source_preview_anchor_.source_block == UINT32_MAX ? 0 : source_preview_anchor_.source_block;
        const uint32_t source_end = info_.format == BookFormat::kEbook ?
            static_cast<uint32_t>(ebook->ChapterCount()) : static_cast<uint32_t>(source_chapter_count_);
        if (epub && !epub->Toc().empty() && !page_images_mode_) {
            std::vector<std::pair<BookSourcePosition, int>> starts;
            starts.reserve(epub_toc_positions_.size() + 1);
            starts.push_back({first_source, -1});
            for (size_t i = 0; i < epub_toc_positions_.size(); ++i) {
                starts.push_back({epub_toc_positions_[i], static_cast<int>(i)});
            }
            std::sort(starts.begin(), starts.end(), [](const auto& a, const auto& b) {
                if (SourceBefore(a.first, b.first)) {
                    return true;
                }
                if (SourceBefore(b.first, a.first)) {
                    return false;
                }
                return a.second < b.second;
            });
            for (const auto& start : starts) {
                if (!chapters->empty() && !SourceBefore(chapters->back().first, start.first)) {
                    chapters->back().toc_index = start.second;
                    chapters->back().content_begin = start.second >= 0 ? toc_content[start.second] : 0;
                    continue;
                }
                if (!chapters->empty()) {
                    chapters->back().end = start.first;
                    chapters->back().content_end = start.second >= 0 ? toc_content[start.second] : 0;
                }
                chapters->push_back({start.first, {source_end, 0, 0},
                                     start.second >= 0 ? toc_content[start.second] : 0, sizes->back(), start.second});
            }
        } else {
            for (int i = 0; i < source_chapter_count_; ++i) {
                const uint32_t raw = ebook ? ebook_spine_[i] : i;
                const uint32_t end = i + 1 < source_chapter_count_ ? (ebook ? ebook_spine_[i + 1] : i + 1) : source_end;
                int toc_index = i < static_cast<int>(toc_.size()) ? i : -1;
                if (epub && !epub->Toc().empty()) {
                    toc_index = -1;
                    for (size_t entry = 0; entry < epub_toc_positions_.size(); ++entry) {
                        if (epub_toc_positions_[entry].chapter <= raw) {
                            toc_index = static_cast<int>(entry);
                        }
                    }
                }
                chapters->push_back({{raw, 0, 0}, {end, 0, 0}, (*sizes)[i], (*sizes)[i + 1], toc_index});
            }
        }
    }
    chapters->erase(std::remove_if(chapters->begin(), chapters->end(), [](const ReadingChapter& chapter) {
        return chapter.content_begin >= chapter.content_end;
    }), chapters->end());
    auto completed_sizes = std::make_shared<const std::vector<uint64_t>>(std::move(*sizes));
    chapter_count_ = static_cast<int>(chapters->size());
    reading_chapters_ = std::move(chapters);
    source_sizes_ = std::move(completed_sizes);
    return chapter_count_ > 0;
}

bool BookSession::PaginateChapter(int chapter, BookPageStore& built, uint32_t generation, const LayoutAnchor* preview,
                                  const std::atomic<bool>* cancelled) const {
    if (!reading_chapters_ || chapter < 0 || chapter >= static_cast<int>(reading_chapters_->size())) {
        return false;
    }
    const auto* owner = navigation_source_ ? navigation_source_ : this;
    auto* ebook = owner->ebook_.get();
    const auto& range = (*reading_chapters_)[chapter];
    if (info_.format == BookFormat::kTxt) {
        BookSession reader;
        reader.info_ = info_;
        reader.SetFont(font_, glyph_font_);
        reader.text_style_ = text_style_;
        reader.SetVerticalWriting(vertical_writing_);
        reader.SetViewport(physical_w_, physical_h_);
        reader.SetLineSpacing(line_spacing_percent_, para_gap_);
        reader.txt_mode_ = true;
        reader.txt_as_gb18030_ = txt_as_gb18030_;
        reader.txt_range_end_ = range.end.offset;
        reader.navigation_source_ = this;
        uint32_t offset = preview ? preview->txt_byte_off : range.first.offset;
        bool paragraph = preview ? preview->txt_at_para_start : true;
        do {
            if (owner->background_paused_.load() || generation != owner->chapter_layout_generation_.load() ||
                (cancelled && cancelled->load())) {
                error_ = BookReadError::kCancelled;
                return false;
            }
            if (!reader.FillTxtLivePreview(offset, paragraph)) {
                error_ = reader.Error();
                return false;
            }
            const uint32_t end = std::min(range.end.offset, reader.live_preview_next_off_);
            for (auto& item : reader.live_preview_page_.items) {
                item.source_block = 0;
                item.source_begin = offset;
                item.source_end = end;
                item.source_progress = offset;
            }
            if (!built.Push(std::move(reader.live_preview_page_))) {
                error_ = built.Error();
                return false;
            }
            if (preview || !reader.live_preview_has_next_ || end >= range.end.offset) {
                break;
            }
            if (end <= offset) {
                error_ = BookReadError::kInvalidData;
                return false;
            }
            offset = end;
            paragraph = reader.live_preview_next_at_para_start_;
        } while (true);
        return true;
    }
    for (int source = 0; source < source_chapter_count_; ++source) {
        const uint32_t raw = ebook ? ebook_spine_[source] : source;
        if (raw < range.first.chapter || raw > range.end.chapter ||
            (raw == range.end.chapter && range.end.block == 0 && range.end.offset == 0)) {
            continue;
        }
        if (!PaginateSourceChapter(source, built, generation, preview, cancelled, &range)) {
            return false;
        }
        if (preview && !built.empty()) {
            break;
        }
    }
    return !built.empty();
}

bool BookSession::LoadChapter(int chapter) {
    if (chapter == chapter_index_ && !pages_.empty()) {
        page_index_ = 0;
        navigation_anchor_ = {};
        return true;
    }
    if (navigation_loading_chapter_) {
        navigation_loading_chapter_->store(true);
    }
    const auto* source = navigation_source_ ? navigation_source_ : this;
    std::lock_guard<std::mutex> lock(source->chapter_io_mu_);
    BookPageStore built;
    if (!built.Open(info_.path)) {
        error_ = built.Error();
        return false;
    }
    if (!source->PaginateChapter(chapter, built, source->chapter_layout_generation_.load(), nullptr, navigation_cancelled_) ||
        !built.Finish()) {
        error_ = built.Good() ? source->Error() : built.Error();
        return false;
    }
    pages_.swap(built);
    navigation_replaced_pages_ = navigation_source_ != nullptr;
    navigation_anchor_ = {};
    page_index_ = 0;
    chapter_index_ = chapter;
    ReportOpenProgress(90);
    return true;
}

bool BookSession::PaginateSourceChapter(int chapter, BookPageStore& built, uint32_t generation, const LayoutAnchor* preview,
                                  const std::atomic<bool>* cancelled, const ReadingChapter* range, bool* preview_has_more, size_t page_limit) const {
    const auto* owner = navigation_source_ ? navigation_source_ : this;
    auto* epub = owner->epub_.get();
    auto* ebook = owner->ebook_.get();
    if (preview_has_more) {
        *preview_has_more = false;
    }
    if (chapter < 0 || chapter >= source_chapter_count_) {
        return false;
    }
    const uint32_t raw_chapter = ebook ? ebook_spine_[chapter] : chapter;
    uint64_t content_offset = source_sizes_ ? (*source_sizes_)[chapter] : 0;
    uint64_t block_content_offset = content_offset;
    bool range_complete = false;
    Page cur;
    lv_coord_t y = 0, line_w = 0;
    const lv_coord_t line_h = LayoutLineHeight();
    const auto layout = LineLayout();
    int row = 0;
    bool previous_title = false;
    std::string line;
    VerticalBreakState breaks;
    TextSpacingState spacing;
    uint32_t block_index = UINT32_MAX, offset = 0, line_begin = 0;
    bool first_line = true;
    bool illustration_placeholder = false;
    size_t glyphs = 0;
    bool page_limit_reached = false;
    ZipReader image_source;
    auto flush_line = [&](bool title, bool justify = false, bool hyphenated = false) {
        if (line.empty()) {
            return true;
        }
        const bool indent = first_line && !title;
        if (indent) {
            StripLeadingParaWs(line);
        }
        if (hyphenated) {
            line.push_back('-');
        }
        const lv_coord_t gap = first_line ? ReaderParagraphGap(para_gap_, previous_title) : 0;
        const bool ok = FlushLineToPage(line, built, cur, y, layout, row, gap, indent, title, first_line, block_index,
                                        illustration_placeholder ? 0 : line_begin, illustration_placeholder ? 1 : offset,
                                        justify, hyphenated, illustration_placeholder, raw_chapter, block_content_offset + line_begin);
        if ((preview && !built.empty()) || built.size() >= page_limit) {
            page_limit_reached = true;
        }
        first_line = false;
        line_w = 0;
        breaks = {};
        spacing = {};
        return ok && !page_limit_reached;
    };
    auto interrupted = [&]() {
        if (owner->background_paused_.load() || generation != owner->chapter_layout_generation_ || (cancelled && cancelled->load())) {
            error_ = BookReadError::kCancelled;
            return true;
        }
        return false;
    };
    bool first = true, inserted = false, removed = false;
    std::string raw_first, displayed_first;
    std::vector<BookSourcePosition> epub_breaks;
    std::vector<bool> epub_break_seen(epub ? epub->Toc().size() : 0, false);
    size_t epub_break_index = 0, epub_break_cached_index = SIZE_MAX;
    BookSourcePosition epub_next_break;
    const auto epub_source_position = [&](uint32_t block, uint32_t source_offset) {
        BookSourcePosition position{static_cast<uint32_t>(chapter), block, source_offset};
        if (block > 0) {
            position.block =
                static_cast<uint32_t>(static_cast<int64_t>(block) + (inserted ? 1 : 0) - (removed ? 1 : 0));
        } else {
            position.block = inserted ? 1 : 0;
            position.offset = removed ? 0 : EpubFirstSourceOffset(raw_first, displayed_first, source_offset);
        }
        return position;
    };
    const auto apply_epub_breaks = [&](bool title) {
        const BookSourcePosition current{static_cast<uint32_t>(chapter), block_index, offset};
        bool boundary = false;
        while (epub_break_index < epub_breaks.size()) {
            if (epub_break_cached_index != epub_break_index) {
                const auto& raw = epub_breaks[epub_break_index];
                epub_next_break = epub_source_position(raw.block, raw.offset);
                epub_break_cached_index = epub_break_index;
            }
            if (SourceBefore(current, epub_next_break)) {
                break;
            }
            ++epub_break_index;
            boundary = true;
        }
        if (!boundary) {
            return true;
        }
        if (!line.empty() && !flush_line(title)) {
            return false;
        }
        if (!cur.items.empty()) {
            if (!built.Push(std::move(cur))) {
                return false;
            }
            if (preview || built.size() >= page_limit) {
                page_limit_reached = true;
                return false;
            }
            cur = Page{};
            y = 0;
            row = 0;
        }
        return true;
    };
    auto consume = [&](ContentBlock&& block) -> bool {
        if (interrupted()) {
            return false;
        }
        if (!block.continuation) {
            ++block_index;
            offset = 0;
            first_line = true;
            line_w = 0;
        }
        const uint32_t chunk_offset = offset;
        block_content_offset = content_offset - chunk_offset;
        content_offset += block.kind == ContentKind::kImage ? 1 : block.text.size();
        if (range) {
            const BookSourcePosition begin{raw_chapter, block_index, offset};
            const uint32_t length = block.kind == ContentKind::kImage ? 1 : block.text.size();
            const BookSourcePosition end{raw_chapter, block_index, offset + length};
            if (!SourceBefore(begin, range->end)) {
                range_complete = true;
                return false;
            }
            if (!SourceBefore(range->first, end)) {
                offset += length;
                return true;
            }
            if (raw_chapter == range->end.chapter && block_index == range->end.block &&
                block.kind == ContentKind::kText && range->end.offset < offset + block.text.size()) {
                block.text.resize(range->end.offset - offset);
                block.continues = false;
            }
            if (raw_chapter == range->first.chapter && block_index == range->first.block &&
                block.kind == ContentKind::kText && offset < range->first.offset) {
                const size_t skip = std::min<size_t>(range->first.offset - offset, block.text.size());
                block.text.erase(0, skip);
                offset += skip;
                first_line = offset == 0;
            }
        }
        if (preview && raw_chapter < preview->source_chapter) {
            offset += block.kind == ContentKind::kImage ? 1 : block.text.size();
            return true;
        }
        if (preview && raw_chapter == preview->source_chapter && block_index < preview->source_block) {
            return true;
        }
        if (preview && raw_chapter == preview->source_chapter && block_index == preview->source_block &&
            offset < preview->source_offset) {
            const size_t skip = std::min<size_t>(preview->source_offset - offset, block.text.size());
            block.text.erase(0, skip);
            offset += skip;
            first_line = preview->source_para_start;
            if (offset < preview->source_offset) {
                return true;
            }
        }
        illustration_placeholder = block.kind == ContentKind::kImage && !text_style_.inline_images;
        if (illustration_placeholder) {
            block.kind = ContentKind::kText;
            block.text = UiIllustration();
            first_line = false;
        }
        if (block.kind == ContentKind::kText) {
            if (block.chapter_title && !epub && !cur.items.empty()) {
                if (!built.Push(std::move(cur))) {
                    return false;
                }
                if (preview || built.size() >= page_limit) {
                    page_limit_reached = true;
                    return false;
                }
                cur = Page{};
                y = 0;
                row = 0;
            }
            const auto* data = reinterpret_cast<const uint8_t*>(block.text.data());
            size_t english_end = 0, english_split = SIZE_MAX;
            for (size_t i = 0; i < block.text.size();) {
                if (interrupted()) {
                    return false;
                }
                if ((++glyphs & 0x7FFu) == 0) {
#ifndef EBOOK_EMULATOR
                    vTaskDelay(1);
#endif
                }
                if (!apply_epub_breaks(block.chapter_title))
                    return false;
                if (i == english_split) {
                    if (!flush_line(block.chapter_title, true, true)) {
                        return false;
                    }
                    english_split = SIZE_MAX;
                    english_end = i;
                }
                uint32_t cp = 0;
                const size_t n = Utf8Next(data + i, block.text.size() - i, &cp);
                if (!n) {
                    error_ = BookReadError::kInvalidData;
                    return false;
                }
                if (cp == '\n' && !(vertical_writing_ && block.chapter_title)) {
                    if (!flush_line(block.chapter_title))
                        return false;
                    i += n;
                    offset += n;
                    continue;
                }
                if (first_line && line.empty() && IsTxtParaIndentWs(cp)) {
                    i += n;
                    offset += n;
                    continue;
                }
                if (!vertical_writing_ && ReaderEnglishLetter(cp) && i >= english_end) {
                    english_end = i + 1;
                    while (english_end < block.text.size()) {
                        const unsigned char next = block.text[english_end];
                        if (!ReaderEnglishLetter(next) && !(next == '\'' && english_end + 1 < block.text.size() &&
                            ReaderEnglishLetter(static_cast<unsigned char>(block.text[english_end - 1])) &&
                            ReaderEnglishLetter(static_cast<unsigned char>(block.text[english_end + 1])))) {
                            break;
                        }
                        ++english_end;
                    }
                    const int used = line.empty() && first_line && !block.chapter_title ? LayoutIndentWidth() : line_w;
                    const auto plan = ReaderPlanEnglishBreak(
                        std::string_view(block.text).substr(i, english_end - i), viewport_w_ - used,
                        kMaxLineSourceBytes - line.size(), line.empty(),
                        [&](uint32_t letter) { return LayoutGlyphWidth(letter); });
                    if (plan.move_word) {
                        if (!flush_line(block.chapter_title, true)) {
                            return false;
                        }
                        english_end = i;
                        continue;
                    }
                    english_split = plan.hyphen ? i + plan.prefix : SIZE_MAX;
                }
                lv_coord_t width = LayoutGlyphWidth(cp);
                if ((cp == 0x20 || cp == 0xA0) && width <= 0)
                    width = std::max<lv_coord_t>(1, line_h / 3);
                TextSpacingState candidate_spacing = spacing;
                if (!vertical_writing_ && line_w + width > viewport_w_)
                    ObserveTextGlyph(candidate_spacing, glyph_font_, cp, text_style_.letter_gap);
                VerticalBreakState candidate = breaks;
                candidate.Observe(cp, offset, line.size(), !vertical_writing_);
                if (!line.empty() && ((line_w + width > viewport_w_ &&
                                       (vertical_writing_ || !CanFitTrailingPunctuation(cp, line_w + width, viewport_w_,
                                                                                        candidate_spacing))) ||
                                      line.size() + n > kMaxLineSourceBytes)) {
                    if (candidate.valid && candidate.offset < offset) {
                        const uint32_t cut = candidate.offset;
                        std::string tail = line.substr(candidate.text_size);
                        line.resize(candidate.text_size);
                        const uint32_t end = offset;
                        offset = cut;
                        if (!flush_line(block.chapter_title, true))
                            return false;
                        line = std::move(tail);
                        line_begin = cut;
                        offset = end;
                        for (size_t j = 0; j < line.size();) {
                            if (interrupted())
                                return false;
                            uint32_t tail_cp = 0;
                            const size_t count =
                                Utf8Next(reinterpret_cast<const uint8_t*>(line.data()) + j, line.size() - j, &tail_cp);
                            if (!count)
                                return false;
                            if (!vertical_writing_)
                                ObserveTextGlyph(spacing, glyph_font_, tail_cp, text_style_.letter_gap);
                            breaks.Observe(tail_cp, cut + j, j, !vertical_writing_);
                            line_w += LayoutGlyphWidth(tail_cp);
                            j += count;
                        }
                    } else if (!flush_line(block.chapter_title, true))
                        return false;
                }
                if (line.empty()) {
                    line_begin = offset;
                    line_w = first_line && !block.chapter_title ? LayoutIndentWidth() : 0;
                }
                if (!vertical_writing_)
                    ObserveTextGlyph(spacing, glyph_font_, cp, text_style_.letter_gap);
                breaks.Observe(cp, offset, line.size(), !vertical_writing_);
                line.append(block.text.data() + i, n);
                line_w += width;
                i += n;
                offset += n;
            }
            if (!apply_epub_breaks(block.chapter_title))
                return false;
            if (!block.continues) {
                if (!line.empty() && !flush_line(block.chapter_title))
                    return false;
                previous_title = block.chapter_title;
            }
            if (illustration_placeholder && page_images_mode_ && !cur.items.empty()) {
                if (!built.Push(std::move(cur))) {
                    return false;
                }
                if (preview || built.size() >= page_limit) {
                    page_limit_reached = true;
                    return false;
                }
                cur = Page{};
                y = 0;
                row = 0;
            }
            return true;
        }
        if (!apply_epub_breaks(false))
            return false;
        int img_w = physical_w_, img_h = physical_h_;
        const bool full_image = page_images_mode_ || vertical_writing_;
        if (!full_image) {
            int width = block.image_width, height = block.image_height;
            if (epub) {
                // HTML 流仍占用文档 ZIP，尺寸探测使用独立句柄，避免嵌套锁。
                if (!image_source.IsOpen() && !image_source.Open(info_.path.c_str())) {
                    error_ = image_source.Error();
                    return false;
                }
                if (image_source.OpenEntryStream(block.image_href.c_str())) {
                    ReadImageDimensions(
                        [](void* context, uint8_t* bytes, size_t size) {
                            return static_cast<ZipReader*>(context)->EntryStreamRead(bytes, size);
                        },
                        &image_source, &width, &height);
                    image_source.CloseEntryStream();
                }
            }
            // 未知或不支持的图片保留独立占位，避免影响后续内容边界。
            FitImageSize(width, height, physical_w_, physical_h_, &img_w, &img_h);
        }
        if (!cur.items.empty() && (full_image || y + img_h > viewport_h_)) {
            if (!built.Push(std::move(cur)))
                return false;
            if (preview || built.size() >= page_limit) {
                page_limit_reached = true;
                return false;
            }
            cur = Page{};
            y = 0;
            row = 0;
        }
        PageItem item;
        item.kind = ContentKind::kImage;
        item.source_chapter = raw_chapter;
        item.source_progress = block_content_offset;
        item.source_block = block_index;
        item.source_end = 1;
        item.image_href = std::move(block.image_href);
        item.image_w = img_w;
        item.image_h = img_h;
        cur.items.push_back(std::move(item));
        y += img_h;
        previous_title = false;
        if (full_image) {
            if (!built.Push(std::move(cur)))
                return false;
            if (preview || built.size() >= page_limit) {
                page_limit_reached = true;
                return false;
            }
            cur = Page{};
            y = 0;
            row = 0;
        }
        return true;
    };
    HtmlAnchorSink anchors;
    if (epub) {
        bool has_fragments = false;
        for (size_t i = 0; i < epub->Toc().size(); ++i) {
            if (epub->Toc()[i].spine_index != chapter) {
                continue;
            }
            has_fragments = has_fragments || !epub->Toc()[i].fragment.empty();
        }
        if (has_fragments) {
            anchors = [&](const std::string& name, uint32_t block, uint32_t source_offset) {
                bool boundary = false;
                for (size_t i = 0; i < epub->Toc().size(); ++i) {
                    const auto& entry = epub->Toc()[i];
                    if (entry.spine_index != chapter || entry.fragment.empty() || entry.fragment != name) {
                        continue;
                    }
                    if (!epub_break_seen[i]) {
                        boundary = true;
                        epub_break_seen[i] = true;
                    }
                }
                const BookSourcePosition position{static_cast<uint32_t>(chapter), block, source_offset};
                // 父目录与子目录可能指向同一位置，只产生一次分页边界。
                if (boundary && (epub_breaks.empty() || SourceBefore(epub_breaks.back(), position))) {
                    epub_breaks.push_back(position);
                }
            };
        }
    }
    const auto sink = [&](ContentBlock&& block) {
        if (page_images_mode_) {
            return consume(std::move(block));
        }
        if (!first) {
            return consume(std::move(block));
        }
        first = false;
        if (epub) {
            raw_first = block.text;
        }
        std::vector<ContentBlock> initial;
        initial.push_back(std::move(block));
        const std::string title =
            ebook ? ebook->ChapterTitle(ebook_spine_[chapter]) : FormatChapterOrdinalTitle(chapter + 1);
        EnsureChapterTitleInBlocks(title, chapter + 1, initial, &inserted);
        removed = initial.size() == (inserted ? 1u : 0u);
        if (epub && !removed) {
            displayed_first = initial[inserted ? 1 : 0].text;
        }
        for (size_t i = 0; i < initial.size(); ++i) {
            // 旧逻辑兼容：保留生成标题所占的块编号，既有 EPUB 书签仍指向原文。
            if (epub && inserted && i == 0) {
                ++block_index;
                continue;
            }
            if (!consume(std::move(initial[i]))) {
                return false;
            }
        }
        return true;
    };
    error_ = BookReadError::kInvalidData;
    bool ok = false;
    try {
        if (ebook) {
            ok = ebook->StreamChapterBlocks(ebook_spine_[chapter], sink);
        } else if (epub) {
            ok = epub->StreamChapterBlocks(chapter, sink, anchors, cancelled);
        }
        if ((page_limit_reached || range_complete) && !interrupted()) {
            ok = true;
        }
        if (ok && !page_limit_reached && !cur.items.empty()) {
            ok = built.Push(std::move(cur));
        }
        if (ok && built.empty() && !range && !preview) {
            Page empty;
            PageItem item;
            item.text = UiChapterNoText();
            empty.items.push_back(std::move(item));
            ok = built.Push(std::move(empty));
        }
    } catch (const std::bad_alloc&) {
        error_ = BookReadError::kNoMemory;
        return false;
    }
    if (!built.Good()) {
        error_ = built.Error();
    } else if (!ok && error_ == BookReadError::kInvalidData) {
        const auto doc_error = ebook ? ebook->Error() : epub->Error();
        if (doc_error != BookReadError::kNone) {
            error_ = doc_error;
        }
    }
    if (ok) {
        error_ = BookReadError::kNone;
    }
    if (preview_has_more) {
        *preview_has_more = page_limit_reached;
    }
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
        if (ebook_->LoadCoverBytes(bytes) && DecodeImageToL8(bytes.data(), bytes.size(), max_w, max_h, cover_)) {
            return true;
        }
    }
    return false;
}

#ifndef EBOOK_EMULATOR
void BookSession::ReclaimOptionalMemory(void* context) {
    auto& session = *static_cast<BookSession*>(context);
    session.page_image_reclaim_pending_.store(true);
    std::unique_lock<std::mutex> lock(session.page_image_cache_mu_, std::try_to_lock);
    if (lock.owns_lock()) {
        session.ReclaimPageImagesLocked();
    }
}
#endif

void BookSession::ReclaimPageImagesLocked() const {
    if (page_image_reclaim_pending_.exchange(false)) {
        std::vector<CachedPageImage>().swap(cached_page_images_);
    }
}

size_t BookSession::PageImageCacheBytesLocked() const {
    ReclaimPageImagesLocked();
    size_t used = 0;
    for (const auto& entry : cached_page_images_) {
        used += entry.image.pixels.capacity();
    }
    return used;
}

size_t BookSession::PageImageCacheBudgetLocked(size_t used) const {
    if (!page_images_mode_) {
        return size_t(std::max<int>(1, physical_w_)) * std::max<int>(1, physical_h_);
    }
    #ifndef EBOOK_EMULATOR
    return FirmwarePsramCacheBudget(used);
    #else
    (void)used;
    constexpr size_t available = 64 * 1024 * 1024;
    constexpr size_t reserve = 2 * 1024 * 1024;
    return std::min(available / 2, available - reserve);
    #endif
}

bool BookSession::LoadPageImage(const std::string& href, int max_w, int max_h, RasterImage& out) {
    out.Reset();
    if (href.empty()) {
        return false;
    }
    if (TryCopyCachedPageImage(href, max_w, max_h, out)) {
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
    #ifndef EBOOK_EMULATOR
    FirmwarePrefetchScope cache(out.pixels.size());
    if (!cache.Valid()) {
        return true;
    }
    const uint32_t reclaim_generation = FirmwareMemoryReclaimGeneration();
    #else
    const uint32_t reclaim_generation = UINT32_MAX;
    #endif
    try {
        RasterImage copy(out);
        SetCachedPageImage(href, max_w, max_h, std::move(copy), reclaim_generation);
    } catch (const std::bad_alloc&) {
        ESP_LOGD("BookSession", "operation=page_image stage=cache_copy result=no_memory");
    }
    return true;
}

bool BookSession::TryCopyCachedPageImage(const std::string& href, int max_w, int max_h, RasterImage& out) const try {
    out.Reset();
    #ifndef EBOOK_EMULATOR
    FirmwareMemoryDemand demand(size_t(std::max(0, max_w)) * std::max(0, max_h));
    #endif
    std::lock_guard<std::mutex> lock(page_image_cache_mu_);
    ReclaimPageImagesLocked();
    auto entry = std::find_if(cached_page_images_.begin(), cached_page_images_.end(),
                              [&](const auto& e) { return e.href == href && e.max_w == max_w && e.max_h == max_h; });
    if (entry == cached_page_images_.end()) {
        return false;
    }
    out = entry->image;
    std::rotate(entry, entry + 1, cached_page_images_.end());
    return true;
} catch (const std::bad_alloc&) {
    out.Reset();
    return false;
}

bool BookSession::HasCachedPageImage(const std::string& href, int max_w, int max_h) const {
    std::lock_guard<std::mutex> lock(page_image_cache_mu_);
    ReclaimPageImagesLocked();
    auto entry = std::find_if(cached_page_images_.begin(), cached_page_images_.end(),
                              [&](const auto& e) { return e.href == href && e.max_w == max_w && e.max_h == max_h; });
    if (entry == cached_page_images_.end()) {
        return false;
    }
    std::rotate(entry, entry + 1, cached_page_images_.end());
    return true;
}

bool BookSession::CanPrefetchPageImage() const {
    std::lock_guard<std::mutex> lock(page_image_cache_mu_);
    if (!page_images_mode_ || physical_w_ <= 0 || physical_h_ <= 0) {
        return false;
    }
    const size_t used = PageImageCacheBytesLocked();
    const size_t budget = PageImageCacheBudgetLocked(used);
    const size_t frame = size_t(physical_w_) * physical_h_;
    return used <= budget && frame <= budget - used;
}

void BookSession::PreparePageImagePrefetch(const std::string& anchor) {
    std::lock_guard<std::mutex> lock(page_image_cache_mu_);
    if (anchor == page_image_anchor_) {
        return;
    }
    page_image_anchor_ = anchor;
    size_t used = PageImageCacheBytesLocked();
    const size_t budget = PageImageCacheBudgetLocked(used);
    const size_t working = size_t(std::max<int>(0, physical_w_)) * std::max<int>(0, physical_h_) * 2;
    const size_t target = budget > working ? budget - working : 0;
    while (used > target && !cached_page_images_.empty()) {
        used -= cached_page_images_.front().image.pixels.capacity();
        cached_page_images_.erase(cached_page_images_.begin());
    }
}

void BookSession::ClearPageImageCache() {
    std::lock_guard<std::mutex> lock(page_image_cache_mu_);
    std::vector<CachedPageImage>().swap(cached_page_images_);
    std::string().swap(page_image_anchor_);
    page_image_reclaim_pending_.store(false);
}

void BookSession::SetCachedPageImage(const std::string& href, int max_w, int max_h, RasterImage&& img,
                                     uint32_t reclaim_generation) try {
    #ifndef EBOOK_EMULATOR
    if (!FirmwarePrefetchAllowed() ||
        reclaim_generation != FirmwareMemoryReclaimGeneration()) {
        return;
    }
    #else
    (void)reclaim_generation;
    #endif
    if (href.empty() || img.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(page_image_cache_mu_);
    ReclaimPageImagesLocked();
    size_t used = img.pixels.capacity();
    for (auto entry = cached_page_images_.begin(); entry != cached_page_images_.end();) {
        if (entry->href == href && entry->max_w == max_w && entry->max_h == max_h) {
            entry = cached_page_images_.erase(entry);
        } else {
            used += entry->image.pixels.capacity();
            ++entry;
        }
    }
    // 图像已完成分配，将即将接管的像素计入已有缓存，避免再次扣除。
    const size_t budget = PageImageCacheBudgetLocked(used);
    if (img.pixels.capacity() > budget) {
        return;
    }
    while (!cached_page_images_.empty() && (used > budget || !page_images_mode_)) {
        used -= cached_page_images_.front().image.pixels.capacity();
        cached_page_images_.erase(cached_page_images_.begin());
    }
    cached_page_images_.push_back({href, max_w, max_h, std::move(img)});
    #ifndef EBOOK_EMULATOR
    if (reclaim_generation != FirmwareMemoryReclaimGeneration()) {
        std::vector<CachedPageImage>().swap(cached_page_images_);
        return;
    }
    #endif
} catch (const std::bad_alloc&) {
    // 缓存为可选加速，分配失败不影响当前页显示。
}

const Page* BookSession::CurrentPageData() const try {
    if (live_preview_active_) {
        return &live_preview_page_;
    }
    if (page_index_ < 0 || page_index_ >= PageCount()) {
        return nullptr;
    }
    const Page& page = pages_[static_cast<size_t>(page_index_)];
    return pages_.Good() ? &page : nullptr;
} catch (const std::bad_alloc&) {
    error_ = BookReadError::kNoMemory;
    return nullptr;
}

bool BookSession::GoToPage(int page) {
    navigation_anchor_ = {};
    if (live_preview_active_) {
        return false;
    }
    if (page < 0 || page >= PageCount()) {
        return false;
    }
    if (!txt_mode_) {
        pages_[static_cast<size_t>(page)];
        if (!pages_.Good()) {
            error_ = pages_.Error();
            return false;
        }
    }
    page_index_ = page;
    return true;
}

bool BookSession::IsChapteredBook() const {
    return reading_chapters_ && !reading_chapters_->empty();
}

void BookSession::SetBackgroundPaused(bool paused) {
    background_paused_.store(paused);
    if (paused) {
        AbortChapterLayout();
    } else if (error_ == BookReadError::kCancelled) {
        error_ = BookReadError::kNone;
    }
}

uint32_t BookSession::AbortChapterLayout() {
    return ++chapter_layout_generation_;
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
        return structure_pending_ && !txt_mode_ ? !source_preview_history_.empty() : !live_preview_hist_.empty();
    }
    if (page_index_ > 0) {
        return true;
    }
    return IsChapteredBook() && chapter_index_ > 0;
}

bool BookSession::NextPage() {
    navigation_anchor_ = {};
    if (structure_pending_ && live_preview_active_ && !txt_mode_) {
        if (!live_preview_has_next_) {
            return false;
        }
        const auto previous = source_preview_anchor_;
        if (!PrepareSourcePreview(source_preview_next_)) {
            return false;
        }
        if (source_preview_history_.size() == kLivePreviewHistMax) {
            source_preview_history_.erase(source_preview_history_.begin());
        }
        source_preview_history_.push_back(previous);
        preview_toc_index_ = -1;
        return true;
    }
    if (live_preview_active_) {
        if (!live_preview_has_next_) {
            return false;
        }
        if (live_preview_hist_.size() < kLivePreviewHistMax) {
            try {
                live_preview_hist_.push_back(LivePreviewHistEnt{live_preview_byte_off_, live_preview_at_para_start_});
            } catch (const std::bad_alloc&) {
                // 栈满则仍翻下一页，只是更早无法回退
            }
        } else if (!live_preview_hist_.empty()) {
            // ponytail: 满则丢最旧，保近期回退
            live_preview_hist_.erase(live_preview_hist_.begin());
            live_preview_hist_.push_back(LivePreviewHistEnt{live_preview_byte_off_, live_preview_at_para_start_});
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
        return true;
    }
    if (IsChapteredBook() && chapter_index_ + 1 < chapter_count_) {
        return LoadChapter(chapter_index_ + 1);
    }
    return false;
}

bool BookSession::PrevPage() {
    navigation_anchor_ = {};
    if (structure_pending_ && live_preview_active_ && !txt_mode_) {
        if (source_preview_history_.empty()) {
            return false;
        }
        const auto previous = source_preview_history_.back();
        if (!PrepareSourcePreview(previous)) {
            return false;
        }
        source_preview_history_.pop_back();
        preview_toc_index_ = -1;
        return true;
    }
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
        return true;
    }
    if (IsChapteredBook() && chapter_index_ > 0) {
        const bool ok = LoadChapter(chapter_index_ - 1);
        if (ok) {
            page_index_ = std::max(0, PageCount() - 1);
            return true;
        }
    }
    return false;
}

void BookSession::ClearToc() {
    toc_.clear();
}

void BookSession::ClearLivePreview() {
    live_preview_active_ = false;
    chapter_preview_pending_ = false;
    txt_preview_relayout_pending_ = false;
    live_preview_byte_off_ = 0;
    live_preview_at_para_start_ = true;
    live_preview_next_off_ = 0;
    live_preview_next_at_para_start_ = true;
    live_preview_has_next_ = false;
    live_preview_page_ = Page{};
    live_preview_hist_.clear();
}

BookSession::LayoutAnchor BookSession::CaptureLayoutAnchor() const {
    if (navigation_anchor_.valid && navigation_anchor_.page == page_index_ &&
        navigation_anchor_.chapter == chapter_index_) {
        return navigation_anchor_;
    }
    LayoutAnchor a;
    if (!open_) {
        return a;
    }
    if (structure_pending_ && live_preview_active_ && !txt_mode_) {
        return source_preview_anchor_;
    }
    a.valid = true;
    a.txt = info_.format == BookFormat::kTxt;
    a.chapter = chapter_index_;
    a.page = std::max(0, page_index_);
    a.page_count = std::max(1, PageCount());
    if (txt_mode_) {
        if (live_preview_active_) {
            a.txt_byte_off = live_preview_byte_off_;
            a.txt_at_para_start = live_preview_at_para_start_;
        }
    } else {
        const auto* page = CurrentPageData();
        if (page) {
            for (const auto& item : page->items) {
                if (item.source_block == UINT32_MAX) {
                    continue;
                }
                a.source_chapter = item.source_chapter;
                if (info_.format == BookFormat::kTxt) {
                    a.txt_byte_off = item.source_begin;
                    a.txt_at_para_start = item.para_indent || item.chapter_title;
                }
                a.source_block = item.source_block;
                a.source_offset = item.source_begin;
                a.source_para_start = item.para_indent || item.chapter_title;
                break;
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
        error_ = BookReadError::kFileIo;
        return false;
    }
    if (std::fseek(txt_fp_, static_cast<long>(byte_off), SEEK_SET) != 0) {
        error_ = BookReadError::kFileIo;
        return false;
    }

    BookSessionTxtByteStream stream;
    if (!stream.InitBuf()) {
        error_ = BookReadError::kNoMemory;
        return false;
    }
    stream.fp = txt_fp_;
    stream.as_gb18030 = txt_as_gb18030_;
    stream.file_base = byte_off;
    stream.pos = 0;
    stream.end = 0;
    stream.eof = false;
    if (byte_off == 0) {
        stream.SkipBomIfAny();
    }

    const lv_coord_t indent_w = LayoutIndentWidth();
    const auto layout = LineLayout();
    if (layout.count <= 0) {
        error_ = BookReadError::kUnsupported;
        return false;
    }
    int row = 0;
    lv_coord_t y = 0;
    std::string line;
    lv_coord_t line_w = 0;
    bool line_started = false;
    bool line_para_start = false;
    bool para_at_start = at_para_start;
    bool para_saw_indent_ws = false;
    TxtChapterDetector detector;
    TxtHeadingKind para_kind = TxtHeadingKind::kNone;
    std::string paragraph_title;
    uint32_t line_start_off = byte_off;
    bool page_full = false;
    unsigned glyph_count = 0;
    std::string english_word;
    size_t english_remaining = 0;
    size_t english_prefix = SIZE_MAX;

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
            error_ = BookReadError::kNoMemory;
            return false;
        }
        return true;
    };

    VerticalBreakState breaks;
    TextSpacingState spacing;
    auto flush_line = [&](bool justify = false, bool hyphenated = false) -> int {
        // 返回 1 表示成功、0 表示页满、-1 表示分配失败。
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
        // 章名另起一页，折行分别加粗居中。
        const bool heading = para_kind != TxtHeadingKind::kNone;
        const bool is_chap = line_para_start && para_kind == TxtHeadingKind::kChapter;
        const bool indent = line_para_start && !heading;
        const bool after_chap =
            line_para_start && !live_preview_page_.items.empty() && live_preview_page_.items.back().chapter_title;
        const lv_coord_t gap = para_kind == TxtHeadingKind::kSubtitle ? 0 : ReaderParagraphGap(para_gap_, after_chap);
        lv_coord_t need_y = y;
        if (line_para_start && y > 0) {
            need_y += gap;
        }
        // 章名有副标题时展示两行，预留高度
        lv_coord_t chap_extra = 0;
        if (is_chap && !vertical_writing_) {
            std::string ord;
            std::string name;
            if (SplitChapterHeading(line, ord, name) && !name.empty()) {
                chap_extra = layout.CellHeight(row + 1);
            }
        }
        const int rows_needed = chap_extra > 0 ? 2 : 1;
        if ((is_chap || row + rows_needed > layout.count || need_y + layout.CellHeight(row) + chap_extra > viewport_h_) &&
            !live_preview_page_.items.empty()) {
            live_preview_next_off_ = line_start_off;
            live_preview_next_at_para_start_ = line_para_start;
            live_preview_has_next_ = true;
            page_full = true;
            return 0;
        }
        if (line_para_start && y > 0) {
            y += gap;
        }
        try {
            PageItem item;
            item.kind = ContentKind::kText;
            item.para_indent = indent;
            item.para_gap_before = line_para_start && para_kind != TxtHeadingKind::kSubtitle;
            item.chapter_title = heading;
            item.justify = justify && !item.chapter_title;
            item.text = is_chap && !vertical_writing_ ? FormatChapterHeadingDisplay(line) : std::move(line);
            item.hyphenated = hyphenated;
            if (hyphenated) {
                item.text.push_back('-');
            }
            live_preview_page_.items.push_back(std::move(item));
        } catch (const std::bad_alloc&) {
            error_ = BookReadError::kNoMemory;
            line.clear();
            return -1;
        }
        line.clear();
        breaks = {};
        spacing = {};
        y += layout.CellHeight(row) + chap_extra;
        row += rows_needed;
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
        if ((++glyph_count & 0x1FFu) == 0 &&
            (background_paused_.load() || (navigation_source_ && navigation_source_->background_paused_.load()) ||
             (navigation_cancelled_ && navigation_cancelled_->load()))) {
            error_ = BookReadError::kCancelled;
            return false;
        }
        if (goff >= txt_range_end_) {
            break;
        }
        if (english_prefix == 0) {
            const int fr = flush_line(true, true);
            if (fr == 0) {
                break;
            }
            if (fr < 0) {
                return false;
            }
            english_prefix = SIZE_MAX;
            english_remaining = 0;
        }
        if (is_nl) {
            if (line_started) {
                const int fr = flush_line();
                if (fr == 0) {
                    break;
                }
                if (fr < 0) {
                    return false;
                }
            } else {
                detector.BlankLine();
            }
            para_at_start = true;
            para_kind = TxtHeadingKind::kNone;
            para_saw_indent_ws = false;
            english_remaining = 0;
            english_prefix = SIZE_MAX;
            continue;
        }
        if (para_at_start && !line_started && IsTxtParaIndentWs(cp)) {
            para_saw_indent_ws = true;
            continue;
        }
        if (para_at_start) {
            bool complete = false;
            paragraph_title.clear();
            const bool known = KnownTxtHeading(TocEntries(), goff, {}) != TxtHeadingKind::kNone;
            if ((known || detector.MayStart(cp)) && !ProbeTxtParagraph(stream, goff, paragraph_title, complete)) {
                error_ = BookReadError::kFileIo;
                return false;
            }
            para_kind = detector.Classify(paragraph_title, para_saw_indent_ws, goff, complete);
            const auto saved = complete ? KnownTxtHeading(TocEntries(), goff, paragraph_title) : TxtHeadingKind::kNone;
            if (saved != TxtHeadingKind::kNone) {
                para_kind = saved;
            }
        }
        if (!vertical_writing_ && ReaderEnglishLetter(cp) && english_remaining == 0) {
            const BookReadError probe = ProbeTxtEnglishWord(stream, goff, txt_range_end_, english_word);
            if (probe != BookReadError::kNone) {
                error_ = probe;
                return false;
            }
            const int used = !line_started && para_at_start && para_kind == TxtHeadingKind::kNone
                                 ? indent_w : line_w;
            const auto plan = ReaderPlanEnglishBreak(
                english_word, viewport_w_ - used, kMaxLineSourceBytes - line.size(), line.empty(),
                [&](uint32_t letter) { return LayoutGlyphWidth(letter); });
            if (plan.move_word) {
                const int fr = flush_line(true);
                if (fr == 0) {
                    break;
                }
                if (fr < 0) {
                    return false;
                }
                if (!stream.Seek(goff)) {
                    ESP_LOGE(TAG, "operation=english_wrap stage=rewind errno=%d", errno);
                    error_ = BookReadError::kFileIo;
                    return false;
                }
                continue;
            }
            english_remaining = english_word.size();
            english_prefix = plan.hyphen ? plan.prefix : SIZE_MAX;
        }
        lv_coord_t cw = LayoutGlyphWidth(cp);
        if ((cp == 0x20 || cp == 0xA0) && cw <= 0) {
            cw = std::max<lv_coord_t>(1, LayoutLineHeight() / 3);
        }
        TextSpacingState candidate_spacing = spacing;
        if (!vertical_writing_ && line_w + cw > viewport_w_) {
            ObserveTextGlyph(candidate_spacing, glyph_font_, cp, text_style_.letter_gap);
        }
        VerticalBreakState candidate = breaks;
        candidate.Observe(cp, goff, line.size(), !vertical_writing_);
        const size_t utf8_bytes = cp < 0x80u ? 1 : cp < 0x800u ? 2 : cp < 0x10000u ? 3 : 4;
        if (line_started &&
            ((line_w + cw > viewport_w_ &&
              (vertical_writing_ || !CanFitTrailingPunctuation(cp, line_w + cw, viewport_w_, candidate_spacing))) ||
             line.size() + utf8_bytes > kMaxLineSourceBytes)) {
            const bool rewind = candidate.valid && candidate.offset < goff;
            const uint32_t cut = rewind ? candidate.offset : goff;
            if (rewind) {
                line.resize(candidate.text_size);
            }
            const int fr = flush_line(true);
            if (fr == 0) {
                break;
            }
            if (fr < 0) {
                return false;
            }
            if (rewind) {
                if (!stream.Seek(cut)) {
                    error_ = BookReadError::kFileIo;
                    return false;
                }
                english_remaining = 0;
                english_prefix = SIZE_MAX;
                continue;
            }
        }
        if (!line_started) {
            line_started = true;
            line_start_off = goff;
            if (para_at_start) {
                line_w = para_kind == TxtHeadingKind::kNone ? indent_w : 0;
                line_para_start = true;
                para_at_start = false;
            } else {
                line_w = 0;
            }
        }
        if (!vertical_writing_) {
            ObserveTextGlyph(spacing, glyph_font_, cp, text_style_.letter_gap);
        }
        breaks.Observe(cp, goff, line.size(), !vertical_writing_);
        if (!append_cp(cp)) {
            return false;
        }
        line_w += cw;
        if (english_remaining > 0) {
            --english_remaining;
        }
        if (english_prefix != SIZE_MAX && english_prefix > 0) {
            --english_prefix;
        }
    }
    if (!page_full && line_started) {
        const int fr = flush_line();
        if (fr < 0) {
            return false;
        }
    }
    if (!page_full) {
        live_preview_next_off_ = std::min(stream.Tell(), txt_range_end_);
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

bool BookSession::BeginLiveRelayout(LayoutAnchor* anchor_inout) {
    if (!open_ || font_ == nullptr) {
        return false;
    }
    if (!text_style_.inline_images) {
        ClearPageImageCache();
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

    navigation_anchor_ = {};
    if (structure_pending_) {
        structure_failed_.store(false);
        source_preview_history_.clear();
        if (!txt_mode_) {
            chapter_preview_anchor_ = anchor;
            chapter_preview_pending_ = true;
            AbortChapterLayout();
            return true;
        }
    }
    if (txt_mode_) {
        live_preview_hist_.clear();
        live_preview_byte_off_ = anchor.txt_byte_off;
        live_preview_at_para_start_ = anchor.txt_at_para_start;
        live_preview_active_ = true;
        txt_preview_relayout_pending_ = true;
        if (anchor_inout != nullptr) {
            *anchor_inout = anchor;
            anchor_inout->txt_byte_off = live_preview_byte_off_;
            anchor_inout->txt_at_para_start = live_preview_at_para_start_;
        }
        return true;
    }
    if (IsChapteredBook()) {
        ClearLivePreview();
        chapter_relayout_pending_ = true;
        chapter_preview_anchor_ = anchor;
        chapter_preview_pending_ = !page_images_mode_ && anchor.source_block != UINT32_MAX;
        live_preview_active_ = chapter_preview_pending_;
        AbortChapterLayout();
        return true;
    }
    return false;
}

bool BookSession::FinishStructureLayout(const LayoutAnchor& anchor, uint32_t generation) {
    std::lock_guard<std::mutex> lock(chapter_io_mu_);
    if (!pending_structure_) {
        auto pending = std::make_unique<BookSession>();
        auto& staged = *pending;
        staged.navigation_source_ = this;
        staged.structure_staging_ = true;
        staged.info_ = info_;
        staged.toc_ = toc_;
        staged.epub_toc_positions_ = epub_toc_positions_;
        staged.ebook_spine_ = ebook_spine_;
        staged.chapter_count_ = staged.source_chapter_count_ = source_chapter_count_;
        staged.page_images_mode_ = page_images_mode_;
        staged.txt_mode_ = info_.format == BookFormat::kTxt;
        staged.txt_as_gb18030_ = txt_as_gb18030_;
        pending_structure_ = std::move(pending);
    }
    auto& staged = *pending_structure_;
    staged.error_ = BookReadError::kNone;
    staged.SetFont(font_, glyph_font_);
    staged.text_style_ = text_style_;
    staged.SetVerticalWriting(vertical_writing_);
    staged.SetViewport(physical_w_, physical_h_);
    staged.SetLineSpacing(line_spacing_percent_, para_gap_);
    if (!staged.source_sizes_) {
        if (!staged.structure_cache_checked_) {
            staged.structure_cache_checked_ = true;
            if (!staged.LoadStructureCache(generation)) {
                if (staged.Error() == BookReadError::kCancelled) {
                    staged.structure_cache_checked_ = false;
                    error_ = BookReadError::kCancelled;
                    return false;
                }
                staged.toc_ = toc_;
                staged.epub_toc_positions_ = epub_toc_positions_;
            }
        }
        if (!staged.source_sizes_) {
            const bool scanned = info_.format != BookFormat::kTxt || staged.ScanTxtChapters(generation);
            if (!scanned || !staged.BuildReadingChapters(generation)) {
                error_ = staged.Error();
                if (!scanned && Error() != BookReadError::kCancelled) {
                    staged.txt_structure_scan_.reset();
                    staged.toc_.clear();
                }
                return false;
            }
            struct stat current{};
            if (stat(info_.path.c_str(), &current) != 0 || current.st_size != static_cast<int64_t>(info_.file_size) ||
                current.st_mtime != info_.mtime) {
                error_ = BookReadError::kInvalidData;
                ESP_LOGE(TAG, "chapter structure failed stage=source_identity");
                return false;
            }
            if (!staged.SaveStructureCache(generation) && staged.Error() == BookReadError::kCancelled) {
                error_ = BookReadError::kCancelled;
                return false;
            }
        }
    }
    if (StructureScanInterrupted(generation)) {
        return false;
    }
    if (staged.txt_mode_) {
        return true;
    }
    const BookSourcePosition position = anchor.txt ? BookSourcePosition{0, 0, anchor.txt_byte_off} :
        BookSourcePosition{anchor.source_chapter, anchor.source_block, anchor.source_offset};
    const int chapter = staged.ReadingChapterForPosition(position);
    pending_pages_.clear();
    if (chapter < 0 || !pending_pages_.Open(info_.path) ||
        !staged.PaginateChapter(chapter, pending_pages_, generation) || !pending_pages_.Finish()) {
        error_ = pending_pages_.Good() ? staged.Error() : pending_pages_.Error();
        if (error_ == BookReadError::kNone) {
            error_ = BookReadError::kInvalidData;
        }
        pending_pages_.clear();
        return false;
    }
    staged.chapter_index_ = chapter;
    pending_chapter_anchor_page_ = 0;
    for (size_t page = 0; page < pending_pages_.size(); ++page) {
        if (StructureScanInterrupted(generation)) {
            return false;
        }
        for (const auto& item : pending_pages_[page].items) {
            if (item.source_block != UINT32_MAX &&
                SourceBefore(position, {item.source_chapter, item.source_block, item.source_end})) {
                pending_chapter_anchor_page_ = static_cast<int>(page);
                return pending_pages_.Good();
            }
        }
    }
    error_ = pending_pages_.Good() ? BookReadError::kInvalidData : pending_pages_.Error();
    return false;
}

bool BookSession::FinishLiveRelayout(const LayoutAnchor& anchor, uint32_t generation) try {
    if (StructureScanInterrupted(generation)) {
        return false;
    }
    if (structure_pending_) {
        const bool ok = FinishStructureLayout(anchor, generation);
        if (!ok && Error() != BookReadError::kCancelled) {
            structure_failed_.store(true);
        }
        return ok;
    }
    if (IsChapteredBook()) {
        std::lock_guard<std::mutex> lock(chapter_io_mu_);
        pending_pages_.clear();
        if (!pending_pages_.Open(info_.path)) {
            error_ = pending_pages_.Error();
            return false;
        }
        const int chapter = anchor.valid ? anchor.chapter : chapter_index_;
        const bool ok = PaginateChapter(chapter, pending_pages_, generation) && pending_pages_.Finish();
        if (ok) {
            pending_chapter_anchor_page_ =
                std::clamp(static_cast<int>(static_cast<int64_t>(anchor.page) * pending_pages_.size() /
                                            std::max(1, anchor.page_count)),
                           0, std::max(0, static_cast<int>(pending_pages_.size()) - 1));
            if (anchor.source_block != UINT32_MAX) {
                bool found = false;
                for (size_t p = 0; p < pending_pages_.size() && !found; ++p) {
                    if (StructureScanInterrupted(generation)) {
                        return false;
                    }
                    for (const auto& item : pending_pages_[p].items) {
                        if (item.source_block == UINT32_MAX) {
                            continue;
                        }
                        if (SourceBefore({anchor.source_chapter, anchor.source_block, anchor.source_offset},
                                         {item.source_chapter, item.source_block, item.source_end})) {
                            pending_chapter_anchor_page_ = p;
                            found = true;
                            break;
                        }
                    }
                }
                if (!pending_pages_.Good()) {
                    error_ = pending_pages_.Error();
                    return false;
                }
            }
        }
        if (!ok && !pending_pages_.Good()) {
            error_ = pending_pages_.Error();
        }
        if (!ok) {
            pending_pages_.clear();
        }
        return ok;
    }
    return true;
} catch (const std::bad_alloc&) {
    error_ = BookReadError::kNoMemory;
    pending_pages_.clear();
    if (structure_pending_) {
        structure_failed_.store(true);
    }
    if (txt_mode_) {
        txt_lines_.Reset();
        txt_page_first_line_.Reset();
        txt_lines_len_ = txt_page_count_ = 0;
    }
    return false;
}

void BookSession::CommitLiveRelayout(const LayoutAnchor& anchor) {
    navigation_anchor_ = {};
    if (structure_pending_ && pending_structure_ && pending_structure_->source_sizes_) {
        auto& staged = *pending_structure_;
        toc_.swap(staged.toc_);
        epub_toc_positions_.swap(staged.epub_toc_positions_);
        reading_chapters_ = std::move(staged.reading_chapters_);
        source_sizes_ = std::move(staged.source_sizes_);
        chapter_count_ = staged.chapter_count_;
        chapter_index_ = staged.txt_mode_ ? 0 : staged.chapter_index_;
        txt_mode_ = staged.txt_mode_;
        structure_pending_ = false;
        structure_failed_.store(false);
        chapter_relayout_pending_ = false;
        if (!txt_mode_) {
            ClearLivePreview();
            pages_.swap(pending_pages_);
            page_index_ = pending_chapter_anchor_page_;
        }
        pending_structure_.reset();
        error_ = BookReadError::kNone;
        return;
    }

    if (IsChapteredBook()) {
        if (pending_pages_.empty()) {
            return;
        }
        ClearLivePreview();
        pages_.swap(pending_pages_);
        chapter_index_ = anchor.valid ? anchor.chapter : chapter_index_;
        const int count = std::max(1, PageCount());
        page_index_ = std::clamp(pending_chapter_anchor_page_, 0, count - 1);
        chapter_relayout_pending_ = false;
        return;
    }

}

bool BookSession::SeekTxtPreview(uint32_t target) {
    if (!EnsureTxtFile()) {
        return false;
    }
    const uint32_t begin = target > 4096 ? target - 4096 : 0;
    if (std::fseek(txt_fp_, begin, SEEK_SET) != 0) {
        return false;
    }
    uint8_t bytes[4096];
    const size_t got = std::fread(bytes, 1, target - begin, txt_fp_);
    if (got != target - begin) {
        error_ = BookReadError::kFileIo;
        return false;
    }
    uint32_t off = target;
    bool paragraph = target == 0;
    for (size_t i = 0; i < got; ++i) {
        if (bytes[i] == '\n') {
            off = begin + i + 1;
            paragraph = true;
        }
    }
    if (!paragraph && !txt_as_gb18030_ && got) {
        size_t i = got - 1;
        while (i > 0 && (bytes[i] & 0xC0) == 0x80) {
            --i;
        }
        const uint8_t lead = bytes[i];
        const size_t size = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (got - i < size) {
            off = begin + static_cast<uint32_t>(i);
        }
    }
    live_preview_hist_.clear();
    const bool ok = FillTxtLivePreview(off, paragraph);
    return ok;
}

const std::vector<TocEntry>& BookSession::TocEntries() const {
    return navigation_source_ && !structure_staging_ ? navigation_source_->TocEntries() : toc_;
}

int BookSession::TocCount() const {
    return static_cast<int>(TocEntries().size());
}

const char* BookSession::TocHomeTitle() const {
    const auto* document = navigation_source_ ? navigation_source_->epub_.get() : epub_.get();
    if (document) {
        std::string language = document->Language();
        for (char& c : language) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (language == "zh" || language.compare(0, 3, "zh-") == 0 || language == "zho" || language == "chi") {
            return LangBookHomeLabel(true);
        }
        if (language == "en" || language.compare(0, 3, "en-") == 0 || language == "eng") {
            return LangBookHomeLabel(false);
        }
    }
    unsigned chinese = 0, latin = 0;
    for (size_t entry = 0; entry < std::min<size_t>(TocEntries().size(), 16); ++entry) {
        const auto& title = TocEntries()[entry].title;
        for (size_t i = 0; i < std::min<size_t>(title.size(), 240);) {
            uint32_t cp = 0;
            const size_t count = Utf8Next(reinterpret_cast<const uint8_t*>(title.data()) + i, title.size() - i, &cp);
            if (!count) {
                break;
            }
            i += count;
            if ((cp >= 0x3400 && cp <= 0x9fff) || (cp >= 0x20000 && cp <= 0x323af)) {
                ++chinese;
            } else if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) {
                ++latin;
            }
        }
    }
    return LangBookHomeLabel(chinese > latin);
}

bool BookSession::TocStartsAfterFirstPage() const {
    return TocCount() > 0 && reading_chapters_ && !reading_chapters_->empty() &&
           reading_chapters_->front().toc_index < 0;
}

const TocEntry* BookSession::TocAt(int index) const {
    if (index < 0) {
        return nullptr;
    }
    if (static_cast<size_t>(index) >= TocEntries().size()) {
        return nullptr;
    }
    return &TocEntries()[static_cast<size_t>(index)];
}

int BookSession::CurrentTocIndex() const {
    if (structure_pending_) {
        return preview_toc_index_;
    }
    if (reading_chapters_ && chapter_index_ >= 0 && chapter_index_ < static_cast<int>(reading_chapters_->size())) {
        return (*reading_chapters_)[chapter_index_].toc_index;
    }
    return -1;
}

bool BookSession::CaptureBookmarkRange(BookSourcePosition& first, BookSourcePosition& end) const {
    if (!open_) {
        return false;
    }
    if (txt_mode_ && live_preview_active_) {
        first = {0, 0, live_preview_byte_off_};
        end = {0, 0,
               live_preview_has_next_ ? live_preview_next_off_
                                      : static_cast<uint32_t>(std::min<size_t>(info_.file_size, UINT32_MAX))};
        return SourceBefore(first, end);
    }
    if (live_preview_active_ && !txt_mode_) {
        bool found = false;
        for (const auto& item : live_preview_page_.items) {
            if (item.source_block == UINT32_MAX) {
                continue;
            }
            if (!found) {
                first = {item.source_chapter, item.source_block, item.source_begin};
                found = true;
            }
            end = {item.source_chapter, item.source_block, item.source_end};
        }
        return found && SourceBefore(first, end);
    }
    if (page_index_ < 0 || static_cast<size_t>(page_index_) >= pages_.size()) {
        return false;
    }
    bool found = false;
    for (const auto& item : pages_[page_index_].items) {
        if (item.source_block == UINT32_MAX) {
            continue;
        }
        if (!found) {
            first = {item.source_chapter, item.source_block, item.source_begin};
            found = true;
        }
        end = {item.source_chapter, item.source_block, item.source_end};
    }
    if (!found) {
        return false;
    }
    if (static_cast<size_t>(page_index_ + 1) < pages_.size()) {
        for (const auto& item : pages_[page_index_ + 1].items) {
            if (item.source_block != UINT32_MAX) {
                end = {item.source_chapter, item.source_block, item.source_begin};
                break;
            }
        }
    }
    return SourceBefore(first, end);
}

uint32_t BookSession::CurrentPageBookmarkId() const {
    BookSourcePosition first, end;
    if (!CaptureBookmarkRange(first, end)) {
        return 0;
    }
    for (const auto& entry : bookmarks_.Entries()) {
        if (SourceInPage(entry.start, first, end)) {
            return entry.id;
        }
    }
    return 0;
}

bool BookSession::CaptureCurrentPageBookmark(BookBookmark& entry, std::string& error) {
    try {
        entry = {};
        if (!CaptureBookmarkRange(entry.start, entry.end)) {
            error = UiBookmarkLayoutBusy();
            return false;
        }
        entry.page = std::max(0, CurrentPage());
        entry.added_at = std::time(nullptr);
        const auto* page = CurrentPageData();
        size_t characters = 0;
        if (page) {
            for (const auto& item : page->items) {
                if (item.kind != ContentKind::kText || item.chapter_title) {
                    continue;
                }
                if (!entry.excerpt.empty() && item.para_gap_before && characters < 150) {
                    entry.excerpt += '\n';
                    ++characters;
                }
                size_t i = 0;
                const size_t text_size = item.text.size() - (item.hyphenated && !item.text.empty() ? 1 : 0);
                while (i < text_size && characters < 150) {
                    const size_t n = Utf8Next(reinterpret_cast<const uint8_t*>(item.text.data() + i),
                                              text_size - i, nullptr);
                    if (!n || entry.excerpt.size() + n > BookBookmarkStore::kMaxExcerptBytes) {
                        break;
                    }
                    entry.excerpt.append(item.text, i, n);
                    i += n;
                    ++characters;
                }
                if (characters >= 150) {
                    break;
                }
            }
        }
        if (entry.excerpt.empty()) {
            entry.excerpt = CurrentTocTitle() + UiBookmarkImageSuffix();
        }
        if (entry.excerpt.size() > BookBookmarkStore::kMaxExcerptBytes) {
            entry.excerpt = UiBookmarkImage();
        }
        return true;
    } catch (const std::bad_alloc&) {
        error = UiBookmarkAddMemory();
        return false;
    }
}

bool BookSession::GoToBookmark(uint32_t id) {
    if (!bookmarks_.Error().empty()) {
        return false;
    }
    const auto& entries = bookmarks_.Entries();
    const auto found =
        std::find_if(entries.begin(), entries.end(), [id](const BookBookmark& entry) { return entry.id == id; });
    if (found == entries.end()) {
        return false;
    }
    return GoToBookmarkPosition(found->start);
}

bool BookSession::GoToBookmarkPosition(const BookSourcePosition& position) {
    if (!open_) {
        return false;
    }
    if (txt_mode_ && live_preview_active_) {
        if (position.offset >= info_.file_size || !EnsureTxtFile()) {
            return false;
        }
        bool paragraph = position.offset == 0;
        if (!paragraph) {
            if (std::fseek(txt_fp_, position.offset - 1, SEEK_SET) != 0) {
                error_ = BookReadError::kFileIo;
                return false;
            }
            const int previous = std::fgetc(txt_fp_);
            if (previous == EOF) {
                error_ = BookReadError::kFileIo;
                return false;
            }
            paragraph = previous == '\n' || previous == '\r';
        }
        live_preview_hist_.clear();
        return FillTxtLivePreview(position.offset, paragraph);
    }
    if (structure_pending_ && !txt_mode_) {
        LayoutAnchor target;
        target.valid = true;
        target.source_chapter = position.chapter;
        target.source_block = position.block;
        target.source_offset = position.offset;
        target.source_para_start = position.offset == 0;
        const bool ok = PrepareSourcePreview(target);
        if (ok) {
            source_preview_history_.clear();
            preview_toc_index_ = -1;
        }
        return ok;
    }
    if (live_preview_active_) {
        return false;
    }
    const int chapter = ReadingChapterForPosition(position);
    if (chapter < 0 || chapter >= chapter_count_) {
        return false;
    }
    const int original_chapter = chapter_index_, original_page = page_index_;
    const bool change_chapter = chapter != chapter_index_ || pages_.empty();
    BookPageStore original_pages;
    if (change_chapter) {
        original_pages = std::move(pages_);
        if (!(LoadChapter(chapter))) {
            pages_ = std::move(original_pages);
            chapter_index_ = original_chapter;
            page_index_ = original_page;
            return false;
        }
    }
    // 旧逻辑兼容：BKM1 使用原始正文区间定位；旧 page 展示字段不参与章节页码计算。
    const int previous_page = page_index_;
    for (size_t p = 0; p < pages_.size(); ++p) {
        page_index_ = p;
        BookSourcePosition first, end;
        if (CaptureBookmarkRange(first, end) &&
            (SourceInPage(position, first, end) || SourceBefore(position, first))) {
            page_index_ = previous_page;
            return GoToPage(p);
        }
    }
    if (info_.format == BookFormat::kEpub && position.block == 0 && position.offset < 64 && !pages_.empty()) {
        page_index_ = previous_page;
        return GoToPage(0);
    }
    if (change_chapter) {
        pages_ = std::move(original_pages);
    }
    chapter_index_ = original_chapter;
    page_index_ = original_page;
    return false;
}

bool BookSession::GoToToc(int index) {
    if (structure_pending_) {
        return ResolveTocPreview(index);
    }
    if (index < 0 || index >= TocCount() || !reading_chapters_) {
        return false;
    }
    const auto& positions = navigation_source_ ? navigation_source_->epub_toc_positions_ : epub_toc_positions_;
    if (info_.format == BookFormat::kEpub && static_cast<size_t>(index) < positions.size()) {
        const int chapter = ReadingChapterForPosition(positions[index]);
        return chapter >= 0 && LoadChapter(chapter);
    }
    for (size_t i = 0; i < reading_chapters_->size(); ++i) {
        if ((*reading_chapters_)[i].toc_index == index) {
            return LoadChapter(static_cast<int>(i));
        }
    }
    return false;
}

std::string BookSession::CurrentTocTitle() const {
    if (page_images_mode_) {
        char title[48];
        std::snprintf(title, sizeof(title), UiPageFmt(), chapter_index_ + 1);
        return title;
    }
    const int index = CurrentTocIndex();
    if (index >= 0 && static_cast<size_t>(index) < TocEntries().size()) {
        return TocEntries()[index].title;
    }
    return {};
}

bool BookSession::GoToReadingProgress(int progress_x10) {
    if (structure_pending_) {
        if (txt_mode_ && info_.file_size) {
            const uint32_t offset = static_cast<uint64_t>(info_.file_size - 1) * std::clamp(progress_x10, 0, 1000) / 1000;
            return SeekTxtPreview(offset);
        }
        if (progress_x10 == 0) {
            LayoutAnchor target;
            target.valid = true;
            target.source_chapter = info_.format == BookFormat::kEbook ? ebook_spine_.front() : 0;
            target.source_block = 0;
            return PrepareSourcePreview(target);
        }
        return false;
    }
    if (!open_ || !source_sizes_ || source_sizes_->empty()) {
        return false;
    }
    const uint64_t total = source_sizes_->back();
    const uint64_t target = (total ? total - 1 : 0) * std::clamp(progress_x10, 0, 1000) / 1000;
    if (txt_mode_) {
        return SeekTxtPreview(static_cast<uint32_t>(target));
    }
    int chapter = -1;
    for (size_t i = 0; i < reading_chapters_->size(); ++i) {
        const auto& range = (*reading_chapters_)[i];
        if (range.content_begin <= target) {
            chapter = static_cast<int>(i);
        }
        if (range.content_end > target) {
            break;
        }
    }
    if (chapter < 0 || !LoadChapter(chapter)) {
        return false;
    }
    int page = 0;
    for (size_t i = 0; i < pages_.size(); ++i) {
        for (const auto& item : pages_[i].items) {
            if (item.source_progress <= target) {
                page = static_cast<int>(i);
            } else {
                return pages_.Good() && GoToPage(page);
            }
        }
    }
    return pages_.Good() && GoToPage(page);
}

int BookSession::ReadingProgressX10() const {
    if (structure_pending_) {
        if (txt_mode_ && info_.file_size) {
            return !live_preview_has_next_ ? 1000 : static_cast<int>(
                std::min<uint64_t>(999, static_cast<uint64_t>(live_preview_byte_off_) * 1000 / info_.file_size));
        }
        return restored_progress_x10_;
    }
    if (!open_ || !source_sizes_ || source_sizes_->empty() || source_sizes_->back() == 0) {
        return 0;
    }
    if (txt_mode_) {
        return !live_preview_has_next_ ? 1000 : static_cast<int>(
            std::min<uint64_t>(999, static_cast<uint64_t>(live_preview_byte_off_) * 1000 / source_sizes_->back()));
    }
    if (chapter_index_ + 1 == chapter_count_ && page_index_ + 1 == PageCount()) {
        return 1000;
    }
    const auto* page = CurrentPageData();
    if (page && !page->items.empty()) {
        return static_cast<int>(std::min<uint64_t>(999, page->items.front().source_progress * 1000 / source_sizes_->back()));
    }
    return 0;
}
