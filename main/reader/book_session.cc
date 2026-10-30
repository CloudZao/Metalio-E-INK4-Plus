#include "book_session.h"

#include <algorithm>
#include <cerrno>
#include <climits>
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
#else
#include <thread>
#endif

#include "book_library.h"
#ifndef EBOOK_EMULATOR
#include "book_home_snapshot.h"
#include "application.h"
#include "sd_access.h"
#include <deque>
#include "book_progress_cache.h"
#endif
#include "image_util.h"
#include "image_geometry.h"
#include "text_encoding.h"
#include "txt_chapter.h"
#include "vertical_text.h"
#ifndef EBOOK_EMULATOR
#include "assets/lang_config.h"
#include "assets/lang_reader.h"
#include "fontpack_lvgl.h"
#endif


namespace reader {
namespace {

#ifndef EBOOK_EMULATOR
const char* UiBlank() { return Lang::Strings::BOOK_BLANK; }
const char* UiReadFileFail() { return Lang::Strings::BOOK_READ_FILE_FAIL; }
const char* UiChapterNoText() { return Lang::Strings::BOOK_CHAPTER_NO_TEXT; }
const char* UiChapterFmt() { return Lang::Strings::BOOK_CHAPTER_FMT; }
const char* UiPageFmt() { return Lang::Strings::BOOK_PAGE_FMT; }
const char* UiBookmarkLayoutBusy() { return Lang::Strings::BOOK_BOOKMARK_LAYOUT_BUSY; }
const char* UiBookmarkAddMemory() { return Lang::Strings::BOOK_BOOKMARK_ADD_MEMORY; }
const char* UiBookmarkImageSuffix() { return Lang::Strings::BOOK_BOOKMARK_IMAGE_SUFFIX; }
const char* UiBookmarkImage() { return Lang::Strings::BOOK_BOOKMARK_IMAGE; }
#else
const char* UiBlank() { return "(Blank)"; }
const char* UiReadFileFail() { return "(Read failed)"; }
const char* UiChapterNoText() { return "(No text in chapter)"; }
const char* UiChapterFmt() { return "Ch.%d"; }
const char* UiPageFmt() { return "Page %d"; }
const char* UiBookmarkLayoutBusy() { return "Laying out text. Add the bookmark shortly."; }
const char* UiBookmarkAddMemory() { return "Not enough memory to add a bookmark"; }
const char* UiBookmarkImageSuffix() { return " (Image or chapter page)"; }
const char* UiBookmarkImage() { return "Image or chapter page"; }
#endif

constexpr const char* TAG = "BookSession";
std::atomic<uint32_t> s_txt_work_id{0}; // 区分异步回收期间重叠的 TXT 会话工作文件
constexpr size_t kMaxTxtBytes = static_cast<size_t>(INT32_MAX) - 1; // 源偏移和 fseek 的表示边界
constexpr size_t kTxtReadChunk = 1024;
constexpr uint32_t kMaxLineSourceBytes = 512;
constexpr uint32_t kTxtIdxVersion = 12; // 分页计入行框、下划线及行间距的完整行占用
constexpr uint16_t kTxtLineParaStart = 0x8000u;
constexpr uint16_t kTxtLineLenMask = 0x7FFFu;
constexpr size_t kMaxTocEntries = 8000;
// 单章解压合计上限：与 tools/ebook/epdbook/format.py DEVICE_CHAPTER_MAX_UNCOMP 一致。
// 超限拒绝加载；分页 OOM 软失败。转换默认拆到 DEFAULT_CHAPTER_MAX_BYTES(256KB)。

#pragma pack(push, 1)
struct TxtIdxHeader {
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

bool FlushLineToPage(std::string& line, BookPageStore& pages, Page& cur, lv_coord_t& y,
                     const TextLineLayout& layout, int& row, lv_coord_t gap_before,
                     bool para_indent = false, bool chapter_title = false,
                     bool para_gap_before = false, uint32_t source_block = UINT32_MAX,
                     uint32_t source_begin = 0, uint32_t source_end = 0) {
    if (line.empty()) {
        return true;
    }
    if (layout.count <= 0) return false;
    if ((row >= layout.count || y + gap_before + layout.CellHeight(row) > layout.extent) && !cur.items.empty()) {
        if (!pages.Push(std::move(cur))) return false;
        cur = Page{}; y = 0; row = 0;
    }
    if (cur.items.empty()) gap_before = 0;
    try {
        PageItem item;
        item.source_block=source_block;
        item.source_begin=source_begin;item.source_end=source_end;
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
    y += gap_before + layout.CellHeight(row++);
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

// 过滤缺少正文且后续重复出现的密集目录；保留不重复的卷章层级和短章节。
void FilterDenseTxtToc(std::vector<TocEntry>& entries,
                       const std::vector<uint32_t>& title_end_chars, uint32_t total_chars) {
    auto body_chars = [&](size_t index) {
        if (index + 1 == entries.size()) return total_chars - title_end_chars[index];
        uint32_t next_title_chars = 0;
        const auto& title = entries[index + 1].title;
        for (size_t i = 0; i < title.size();) {
            uint32_t cp = 0;
            i += Utf8Next(reinterpret_cast<const uint8_t*>(title.data()) + i, title.size() - i, &cp);
            if (!IsTxtParaIndentWs(cp)) ++next_title_chars;
        }
        return title_end_chars[index + 1] - title_end_chars[index] - next_title_chars;
    };
    auto repeated_later = [&](size_t index, size_t begin) {
        std::string ordinal, name;
        const bool numbered = SplitChapterHeading(entries[index].title, ordinal, name);
        for (size_t i = begin; i < entries.size(); ++i) {
            if (!numbered) {
                if (entries[i].title == entries[index].title) return true;
            } else {
                std::string next_ordinal, next_name;
                if (SplitChapterHeading(entries[i].title, next_ordinal, next_name) &&
                    next_ordinal == ordinal) return true;
            }
        }
        return false;
    };
    size_t write = 0;
    for (size_t read = 0; read < entries.size();) {
        size_t end = read;
        while (end < entries.size() && body_chars(end) == 0) ++end;
        if (end - read >= 2 && end < entries.size()) {
            bool repeated = true;
            for (size_t i = read; repeated && i < end; ++i) {
                repeated = repeated_later(i, end);
            }
            if (repeated) {
                // 末项可能带有卷名等短附文，仍须确认后文存在对应章首。
                read = end;
                if (body_chars(end) < 80 && repeated_later(end, end + 1)) ++read;
                continue;
            }
        }
        if (write != read) entries[write] = std::move(entries[read]);
        ++write;
        ++read;
    }
    entries.resize(write);
}

// 带回填缓冲的源文件字节流，按码点推进并记录绝对文件偏移。
// 读缓冲必须在堆上：与 StreamPaginateTxt 栈上溢出变量相邻时，1KB 栈 buf 会被编译器
// 与局部/lambda 状态重叠，fread 满缓冲即踩坏分页状态（表现为 epdfont intervals UAF/空指针）。
struct TxtByteStream {
    FILE* fp = nullptr;
    bool as_gb18030 = false;
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

    bool Seek(uint32_t offset) {
        if (offset >= file_base && offset <= file_base + end) {
            pos = offset - file_base;
            return true;
        }
        if (std::fseek(fp, static_cast<long>(offset), SEEK_SET) != 0) return false;
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

}  // namespace

void BookSession::ClearTxtIndex() {
    if (txt_fp_ != nullptr) {
        std::fclose(txt_fp_);
        txt_fp_ = nullptr;
    }
    txt_lines_.Reset();
    txt_lines_len_ = 0;
    txt_lines_cap_ = 0;
    txt_page_first_line_.Reset();
    txt_page_count_ = 0;
    txt_page_starts_cap_ = 0;
    txt_view_page_ = Page{};
    txt_view_index_ = -1;
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

bool BookSession::GrowTxtLines(size_t need) {
    if (need <= txt_lines_cap_) return txt_lines_.Good();
    const bool ok = txt_lines_.Reserve(info_.path + ".rlines-" + std::to_string(txt_work_id_) + ".tmp", need);
    if (!ok) error_ = txt_lines_.Error();
    if (ok) txt_lines_cap_ = std::max(txt_lines_cap_, need);
    return ok;
}

bool BookSession::GrowTxtPageStarts(size_t need) {
    if (need <= txt_page_starts_cap_) return txt_page_first_line_.Good();
    const bool ok = txt_page_first_line_.Reserve(info_.path + ".rindex-" + std::to_string(txt_work_id_) + ".tmp", need);
    if (!ok && error_ == BookReadError::kNone) error_ = BookReadError::kCacheIo;
    if (ok) txt_page_starts_cap_ = std::max(txt_page_starts_cap_, need);
    return ok;
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
    uint32_t anchor_magic, txt_anchor, txt_para; // POS3 可选扩展：TXT 字节锚点
};
#pragma pack(pop)
static_assert(sizeof(Pos3Blob) == 40, "POS3 plus TXT anchor size");

void LogProgressFailure(const char* stage, const char* path) {
    static std::atomic<int64_t> last_us{0};
    static std::atomic<unsigned> suppressed{0};
    const auto now = esp_timer_get_time();
    auto previous = last_us.load();
    if ((previous == 0 || now - previous >= 5000000) && last_us.compare_exchange_strong(previous, now)) {
        ESP_LOGE(TAG, "progress %s failed errno=%d suppressed=%u path=%s", stage, errno,
                 suppressed.exchange(0), path);
    } else ++suppressed;
}

#ifndef EBOOK_EMULATOR
struct ProgressWrite {
    sd_access::DeferredTransaction access; // 排队期间阻止卸卡
    std::string path, book;
    Pos3Blob blob{};
};
std::mutex progress_write_mu;
std::deque<std::unique_ptr<ProgressWrite>> progress_writes;
ProgressWrite* progress_inflight = nullptr;
bool progress_writer_running = false;

void DrainProgressWrites() {
    for (;;) {
        std::unique_ptr<ProgressWrite> job;
        {
            std::lock_guard<std::mutex> lock(progress_write_mu);
            if (progress_writes.empty()) { progress_writer_running = false; return; }
            job = std::move(progress_writes.front());
            progress_writes.pop_front();
            progress_inflight = job.get();
        }
        sd_access::Transaction transaction(job->access);
        const bool ok = transaction && WriteFileAtomic(job->path.c_str(),
            reinterpret_cast<const uint8_t*>(&job->blob), sizeof(job->blob));
        if (ok) {
            book_home_snapshot::NoteOpenedBook(job->book.c_str());
            BookSession::ProgressPeek snapshot;
            snapshot.progress_x10 = job->blob.pct_x10;
            snapshot.reading_seconds = job->blob.reading_seconds;
            snapshot.daily_seconds = job->blob.daily_seconds;
            book_progress_cache::Upsert(job->book.c_str(), snapshot);
        } else {
            LogProgressFailure("write", job->path.c_str());
        }
        {
            std::lock_guard<std::mutex> lock(progress_write_mu);
            progress_inflight = nullptr;
        }
    }
}

bool QueueProgressWrite(const std::string& path, const std::string& book, const Pos3Blob& blob) try {
    auto job = std::make_unique<ProgressWrite>();
    if (!job->access) { errno = EBUSY; return false; }
    job->path = path; job->book = book; job->blob = blob;
    std::lock_guard<std::mutex> lock(progress_write_mu);
    for (auto& queued : progress_writes) {
        if (queued->path == path) { queued->blob = blob; return true; }
    }
    if (progress_writes.size() >= 8) { errno = ENOBUFS; return false; }
    progress_writes.push_back(std::move(job));
    if (!progress_writer_running) {
        progress_writer_running = true;
        if (!Application::GetInstance().Schedule(DrainProgressWrites, Application::TaskDomain::Storage)) {
            progress_writer_running = false;
            progress_writes.pop_back();
            errno = EAGAIN;
            return false;
        }
    }
    return true;
} catch (const std::bad_alloc&) { errno = ENOMEM; return false; }

bool ReadQueuedProgress(const char* path, Pos3Blob& blob) {
    std::lock_guard<std::mutex> lock(progress_write_mu);
    for (auto it = progress_writes.rbegin(); it != progress_writes.rend(); ++it) {
        if ((*it)->path == path) { blob = (*it)->blob; return true; }
    }
    if (progress_inflight && progress_inflight->path == path) { blob = progress_inflight->blob; return true; }
    return false;
}
#endif

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
    uint32_t marker = 0, para = 0;
    if (out.has_daily && std::fread(&marker, sizeof(marker), 1, fp) == 1 && marker == 0x31585442 &&
        std::fread(&out.txt_anchor, sizeof(out.txt_anchor), 1, fp) == 1 &&
        std::fread(&para, sizeof(para), 1, fp) == 1 && para <= 1) {
        out.has_txt_anchor = true; out.txt_para_start = para != 0;
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
    Pos3Blob queued{};
    if (pos_path && ReadQueuedProgress(pos_path, queued)) {
        out = {};
        std::memcpy(out.magic, queued.magic, sizeof(out.magic));
        out.chapter = queued.chapter; out.page = queued.page; out.pct_x10 = queued.pct_x10;
        out.reading_seconds = queued.reading_seconds; out.day_id = queued.day_id; out.daily_seconds = queued.daily_seconds;
        out.has_pct = out.has_seconds = out.has_daily = true;
        out.has_txt_anchor = queued.anchor_magic == 0x31585442;
        out.txt_anchor = queued.txt_anchor; out.txt_para_start = queued.txt_para != 0;
        return PosLoadStatus::kOk;
    }
#endif

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
            // 未对时仍累计时长；首页快照保存待归属秒数。
            AddReadingSecondsSaturated(sec);
        }
#ifndef EBOOK_EMULATOR
        book_home_snapshot::AddDailyReadingSeconds(today, sec);
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
    if (txt_mode_) {
        const auto anchor = CaptureLayoutAnchor();
        blob.anchor_magic = 0x31585442; blob.txt_anchor = anchor.txt_byte_off;
        blob.txt_para = anchor.txt_at_para_start;
    }

#ifndef EBOOK_EMULATOR
    const bool ok = QueueProgressWrite(path, info_.path, blob);
#else
    const bool ok = WriteFileAtomic(path.c_str(), reinterpret_cast<const uint8_t*>(&blob), sizeof(blob));
#endif
    if (!ok) LogProgressFailure("enqueue", path.c_str());
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

lv_coord_t BookSession::LayoutLineHeight() const {
    return FontLineHeight(font_);
}
TextLineLayout BookSession::LineLayout() const {
    return MakeTextLineLayout(font_, viewport_h_, line_spacing_percent_, vertical_writing_);
}
lv_coord_t BookSession::LayoutGlyphWidth(uint32_t cp) const {
    return vertical_writing_ ? VerticalAdvance(cp, LayoutLineHeight()) : GlyphWidth(font_, cp);
}
lv_coord_t BookSession::LayoutIndentWidth() const {
    return vertical_writing_
        ? std::min<lv_coord_t>(2 * LayoutLineHeight(), std::max<lv_coord_t>(0, viewport_w_ - LayoutLineHeight()))
        : TxtParaIndentWidth(font_);
}

uint32_t BookSession::FontFingerprint() const {
    // 行高 + 若干样例字宽：换字体/字号后自动失效缓存
    uint32_t h = static_cast<uint32_t>(LayoutLineHeight());
    h = h * 131u + static_cast<uint32_t>(LineLayout().count);
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
    if (vertical_writing_) h = h * 131u + 0x56525434u;
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
        hdr.line_spacing_percent != static_cast<uint16_t>(line_spacing_percent_) ||
        hdr.para_gap != static_cast<uint16_t>(para_gap_) ||
        hdr.font_fp != FontFingerprint() ||
        hdr.as_gb18030 != static_cast<uint8_t>(txt_as_gb18030_ ? 1 : 0) ||
        hdr.reserved[0] != 0 ||
        hdr.line_count == 0 || hdr.page_count == 0) {
        std::fclose(ifp);
        ESP_LOGI(TAG, "txt idx stale, will rebuild: %s", idx_path.c_str());
        return false;
    }
    // 合理上限，防止坏文件拖垮内存
    if (hdr.line_count > file_size + 1 || hdr.page_count > hdr.line_count) {
        std::fclose(ifp);
        ESP_LOGW(TAG, "txt idx absurd counts L=%u P=%u", static_cast<unsigned>(hdr.line_count),
                 static_cast<unsigned>(hdr.page_count));
        return false;
    }

    auto fail_cleanup = [&]() {
        std::fclose(ifp);
        txt_lines_.Reset();
        txt_page_first_line_.Reset();
        txt_lines_cap_ = txt_page_starts_cap_ = 0;
        txt_lines_len_ = 0;
        txt_page_count_ = 0;
        ClearToc();
    };

    static_assert(sizeof(TxtLineRef) == sizeof(TxtIdxLineDisk), "TXT index record layout mismatch");
    const uint64_t pages_offset = sizeof(hdr) + static_cast<uint64_t>(hdr.line_count) * sizeof(TxtIdxLineDisk);
    const uint64_t toc_offset = pages_offset + (static_cast<uint64_t>(hdr.page_count) + 1) * sizeof(uint32_t);
    if (toc_offset > static_cast<uint64_t>(LONG_MAX) ||
        !txt_lines_.OpenReadOnly(idx_path, sizeof(hdr), hdr.line_count) ||
        !txt_page_first_line_.OpenReadOnly(idx_path, static_cast<size_t>(pages_offset), static_cast<size_t>(hdr.page_count) + 1) ||
        std::fseek(ifp, static_cast<long>(toc_offset), SEEK_SET) != 0) {
        fail_cleanup();
        return false;
    }
    txt_lines_len_ = hdr.line_count;

    // 校验哨兵
    if (txt_page_first_line_[0] != 0 ||
        txt_page_first_line_[hdr.page_count] != hdr.line_count || !txt_page_first_line_.Good()) {
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
            std::fread(&title_len, sizeof(title_len), 1, ifp) != 1 || title_len > kMaxTxtChapterTitleBytes) {
            fail_cleanup();
            return false;
        }
        if (first_line >= hdr.line_count || (!toc_.empty() && first_line < toc_.back().first_line)) {
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
        ent.first_byte_off = txt_lines_[first_line].off;
        toc_.push_back(std::move(ent));
    }
    FinalizeTocPages();
    if (!txt_page_first_line_.Good()) { fail_cleanup(); return false; }

    std::fclose(ifp);
    ESP_LOGI(TAG, "txt idx hit %s lines=%u pages=%u toc=%u", idx_path.c_str(),
             static_cast<unsigned>(txt_lines_len_), static_cast<unsigned>(txt_page_count_),
             static_cast<unsigned>(toc_.size()));
    return true;
}

bool BookSession::SaveTxtIndex(const char* txt_path, size_t file_size, const std::vector<TocEntry>& entries,
                               uint32_t generation) const {
    if (txt_path == nullptr || !txt_lines_.IsOpen() || !txt_page_first_line_.IsOpen() ||
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
    hdr.line_spacing_percent = static_cast<uint16_t>(line_spacing_percent_);
    hdr.para_gap = static_cast<uint16_t>(para_gap_);
    hdr.font_fp = FontFingerprint();
    hdr.as_gb18030 = txt_as_gb18030_ ? 1 : 0;
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
        if ((i & 0x1FFu) == 0) {
            if (!WaitTxtPaginate(generation)) { fail_write(); return false; }
            #ifndef EBOOK_EMULATOR
            vTaskDelay(1);
            #endif
        }
        TxtIdxLineDisk rec{txt_lines_[i].off, txt_lines_[i].len};
        if (std::fwrite(&rec, 1, sizeof(rec), ofp) != sizeof(rec)) {
            fail_write();
            return false;
        }
    }
    for (size_t i = 0; i <= txt_page_count_; ++i) {
        if ((i & 0x1FFu) == 0 && !WaitTxtPaginate(generation)) { fail_write(); return false; }
        const uint32_t value = txt_page_first_line_[i];
        if (std::fwrite(&value, sizeof(value), 1, ofp) != 1) { fail_write(); return false; }
    }
    if (!txt_lines_.Good() || !txt_page_first_line_.Good()) { fail_write(); return false; }
    const uint32_t toc_count = static_cast<uint32_t>(entries.size());
    if (std::fwrite(&toc_count, sizeof(toc_count), 1, ofp) != 1) {
        fail_write();
        return false;
    }
    for (const auto& ent : entries) {
        if (!WaitTxtPaginate(generation)) { fail_write(); return false; }
        uint16_t title_len = static_cast<uint16_t>(
            ent.title.size() > kMaxTxtChapterTitleBytes ? kMaxTxtChapterTitleBytes : ent.title.size());
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

bool BookSession::CloseSdHandles() {
    bool ok = txt_lines_.Detach();
    if (!txt_page_first_line_.Detach()) ok = false;
    if (!pages_.Detach()) ok = false;
    if (!pending_pages_.Detach()) ok = false;
    if (txt_fp_ != nullptr) {
        if (std::fclose(txt_fp_) != 0) ok = false;
        txt_fp_ = nullptr;
    }
    if (!ok) error_ = BookReadError::kCacheIo;
    return ok;
}

void BookSession::DetachSdHandles() {
    ClearPageImageCache();
    sd_handles_detached_ = true;
    CloseSdHandles();
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

bool BookSession::PrepareClose() {
    if (close_progress_queued_) return true;
    StopReadingClock();
    if (open_ && !SaveProgress()) return false;
    close_progress_queued_ = true;
    return true;
}

void BookSession::Close() {
    if (open_ && !close_progress_queued_) {
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
    ResumeTxtPaginate();
    ClearToc();
    pages_.clear();
    pending_pages_.clear();
    pending_toc_.clear(); pending_toc_complete_ = false;
    pending_txt_bytes_ = pending_txt_pages_ = txt_indexed_bytes_ = txt_indexed_pages_ = 0;
    pending_chapter_counts_.clear(); completed_chapter_counts_.clear();
    page_index_ = 0;
    chapter_index_ = 0;
    chapter_count_ = 1;
    chapter_page_counts_.clear();
    chapter_page_counts_.shrink_to_fit();
    chapter_scan_counts_.clear();
    chapter_scan_counts_.shrink_to_fit();
    chapter_scan_reset_ = true;
    chapter_pages_ready_ = false;
    chapter_relayout_pending_ = false;
    chapter_pages_gen_ = 0;
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
    info_ = {};
    display_title_.clear();
    display_author_.clear();
}

bool BookSession::Open(const BookInfo& info) {
    error_ = BookReadError::kInvalidData;
    try {
        if (OpenImpl(info)) { error_ = BookReadError::kNone; return true; }
    } catch (const std::bad_alloc&) { error_ = BookReadError::kNoMemory; Close(); }
    return false;
}

bool BookSession::OpenImpl(const BookInfo& info) {
    Close();
    close_progress_queued_ = false;
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
        error_ = BookReadError::kUnsupported;
        ESP_LOGE(TAG, "unknown format");
    }
    open_ = ok;
    if (!ok) {
        Close();
        return false;
    }
    ReportOpenProgress(95);
    struct stat bookmark_stat{};
    const bool have_bookmark_stat=stat(info.path.c_str(),&bookmark_stat)==0;
    if(have_bookmark_stat) { info_.file_size=bookmark_stat.st_size;info_.mtime=bookmark_stat.st_mtime; }
    bookmarks_.Load(info.path,have_bookmark_stat?bookmark_stat.st_size:info.file_size,
                    have_bookmark_stat?bookmark_stat.st_mtime:info.mtime,static_cast<uint32_t>(info.format));
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
        if (epub_ || ebook_) loaded = LoadChapter(ch);
        if (!loaded) {
            Close();
            return false;
        }
        // 全书页表：打开只读 .cpg；未命中则估算，后台 FinishChapterPageTable 补齐
        WarmChapterPageTableFromCache();
    }
    if (has_prog && !live_preview_active_) {
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
    if (live_preview_active_ && txt_mode_) {
        uint64_t pages = 1;
        if (txt_indexed_bytes_ && txt_indexed_pages_) {
            pages = (static_cast<uint64_t>(txt_indexed_pages_) * info_.file_size + txt_indexed_bytes_ - 1) / txt_indexed_bytes_;
        } else if (live_preview_next_off_ > live_preview_byte_off_) {
            const uint32_t bytes = live_preview_next_off_ - live_preview_byte_off_;
            pages = (info_.file_size + bytes - 1) / bytes;
        }
        return static_cast<int>(std::min<uint64_t>(INT32_MAX, std::max<uint64_t>(CurrentPage() + 1, pages)));
    }
    if (txt_mode_) {
        return static_cast<int>(txt_page_count_);
    }
    return static_cast<int>(pages_.size());
}

int BookSession::CurrentPage() const {
    if (!live_preview_active_ || !txt_mode_) return page_index_;
    if (txt_indexed_pages_ && indexed_anchor_ == live_preview_byte_off_ && live_preview_byte_off_ <= txt_indexed_bytes_)
        return static_cast<int>(indexed_anchor_page_);
    const uint32_t bytes = txt_indexed_bytes_ ? txt_indexed_bytes_ : live_preview_next_off_ - live_preview_byte_off_;
    const uint32_t pages = txt_indexed_bytes_ ? txt_indexed_pages_ : 1;
    return bytes ? static_cast<int>(std::min<uint64_t>(INT32_MAX - 1, static_cast<uint64_t>(live_preview_byte_off_) * pages / bytes)) : 0;
}

int BookSession::BookPageCount() const {
    if (!IsChapteredBook() || chapter_count_ <= 1) {
        return std::max(1, PageCount());
    }
    if (chapter_page_counts_.size() == static_cast<size_t>(chapter_count_)) {
        int64_t sum = 0;
        for (uint32_t n : chapter_page_counts_) {
            sum += n ? n : std::max(1, PageCount());
        }
        if (sum > 0) {
            return static_cast<int>(std::min<int64_t>(sum, INT32_MAX));
        }
    }
    // 未就绪：沿用「本章页数 × 章数」估算
    return static_cast<int>(std::min<int64_t>(static_cast<int64_t>(std::max(1, ChapterCount())) * std::max(1, PageCount()), INT32_MAX));
}

int BookSession::BookCurrentPage() const {
    if (!IsChapteredBook() || chapter_count_ <= 1) {
        return CurrentPage();
    }
    if (chapter_page_counts_.size() != static_cast<size_t>(chapter_count_)) {
        // 估算：假定每章页数=本章
        const int pc = std::max(1, PageCount());
        return static_cast<int>(std::min<int64_t>(static_cast<int64_t>(chapter_index_) * pc + CurrentPage(), INT32_MAX));
    }
    int64_t before = 0;
    const int ch = std::clamp(chapter_index_, 0, chapter_count_ - 1);
    for (int i = 0; i < ch; ++i) {
        before += chapter_page_counts_[static_cast<size_t>(i)] ? chapter_page_counts_[static_cast<size_t>(i)] : std::max(1, PageCount());
    }
    return static_cast<int>(std::min<int64_t>(before + CurrentPage(), INT32_MAX));
}

void BookSession::MaterializeTxtPage(int page) const {
    txt_view_page_.items.clear();
    if (!txt_mode_ || page < 0 || static_cast<size_t>(page) >= txt_page_count_ ||
        !txt_page_first_line_.IsOpen() || !txt_lines_.IsOpen()) {
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
    if (end - begin > 2048) { error_ = BookReadError::kCacheIo; return; }
    txt_view_page_.items.reserve(end - begin);

    auto read_line_utf8 = [&](uint32_t idx, std::string& out) -> bool {
        const TxtLineRef lr = txt_lines_[idx];
        const uint16_t raw_len = static_cast<uint16_t>(lr.len & kTxtLineLenMask);
        out.clear();
        if (raw_len > kMaxLineSourceBytes) { error_ = BookReadError::kCacheIo; return false; }
        if (raw_len == 0) {
            return true;
        }
        if (std::fseek(txt_fp_, static_cast<long>(lr.off), SEEK_SET) != 0) {
            out = "?";
            return false;
        }
        uint8_t raw[kMaxLineSourceBytes + 4];
        const size_t n = std::fread(raw, 1, raw_len, txt_fp_);
        if (!BytesToUtf8(raw, n, txt_as_gb18030_, out)) {
            out = "?";
            return false;
        }
        return true;
    };

    // 跨页续行：若本页首行非段首，回看段首是否章名
    bool in_chap_title = false;
    if (begin > 0) {
        uint32_t j = begin;
        while (j > 0 && begin - j < kMaxTxtChapterTitleBytes && (txt_lines_[j].len & kTxtLineParaStart) == 0) {
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
        const TxtLineRef lr = txt_lines_[i];
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
                while (!vertical_writing_ && i + 1 < end && (txt_lines_[i + 1].len & kTxtLineParaStart) == 0) {
                    ++i;
                    std::string cont;
                    if (!read_line_utf8(i, cont) || cont.empty()) {
                        continue;
                    }
                    item.text += cont;
                }
                if (!vertical_writing_) item.text = FormatChapterHeadingDisplay(item.text);
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
            while (!vertical_writing_ && i + 1 < end && (txt_lines_[i + 1].len & kTxtLineParaStart) == 0) {
                ++i;
                std::string cont;
                if (!read_line_utf8(i, cont) || cont.empty()) {
                    continue;
                }
                item.text += cont;
            }
            if (!vertical_writing_) item.text = FormatChapterHeadingDisplay(item.text);
        }
        txt_view_page_.items.push_back(std::move(item));
    }
    txt_view_index_ = page;
}

bool BookSession::StreamPaginateTxt(FILE* fp, size_t file_size) {
    const uint32_t paginate_gen = txt_paginate_gen_;
    if (!WaitTxtPaginate(paginate_gen)) return false;
    uint32_t next_publish = 0;
    txt_lines_.Reset();
    txt_page_first_line_.Reset();
    txt_lines_cap_ = txt_page_starts_cap_ = 0;
    txt_lines_len_ = 0;
    txt_page_count_ = 0;
    txt_view_index_ = -1;
    txt_view_page_ = Page{};
    // 编排中保留旧 toc_ 供底栏/目录；新目录写入临时表，结束时再替换
    std::vector<TocEntry> built_toc;
    size_t published_toc = 0;
    std::vector<uint32_t> title_end_chars;
    uint32_t content_chars = 0;
    uint32_t counted_to = 0;
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
    const auto& page_starts = txt_page_first_line_;
    const auto& line_refs = txt_lines_;
    auto publish_toc = [&]() {
        std::lock_guard<std::mutex> lock(layout_progress_mu_);
        while (published_toc < built_toc.size() && pending_toc_.size() < 32)
            pending_toc_.push_back(built_toc[published_toc++]);
    };
    auto push_chapter = [&](std::string title, uint32_t line) {
        if (!TryPushTxtChapter(std::move(title), line)) return false;
        title_end_chars.push_back(content_chars);
        uint32_t lo = 0, hi = static_cast<uint32_t>(page_starts_len - 1);
        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo + 1) / 2;
            if (page_starts[mid] <= line) lo = mid;
            else hi = mid - 1;
        }
        built_toc.back().first_page = lo;
        publish_toc();
        return true;
    };

    const lv_coord_t indent_w = LayoutIndentWidth();
    lv_coord_t y = 0;
    VerticalBreakState breaks;
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

    const auto layout = LineLayout();
    if (layout.count <= 0) { error_ = BookReadError::kUnsupported; return false; }
    int row = 0;
    auto flush_line = [&](uint32_t next_off) -> bool {
        breaks = {};
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
            row = 0;
            ESP_LOGI(TAG, "chap txt page-break before '%.*s'", 48, para_title_acc.c_str());
        }
        if ((row >= layout.count || y + layout.CellHeight(row) > viewport_h_) &&
            txt_lines_len_ > txt_page_first_line_[page_starts_len - 1]) {
            if (!push_page_start(static_cast<uint32_t>(txt_lines_len_))) {
                return false;
            }
            y = 0;
            row = 0;
        }
        if (!GrowTxtLines(txt_lines_len_ + 1)) {
            return false;
        }
        uint16_t store_len = line_len > kTxtLineLenMask ? kTxtLineLenMask : static_cast<uint16_t>(line_len);
        if (line_para_start && store_len > 0) {
            store_len = static_cast<uint16_t>(store_len | kTxtLineParaStart);
        }
        txt_lines_[txt_lines_len_++] = TxtLineRef{line_off, store_len};
        y += layout.CellHeight(row++);
        line_off = next_off;
        line_len = 0;
        line_w = 0;
        line_started = false;
        line_para_start = false;
        return true;
    };

    TxtByteStream stream;
    if (!stream.InitBuf()) {
        error_ = BookReadError::kNoMemory;
        ESP_LOGE(TAG, "txt stream buf OOM int=%u spiram=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        return false;
    }
    stream.fp = fp;
    stream.as_gb18030 = txt_as_gb18030_;
    std::rewind(fp);
    stream.SkipBomIfAny();

    uint32_t cp = 0;
    uint32_t goff = 0;
    uint16_t glen = 0;
    bool is_nl = false;
    size_t glyph_n = 0;
    while (stream.NextGlyph(&cp, &goff, &glen, &is_nl)) {
        if (goff >= counted_to) {
            if (!is_nl && !IsTxtParaIndentWs(cp)) ++content_chars;
            counted_to = goff + glen;
        }
        if (!WaitTxtPaginate(paginate_gen)) return false;
        if ((++glyph_n & 0x1FFu) == 0u) {
            publish_toc();
            #ifndef EBOOK_EMULATOR
            // 全量扫描让出时间片，避免饿死 LVGL / 设置卡交互
            vTaskDelay(1);
            #endif
        }
        if (goff >= next_publish && txt_lines_len_ > 0) {
            const uint32_t anchor = txt_preview_anchor_.load();
            uint32_t lo = 0, hi = static_cast<uint32_t>(page_starts_len - 1);
            while (anchor > 0 && anchor <= goff && lo < hi) {
                const uint32_t mid = lo + (hi - lo + 1) / 2;
                const uint32_t line = page_starts[mid];
                if (line < txt_lines_len_ && line_refs[line].off <= anchor) lo = mid;
                else hi = mid - 1;
            }
            std::lock_guard<std::mutex> lock(layout_progress_mu_);
            pending_txt_bytes_ = goff;
            pending_txt_pages_ = static_cast<uint32_t>(page_starts_len - 1);
            pending_anchor_ = anchor; pending_anchor_page_ = lo;
            next_publish = goff + 64 * 1024;
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
                    prev_para_was_chap = push_chapter(std::move(para_title_acc), chap_line);
                    // 展示拆成两行时，源文若只占一行则补一行高度
                    if (prev_para_was_chap && !vertical_writing_) {
                        std::string ord;
                        std::string name;
                        if (SplitChapterHeading(chap_title, ord, name) && !name.empty()) {
                            const uint32_t nlines =
                                static_cast<uint32_t>(txt_lines_len_) - chap_line;
                            if (nlines < 2) {
                                const lv_coord_t stride = layout.CellHeight(row);
                                if (row < layout.count && y + stride <= viewport_h_) {
                                    y += stride; ++row;
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

        const lv_coord_t cw = LayoutGlyphWidth(cp);
        VerticalBreakState candidate = breaks;
        if (vertical_writing_) candidate.Observe(cp, goff, para_title_acc.size());
        if (line_started && (line_w + cw > viewport_w_ || line_len + glen > kMaxLineSourceBytes)) {
            if (vertical_writing_ && candidate.valid && candidate.offset < goff) {
                const uint32_t cut = candidate.offset;
                line_len = cut - line_off;
                para_title_acc.resize(candidate.text_size);
                if (!stream.Seek(cut)) { error_ = BookReadError::kFileIo; return false; }
                if (!flush_line(cut)) return false;
                continue;
            }
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
                    ReaderParagraphGap(para_gap_, prev_para_was_chap);
                prev_para_was_chap = false;
                if (y > 0) {
                    if ((row >= layout.count || y + gap + layout.CellHeight(row) > viewport_h_) &&
                        txt_lines_len_ > txt_page_first_line_[page_starts_len - 1]) {
                        if (!push_page_start(static_cast<uint32_t>(txt_lines_len_))) {
                            return false;
                        }
                        y = 0;
                        row = 0;
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
        if (vertical_writing_) breaks.Observe(cp, goff, para_title_acc.size());
        line_len += glen;
        line_w += cw;
        // 顶格段累加 UTF-8 供章名识别，禁则回退时同步截断。
        // 多保留一个码点，避免超长段落截断后被误判为标题。
        if (para_flush_left && para_title_acc.size() <= kMaxTxtChapterTitleBytes) {
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
            para_title_acc.append(ubuf, un);
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
            push_chapter(std::move(para_title_acc), chap_line);
        }
        para_title_acc.clear();
    } else if (txt_lines_len_ == 0) {
        if (!flush_line(stream.Tell())) {
            ESP_LOGE(TAG, "txt paginate OOM");
            return false;
        }
    }
    if (!WaitTxtPaginate(paginate_gen)) return false;
    if (!push_page_start(static_cast<uint32_t>(txt_lines_len_))) {
        return false;
    }
    if (std::ferror(fp)) { error_ = BookReadError::kFileIo; return false; }
    if (!txt_lines_.Flush() || !txt_page_first_line_.Flush()) { error_ = BookReadError::kCacheIo; return false; }
    error_ = BookReadError::kNone;
    txt_page_count_ = page_starts_len - 1;
    FilterDenseTxtToc(built_toc, title_end_chars, content_chars);
    for (auto& entry : built_toc) {
        if (!WaitTxtPaginate(paginate_gen)) return false;
        entry.first_page = PageIndexForLine(entry.first_line);
    }
    if (!WaitTxtPaginate(paginate_gen)) return false;
    if (!SaveTxtIndex(info_.path.c_str(), file_size, built_toc, paginate_gen) &&
        Error() != BookReadError::kCancelled) ESP_LOGW(TAG, "txt index cache not saved");
    // 发布两份索引的文件长度，后续导航通过独立句柄读取。
    if (!txt_lines_.Detach() || !txt_page_first_line_.Detach()) { error_ = BookReadError::kCacheIo; return false; }
    if (!WaitTxtPaginate(paginate_gen)) return false;
    const size_t toc_count = built_toc.size();
    {
        std::lock_guard<std::mutex> lock(layout_progress_mu_);
        pending_toc_ = std::move(built_toc); pending_toc_complete_ = true;
        pending_txt_bytes_ = static_cast<uint32_t>(file_size);
        pending_txt_pages_ = static_cast<uint32_t>(txt_page_count_);
    }

    ESP_LOGI(TAG, "txt stream index lines=%u pages=%u file=%u toc=%u",
             static_cast<unsigned>(txt_lines_len_), static_cast<unsigned>(txt_page_count_),
             static_cast<unsigned>(file_size),
             static_cast<unsigned>(toc_count));
    return true;
}

bool BookSession::OpenTxt(const char* path, size_t known_size) {
    txt_work_id_ = s_txt_work_id.fetch_add(1);
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
        error_ = BookReadError::kFileIo;
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
        if (!txt_lines_.Detach() || !txt_page_first_line_.Detach()) {
            error_ = BookReadError::kCacheIo;
            return false;
        }
        ESP_LOGI(TAG, "txt empty ok: %s", path);
        ReportOpenProgress(90);
        return true;
    }
    if (sz > kMaxTxtBytes) {
        error_ = BookReadError::kUnsupported;
        ESP_LOGE(TAG, "txt too large: %u > %u (%s)", static_cast<unsigned>(sz),
                 static_cast<unsigned>(kMaxTxtBytes), path);
        std::fclose(fp);
        return false;
    }

    // fopen 后已在文件头；勿再 SEEK_END（已证实部分 TXT 会 EIO）
    uint8_t peek[4096];
    const size_t npeek = std::fread(peek, 1, sizeof(peek), fp);
    txt_as_gb18030_ = !LooksLikeUtf8(peek, npeek);

    const size_t est_lines = sz / 24 + 256;
    ESP_LOGI(TAG,
             "txt %s size=%u encoding=%s est_lines=%u est_idx~%uKB int=%u spiram=%u",
             path, static_cast<unsigned>(sz), txt_as_gb18030_ ? "GB18030" : "UTF-8",
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

    txt_fp_ = fp;
    PosFileData pos;
    uint32_t off = 0;
    bool para = true;
    if (LoadPosFile(ProgressPath(path).c_str(), pos) == PosLoadStatus::kOk) {
        if (pos.has_txt_anchor && pos.txt_anchor < sz) {
            off = pos.txt_anchor; para = pos.txt_para_start;
        } else if (pos.has_pct && pos.pct_x10 > 0 && pos.pct_x10 <= 1000) {
            const uint32_t target = static_cast<uint64_t>(sz - 1) * pos.pct_x10 / 1000;
            const uint32_t begin = target > 4096 ? target - 4096 : 0;
            if (std::fseek(fp, begin, SEEK_SET) == 0) {
                uint8_t bytes[4096];
                const size_t got = std::fread(bytes, 1, target - begin, fp);
                for (size_t i = 0; i < got; ++i) if (bytes[i] == '\n') off = begin + i + 1;
            }
        }
    }
    if (!FillTxtLivePreview(off, para)) return false;
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
    if (page_images_mode_) { SetVerticalWriting(false); SetLineSpacing(0, 0); }
    chapter_count_ = std::max(1, epub_->SpineCount());
    chapter_index_ = 0;
    ClearToc();
    for (int i = 0; i < chapter_count_; ++i) {
        TocEntry ent;
        char buf[48];
        std::snprintf(buf, sizeof(buf), page_images_mode_ ? UiPageFmt() : UiChapterFmt(), i + 1);
        ent.title = buf;
        toc_.push_back(std::move(ent));
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
    if (page_images_mode_) { SetVerticalWriting(false); SetLineSpacing(0, 0); }
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

bool BookSession::LoadChapter(int chapter) {
    if (chapter == chapter_index_ && !pages_.empty()) {
        page_index_ = 0;
        navigation_anchor_ = {};
        return true;
    }
    const auto* source = navigation_source_ ? navigation_source_ : this;
    std::lock_guard<std::mutex> lock(source->chapter_io_mu_);
    BookPageStore built;
    if (!built.Open(info_.path)) { error_ = built.Error(); return false; }
    if (!source->PaginateChapter(chapter, built) || !built.Finish()) {
        error_ = built.Good() ? source->Error() : built.Error();
        return false;
    }
    pages_.swap(built);
    navigation_replaced_pages_ = navigation_source_ != nullptr;
    navigation_anchor_ = {};
    page_index_ = 0;
    chapter_index_ = chapter;
    NoteChapterPageCount(chapter, static_cast<int>(pages_.size()));
    ReportOpenProgress(90);
    return true;
}

bool BookSession::PaginateChapter(int chapter, BookPageStore& built) const {
    if (chapter < 0 || chapter >= chapter_count_) return false;
    const uint32_t generation = chapter_pages_gen_;
    Page cur;
    lv_coord_t y = 0, line_w = 0;
    const lv_coord_t line_h = LayoutLineHeight();
    const auto layout = LineLayout();
    int row = 0;
    bool previous_title = false;
    std::string line;
    VerticalBreakState breaks;
    uint32_t block_index = UINT32_MAX, offset = 0, line_begin = 0;
    bool first_line = true;
    size_t glyphs = 0;
    ZipReader image_source;
    auto flush_line = [&](bool title) {
        if (line.empty()) return true;
        const bool indent = first_line && !title;
        if (indent) StripLeadingParaWs(line);
        const lv_coord_t gap = first_line ? ReaderParagraphGap(para_gap_, previous_title) : 0;
        const bool ok = FlushLineToPage(line, built, cur, y, layout, row, gap,
            indent, title, first_line, block_index, line_begin, offset);
        first_line = false; line_w = 0; breaks = {};
        return ok;
    };
    auto interrupted = [&]() {
        if (background_paused_.load() || generation != chapter_pages_gen_) { error_ = BookReadError::kCancelled; return true; }
        return false;
    };
    auto consume = [&](ContentBlock&& block) -> bool {
        if (interrupted()) return false;
        if (!block.continuation) {
            ++block_index; offset = 0; first_line = true; line_w = 0;
        }
        if (block.kind == ContentKind::kText) {
            if (block.chapter_title && !cur.items.empty()) {
                if (!built.Push(std::move(cur))) return false;
                cur = Page{}; y = 0; row = 0;
            }
            const auto* data = reinterpret_cast<const uint8_t*>(block.text.data());
            for (size_t i = 0; i < block.text.size();) {
                if (interrupted()) return false;
                if ((++glyphs & 0x1FFu) == 0) {
                    #ifndef EBOOK_EMULATOR
                    vTaskDelay(1);
                    #endif
                }
                uint32_t cp = 0;
                const size_t n = Utf8Next(data + i, block.text.size() - i, &cp);
                if (!n) { error_ = BookReadError::kInvalidData; return false; }
                if (cp == '\n' && !(vertical_writing_ && block.chapter_title)) {
                    if (!flush_line(block.chapter_title)) return false;
                    i += n; offset += n; continue;
                }
                if (first_line && line.empty() && IsTxtParaIndentWs(cp)) { i += n; offset += n; continue; }
                lv_coord_t width = LayoutGlyphWidth(cp);
                if ((cp == 0x20 || cp == 0xA0) && width <= 0) width = std::max<lv_coord_t>(1, line_h / 3);
                VerticalBreakState candidate = breaks;
                if (vertical_writing_) candidate.Observe(cp, offset, line.size());
                if (!line.empty() && (line_w + width > viewport_w_ || line.size() + n > kMaxLineSourceBytes)) {
                    if (vertical_writing_ && candidate.valid && candidate.offset < offset) {
                        const uint32_t cut = candidate.offset;
                        std::string tail = line.substr(candidate.text_size);
                        line.resize(candidate.text_size);
                        const uint32_t end = offset;
                        offset = cut;
                        if (!flush_line(block.chapter_title)) return false;
                        line = std::move(tail);
                        line_begin = cut;
                        offset = end;
                        for (size_t j = 0; j < line.size();) {
                            if (interrupted()) return false;
                            uint32_t tail_cp = 0;
                            const size_t count = Utf8Next(reinterpret_cast<const uint8_t*>(line.data()) + j,
                                                         line.size() - j, &tail_cp);
                            if (!count) return false;
                            breaks.Observe(tail_cp, cut + j, j);
                            line_w += LayoutGlyphWidth(tail_cp);
                            j += count;
                        }
                    } else if (!flush_line(block.chapter_title)) return false;
                }
                if (line.empty()) { line_begin = offset; line_w = first_line && !block.chapter_title ? LayoutIndentWidth() : 0; }
                if (vertical_writing_) breaks.Observe(cp, offset, line.size());
                line.append(block.text.data() + i, n); line_w += width; i += n; offset += n;
            }
            if (!block.continues) {
                if (!line.empty() && !flush_line(block.chapter_title)) return false;
                previous_title = block.chapter_title;
            }
            return true;
        }
        int img_w = physical_w_, img_h = physical_h_;
        const bool full_image = page_images_mode_ || vertical_writing_;
        if (!full_image) {
            int width = block.image_width, height = block.image_height;
            if (epub_) {
                // HTML 流仍占用文档 ZIP，尺寸探测使用独立句柄，避免嵌套锁。
                if (!image_source.IsOpen() && !image_source.Open(info_.path.c_str())) {
                    error_ = image_source.Error(); return false;
                }
                if (image_source.OpenEntryStream(block.image_href.c_str())) {
                    ReadImageDimensions([](void* context, uint8_t* bytes, size_t size) {
                        return static_cast<ZipReader*>(context)->EntryStreamRead(bytes, size);
                    }, &image_source, &width, &height);
                    image_source.CloseEntryStream();
                }
            }
            // 未知或不支持的图片保留独立占位，避免影响后续内容边界。
            FitImageSize(width, height, physical_w_, physical_h_, &img_w, &img_h);
        }
        if (!cur.items.empty() && (full_image || y + img_h > viewport_h_)) {
            if (!built.Push(std::move(cur))) return false;
            cur = Page{}; y = 0; row = 0;
        }
        PageItem item;
        item.kind = ContentKind::kImage; item.source_block = block_index; item.source_end = 1;
        item.image_href = std::move(block.image_href); item.image_w = img_w; item.image_h = img_h;
        cur.items.push_back(std::move(item)); y += img_h;
        previous_title = false;
        if (full_image) { if (!built.Push(std::move(cur))) return false; cur = Page{}; y = 0; row = 0; }
        return true;
    };
    bool first = true;
    const auto sink = [&](ContentBlock&& block) {
        if (page_images_mode_) return consume(std::move(block));
        if (!first) return consume(std::move(block));
        first = false;
        std::vector<ContentBlock> initial;
        initial.push_back(std::move(block));
        const std::string title = ebook_ ? ebook_->ChapterTitle(ebook_spine_[chapter]) :
            (static_cast<size_t>(chapter) < toc_.size() ? toc_[chapter].title : std::string{});
        EnsureChapterTitleInBlocks(title, chapter + 1, initial);
        for (auto& value : initial) if (!consume(std::move(value))) return false;
        return true;
    };
    error_ = BookReadError::kInvalidData;
    bool ok = false;
    try {
        if (ebook_) ok = ebook_->StreamChapterBlocks(ebook_spine_[chapter], sink);
        else if (epub_) ok = epub_->StreamChapterBlocks(chapter, sink);
        if (ok && !cur.items.empty()) ok = built.Push(std::move(cur));
        if (ok && built.empty()) {
            Page empty; PageItem item; item.text = UiChapterNoText();
            empty.items.push_back(std::move(item)); ok = built.Push(std::move(empty));
        }
    } catch (const std::bad_alloc&) { error_ = BookReadError::kNoMemory; return false; }
    if (!built.Good()) error_ = built.Error();
    else if (!ok && error_ == BookReadError::kInvalidData) {
        const auto doc_error = ebook_ ? ebook_->Error() : epub_->Error();
        if (doc_error != BookReadError::kNone) error_ = doc_error;
    }
    if (ok) error_ = BookReadError::kNone;
    return ok;
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
    chapter_page_counts_[static_cast<size_t>(chapter)] = static_cast<uint32_t>(pages);
}

bool BookSession::CountChapterPages(int chapter, int& out_pages) const {
    if (!IsChapteredBook() || chapter < 0 || chapter >= chapter_count_) return false;
    std::lock_guard<std::mutex> lock(chapter_io_mu_);
    if (chapter == chapter_index_ && !pages_.empty()) { out_pages = pages_.size(); return pages_.Good(); }
    BookPageStore counter;
    counter.Open(info_.path, true);
    if (!PaginateChapter(chapter, counter)) return false;
    out_pages = counter.size();
    return true;
}

namespace {

constexpr uint32_t kChapterPageCacheVersion = 4;

#pragma pack(push, 1)
struct ChapterPageHdr {
    char magic[4];  // "CPG1"
    uint32_t version;
    uint32_t chapter_count;
    uint16_t viewport_w;
    uint16_t viewport_h;
    uint16_t line_spacing_percent;
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
        hdr.line_spacing_percent != static_cast<uint16_t>(line_spacing_percent_) ||
        hdr.para_gap != static_cast<uint16_t>(para_gap_) || hdr.font_fp != (page_images_mode_ ? 0 : FontFingerprint()) ||
        hdr.page_images_mode != static_cast<uint8_t>(page_images_mode_ ? 1 : 0)) {
        std::fclose(ifp);
        return false;
    }
    std::vector<uint32_t> counts;
    try {
        counts.resize(static_cast<size_t>(chapter_count_));
    } catch (const std::bad_alloc&) {
        std::fclose(ifp);
        return false;
    }
    if (std::fread(counts.data(), sizeof(uint32_t), counts.size(), ifp) != counts.size()) {
        std::fclose(ifp);
        return false;
    }
    std::fclose(ifp);
    for (uint32_t n : counts) {
        if (n == 0 || n > INT32_MAX) {
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

bool BookSession::SaveChapterPageCache(const std::vector<uint32_t>& counts) const {
    if (!IsChapteredBook() || info_.path.empty() ||
        counts.size() != static_cast<size_t>(chapter_count_)) {
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
    hdr.line_spacing_percent = static_cast<uint16_t>(line_spacing_percent_);
    hdr.para_gap = static_cast<uint16_t>(para_gap_);
    hdr.font_fp = page_images_mode_ ? 0 : FontFingerprint();
    hdr.page_images_mode = static_cast<uint8_t>(page_images_mode_ ? 1 : 0);
    if (std::fwrite(&hdr, 1, sizeof(hdr), ofp) != sizeof(hdr) ||
        std::fwrite(counts.data(), sizeof(uint32_t), counts.size(),
                    ofp) != counts.size()) {
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
    try {
        std::lock_guard<std::mutex> lock(layout_progress_mu_);
        if (background_paused_.load() || gen != chapter_pages_gen_) return true;
        if (!completed_chapter_counts_.empty()) return true;
        if (chapter_scan_reset_ || chapter_scan_counts_.size() != static_cast<size_t>(chapter_count_)) {
            chapter_scan_counts_.assign(static_cast<size_t>(chapter_count_), 0);
            chapter_scan_reset_ = false;
        }
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "RebuildChapterPageTable OOM");
        return false;
    }
    // 前台导航已结束，当前章节的分页与扫描参数一致，可直接复用。
    if (!chapter_relayout_pending_ && chapter_index_ >= 0 && chapter_index_ < chapter_count_ && !pages_.empty() && pages_.Good()) {
        chapter_scan_counts_[static_cast<size_t>(chapter_index_)] = static_cast<uint32_t>(pages_.size());
    }
    for (int ch = 0; ch < chapter_count_; ++ch) {
        if (background_paused_.load() || gen != chapter_pages_gen_) {
            chapter_pages_ready_ = false;
            return true;
        }
        if (chapter_scan_counts_[static_cast<size_t>(ch)] != 0) continue;
        int n = 1;
        if (!CountChapterPages(ch, n)) {
            chapter_pages_ready_ = false;
            if (gen != chapter_pages_gen_) return true;
            return false;
        }
        if (n < 1) {
            n = 1;
        }
        chapter_scan_counts_[static_cast<size_t>(ch)] = static_cast<uint32_t>(n);
        std::lock_guard<std::mutex> lock(layout_progress_mu_);
        if (gen == chapter_pages_gen_ && pending_chapter_counts_.size() < 32) pending_chapter_counts_.emplace_back(ch, n);
    }
    if (gen != chapter_pages_gen_) {
        // 扫表期间又换了排版：作废本次结果，留给 again 再跑
        chapter_pages_ready_ = false;
        return true;
    }
    if (!SaveChapterPageCache(chapter_scan_counts_)) ESP_LOGW(TAG, "chapter page cache not saved");
    {
        std::lock_guard<std::mutex> lock(layout_progress_mu_);
        if (gen != chapter_pages_gen_) return true;
        completed_chapter_counts_ = std::move(chapter_scan_counts_);
    }

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
    if (chapter_pages_ready_.load()) return true;
    if (!IsChapteredBook()) {
        chapter_pages_ready_ = true;
        return true;
    }
    return RebuildChapterPageTable();
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

void BookSession::ClearPageImageCache() {
    std::lock_guard<std::mutex> lock(page_image_cache_mu_);
    cached_page_images_.clear();
}

size_t BookSession::PageImageCacheBudgetLocked() const {
    const size_t frame = size_t(std::max<int>(1, physical_w_)) * std::max<int>(1, physical_h_);
    if (!page_images_mode_) return frame;
    #ifndef EBOOK_EMULATOR
    size_t used = 0;
    for (const auto& entry : cached_page_images_) used += entry.image.pixels.size();
    const size_t available = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) + used;
    #else
    const size_t available = 64 * 1024 * 1024;
    #endif
    // 留出一半可用空间供解码和其他业务使用，并保持至少 2 MiB PSRAM 余量。
    constexpr size_t reserve = 2 * 1024 * 1024;
    return available > reserve ? std::min({available / 2, available - reserve, frame * 21}) : 0;
}

int BookSession::PageImagePrefetchRadius() const {
    std::lock_guard<std::mutex> lock(page_image_cache_mu_);
    if (!page_images_mode_ || physical_w_ <= 0 || physical_h_ <= 0) return 0;
    const size_t budget = PageImageCacheBudgetLocked();
    size_t used = 0;
    for (const auto& entry : cached_page_images_) used += entry.image.pixels.size();
    while (used > budget && !cached_page_images_.empty()) {
        used -= cached_page_images_.front().image.pixels.size();
        cached_page_images_.erase(cached_page_images_.begin());
    }
    const size_t slots = budget / (size_t(physical_w_) * physical_h_);
    return slots > 1 ? std::min<size_t>(10, (slots - 1) / 2) : 0;
}

bool BookSession::HasCachedPageImage(const std::string& href, int max_w, int max_h) const {
    std::lock_guard<std::mutex> lock(page_image_cache_mu_);
    auto entry = std::find_if(cached_page_images_.begin(), cached_page_images_.end(), [&](const auto& e) {
        return e.href == href && e.max_w == max_w && e.max_h == max_h;
    });
    if (entry == cached_page_images_.end()) return false;
    std::rotate(entry, entry + 1, cached_page_images_.end());
    return true;
}

bool BookSession::TryCopyCachedPageImage(const std::string& href, int max_w, int max_h, RasterImage& out) const try {
    out.Reset();
    std::lock_guard<std::mutex> lock(page_image_cache_mu_);
    auto entry = std::find_if(cached_page_images_.begin(), cached_page_images_.end(), [&](const auto& e) {
        return e.href == href && e.max_w == max_w && e.max_h == max_h;
    });
    if (entry == cached_page_images_.end()) return false;
    out.pixels = entry->image.pixels;
    out.width = entry->image.width; out.height = entry->image.height;
    out.BindDsc();
    std::rotate(entry, entry + 1, cached_page_images_.end());
    return true;
} catch (const std::bad_alloc&) {
    out.Reset();
    return false;
}

void BookSession::SetCachedPageImage(const std::string& href, int max_w, int max_h, RasterImage&& img) try {
    if (href.empty() || img.empty()) return;
    std::lock_guard<std::mutex> lock(page_image_cache_mu_);
    const size_t budget = PageImageCacheBudgetLocked();
    if (img.pixels.size() > budget) return;
    for (auto entry = cached_page_images_.begin(); entry != cached_page_images_.end();) {
        if (entry->href == href && entry->max_w == max_w && entry->max_h == max_h)
            entry = cached_page_images_.erase(entry);
        else ++entry;
    }
    size_t used = img.pixels.size();
    for (const auto& entry : cached_page_images_) used += entry.image.pixels.size();
    const size_t count = page_images_mode_ ? 21 : 1;
    while (!cached_page_images_.empty() && (used > budget || cached_page_images_.size() >= count)) {
        used -= cached_page_images_.front().image.pixels.size();
        cached_page_images_.erase(cached_page_images_.begin());
    }
    cached_page_images_.push_back({href, max_w, max_h, std::move(img)});
    cached_page_images_.back().image.BindDsc();
} catch (const std::bad_alloc&) {
    // 缓存为可选加速，分配失败不影响当前页显示。
}

bool BookSession::LoadPageImage(const std::string& href, int max_w, int max_h, RasterImage& out) {
    out.Reset();
    if (href.empty()) {
        return false;
    }
    if (TryCopyCachedPageImage(href, max_w, max_h, out)) return true;
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
    RasterImage copy;
    copy.pixels = out.pixels; copy.width = out.width; copy.height = out.height;
    copy.BindDsc();
    SetCachedPageImage(href, max_w, max_h, std::move(copy));
    return true;
}

const Page* BookSession::CurrentPageData() const try {
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
        return txt_lines_.Good() && txt_page_first_line_.Good() ? &txt_view_page_ : nullptr;
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
        if (!pages_.Good()) { error_ = pages_.Error(); return false; }
    }
    page_index_ = page;
    return true;
}

bool BookSession::IsChapteredBook() const {
    if (navigation_source_) return navigation_source_->IsChapteredBook();
    return (info_.format == BookFormat::kEpub && epub_) ||
           (info_.format == BookFormat::kEbook && ebook_);
}

bool BookSession::NeedsChapterPageRebuild() const {
    return IsChapteredBook() && !chapter_pages_ready_;
}

void BookSession::SetBackgroundPaused(bool paused) {
    background_paused_.store(paused);
    if (paused) {
        AbortTxtPaginate();
        AbortChapterPageTable();
    } else if (error_ == BookReadError::kCancelled) {
        error_ = BookReadError::kNone;
    }
}

void BookSession::AbortTxtPaginate() {
    txt_paginate_gen_++;
    ResumeTxtPaginate();
}

bool BookSession::PauseTxtPaginate() {
    if (!txt_mode_ || !live_preview_active_) return false;
    auto running = TxtScanState::kRunning;
    txt_scan_state_.compare_exchange_strong(running, TxtScanState::kPauseRequested);
    return txt_scan_state_.load() == TxtScanState::kPaused;
}

void BookSession::ResumeTxtPaginate() {
    txt_scan_state_.store(TxtScanState::kRunning);
}

bool BookSession::WaitTxtPaginate(uint32_t generation) const {
    for (;;) {
        if (background_paused_.load() || generation != txt_paginate_gen_) {
            error_ = BookReadError::kCancelled;
            return false;
        }
        auto state = txt_scan_state_.load();
        if (state == TxtScanState::kRunning) return true;
        if (state == TxtScanState::kPauseRequested)
            txt_scan_state_.compare_exchange_strong(state, TxtScanState::kPaused);
        #ifdef EBOOK_EMULATOR
        std::this_thread::yield();
        #else
        vTaskDelay(1);
        #endif
    }
}

void BookSession::AbortChapterPageTable() {
    if (!IsChapteredBook()) {
        return;
    }
    chapter_pages_gen_++;
}

void BookSession::InvalidateChapterPageTable() {
    if (!IsChapteredBook()) return;
    AbortChapterPageTable();
    chapter_pages_ready_ = false;
    std::lock_guard<std::mutex> lock(layout_progress_mu_);
    pending_chapter_counts_.clear(); completed_chapter_counts_.clear();
    chapter_scan_reset_ = true;
    std::fill(chapter_page_counts_.begin(), chapter_page_counts_.end(), 0);
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
    navigation_anchor_ = {};
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
        return true;
    }
    if (IsChapteredBook() && chapter_index_ + 1 < chapter_count_) {
        return LoadChapter(chapter_index_ + 1);
    }
    return false;
}

bool BookSession::PrevPage() {
    navigation_anchor_ = {};
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

int BookSession::PageIndexForLine(uint32_t line) const {
    if (!txt_mode_ || !txt_page_first_line_.IsOpen() || txt_page_count_ == 0) {
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
    if (!txt_mode_ || !txt_page_first_line_.IsOpen() || !txt_lines_.IsOpen() ||
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
    if (navigation_anchor_.valid && navigation_anchor_.page == page_index_ && navigation_anchor_.chapter == chapter_index_) return navigation_anchor_;
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
        } else if (!txt_empty_ && txt_lines_.IsOpen() && txt_page_first_line_.IsOpen() &&
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
    txt_preview_anchor_.store(byte_off);
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
        return false;
    }

    TxtByteStream stream;
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
    if (layout.count <= 0) { error_ = BookReadError::kUnsupported; return false; }
    int row = 0;
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

    VerticalBreakState breaks;
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
        if (chap_cont && !vertical_writing_) {
            if (row >= layout.count || y + layout.CellHeight(row) > viewport_h_) return 0;
            try {
                live_preview_page_.items.back().text += line;
            } catch (const std::bad_alloc&) {
                line.clear();
                return -1;
            }
            line.clear();
            y += layout.CellHeight(row++);
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
            ReaderParagraphGap(para_gap_, after_chap);
        lv_coord_t need_y = y;
        if (line_para_start && y > 0) {
            if ((row >= layout.count || y + gap + layout.CellHeight(row) > viewport_h_) && !live_preview_page_.items.empty()) {
                return 0;
            }
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
        if ((row + rows_needed > layout.count || need_y + layout.CellHeight(row) + chap_extra > viewport_h_) && !live_preview_page_.items.empty()) {
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
            item.chapter_title = is_chap || chap_cont;
            item.text = is_chap && !vertical_writing_ ? FormatChapterHeadingDisplay(line) : std::move(line);
            live_preview_page_.items.push_back(std::move(item));
        } catch (const std::bad_alloc&) {
            line.clear();
            return -1;
        }
        line.clear();
        breaks = {};
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
        lv_coord_t cw = LayoutGlyphWidth(cp);
        if ((cp == 0x20 || cp == 0xA0) && cw <= 0) {
            cw = std::max<lv_coord_t>(1, LayoutLineHeight() / 3);
        }
        VerticalBreakState candidate = breaks;
        if (vertical_writing_) candidate.Observe(cp, goff, line.size());
        if (line_started && (line_w + cw > viewport_w_ || line.size() + 4 > kMaxLineSourceBytes)) {
            const bool rewind = vertical_writing_ && candidate.valid && candidate.offset < goff;
            const uint32_t cut = rewind ? candidate.offset : goff;
            if (rewind) line.resize(candidate.text_size);
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
            if (rewind) {
                if (!stream.Seek(cut)) { error_ = BookReadError::kFileIo; return false; }
                continue;
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
        if (vertical_writing_) breaks.Observe(cp, goff, line.size());
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

    navigation_anchor_ = {};
    if (txt_mode_) {
        PollLayoutProgress();
        txt_indexed_bytes_ = txt_indexed_pages_ = 0;
        {
            std::lock_guard<std::mutex> lock(layout_progress_mu_);
            pending_txt_bytes_ = pending_txt_pages_ = 0;
        }
        live_preview_hist_.clear();
        live_preview_byte_off_ = anchor.txt_byte_off;
        live_preview_at_para_start_ = anchor.txt_at_para_start;
        txt_preview_anchor_.store(anchor.txt_byte_off);
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
        InvalidateChapterPageTable();
        return true;
    }
    return false;
}

bool BookSession::FinishLiveRelayout(const LayoutAnchor& anchor) try {
    if (background_paused_.load()) { error_ = BookReadError::kCancelled; return false; }
    if (IsChapteredBook()) {
        std::lock_guard<std::mutex> lock(chapter_io_mu_);
        pending_pages_.clear();
        if (!pending_pages_.Open(info_.path)) { error_ = pending_pages_.Error(); return false; }
        const int chapter = anchor.valid ? anchor.chapter : chapter_index_;
        const bool ok = PaginateChapter(chapter, pending_pages_) && pending_pages_.Finish();
        if (!ok && !pending_pages_.Good()) error_ = pending_pages_.Error();
        if (!ok) pending_pages_.clear();
        return ok;
    }
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
    std::unique_ptr<FILE, decltype(&std::fclose)> source(fp, &std::fclose);
    size_t sz = info_.file_size;
    struct stat file_stat{};
    if (stat(info_.path.c_str(), &file_stat) == 0 && file_stat.st_size >= 0) sz = file_stat.st_size;
    if (!sz) { error_ = BookReadError::kFileIo; return false; }
    OpenProgressFn saved_fn = progress_fn_;
    void* saved_user = progress_user_;
    progress_fn_ = nullptr;
    progress_user_ = nullptr;
    const bool ok = StreamPaginateTxt(fp, sz);
    if (ok) {
        const auto off = txt_preview_anchor_.load();
        const int page = PageIndexForTxtByteOff(off);
        std::lock_guard<std::mutex> lock(layout_progress_mu_);
        pending_anchor_ = off; pending_anchor_page_ = page;
    }
    progress_fn_ = saved_fn;
    progress_user_ = saved_user;
    if (!ok) {
        txt_lines_.Reset();
        txt_page_first_line_.Reset();
        txt_lines_cap_ = txt_page_starts_cap_ = txt_lines_len_ = txt_page_count_ = 0;
        return false;
    }
    return true;
} catch (const std::bad_alloc&) {
    error_ = BookReadError::kNoMemory;
    pending_pages_.clear();
    if (txt_mode_) {
        txt_lines_.Reset();
        txt_page_first_line_.Reset();
        txt_lines_cap_ = txt_page_starts_cap_ = txt_lines_len_ = txt_page_count_ = 0;
    }
    return false;
}

void BookSession::CommitLiveRelayout(const LayoutAnchor& anchor) {
    navigation_anchor_ = {};
    PollLayoutProgress();
    if (IsChapteredBook()) {
        if (pending_pages_.empty()) return;
        pages_.swap(pending_pages_);
        chapter_index_ = anchor.valid ? anchor.chapter : chapter_index_;
        const int count = std::max(1, PageCount());
        page_index_ = std::clamp(static_cast<int>(static_cast<int64_t>(anchor.page) * count / std::max(1, anchor.page_count)), 0, count - 1);
        NoteChapterPageCount(chapter_index_, count);
        chapter_relayout_pending_ = false;
        return;
    }
    // 优先用当前预览锚点（编排中可能已翻页/换参）
    const uint32_t off = live_preview_active_
                             ? live_preview_byte_off_
                             : (anchor.valid ? anchor.txt_byte_off : 0);
    page_index_ = indexed_anchor_ == off ? indexed_anchor_page_ : PageIndexForTxtByteOff(off);
    if (page_index_ < 0) {
        page_index_ = 0;
    }
    if (page_index_ >= PageCount()) {
        page_index_ = std::max(0, PageCount() - 1);
    }
    txt_view_index_ = -1;
    ClearLivePreview();
    ESP_LOGI(TAG, "txt relayout done pages=%d at=%d off=%u", PageCount(), page_index_ + 1,
             static_cast<unsigned>(off));
}

void BookSession::FinalizeTocPages() {
    const auto& starts = txt_page_first_line_;
    size_t page = 0;
    // 目录按章首行有序，顺序读取页表，避免每章二分引发 SD 窗口反复切换。
    for (auto& ent : toc_) {
        while (page + 1 < txt_page_count_ && starts[page + 1] <= ent.first_line) ++page;
        ent.first_page = static_cast<int>(page);
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
    if (!dest.empty() && dest.back().first_line == first_line) {
        return false;
    }
    TocEntry ent;
    ent.title = std::move(title);
    ent.first_line = first_line;
    if (first_line < txt_lines_len_ && txt_lines_.IsOpen()) {
        ent.first_byte_off = txt_lines_[first_line].off;
    }
    ESP_LOGI(TAG, "chap toc+#%u line=%u '%.*s'", static_cast<unsigned>(dest.size() + 1),
             static_cast<unsigned>(first_line), 48, ent.title.c_str());
    dest.push_back(std::move(ent));
    return true;
}


bool BookSession::PollLayoutProgress() try {
    std::vector<TocEntry> entries;
    std::vector<std::pair<int, uint32_t>> counts;
    std::vector<uint32_t> completed;
    bool replace = false, changed = false;
    {
        std::unique_lock<std::mutex> lock(layout_progress_mu_, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        changed = txt_indexed_bytes_ != pending_txt_bytes_ || !pending_toc_.empty() ||
            !pending_chapter_counts_.empty() || !completed_chapter_counts_.empty();
        txt_indexed_bytes_ = pending_txt_bytes_; txt_indexed_pages_ = pending_txt_pages_;
        indexed_anchor_ = pending_anchor_; indexed_anchor_page_ = pending_anchor_page_;
        entries.swap(pending_toc_); counts.swap(pending_chapter_counts_);
        completed.swap(completed_chapter_counts_);
        replace = pending_toc_complete_; pending_toc_complete_ = false;
    }
    if (replace) toc_.swap(entries);
    else for (auto& entry : entries) {
        const auto position = std::lower_bound(toc_.begin(), toc_.end(), entry.first_byte_off,
            [](const TocEntry& old, uint32_t off) { return old.first_byte_off < off; });
        if (position != toc_.end() && position->first_byte_off == entry.first_byte_off) *position = std::move(entry);
        else toc_.insert(position, std::move(entry));
    }
    for (const auto& count : counts) NoteChapterPageCount(count.first, count.second);
    if (!completed.empty()) {
        chapter_page_counts_.swap(completed); chapter_pages_ready_ = true;
    }
    return changed;
} catch (const std::bad_alloc&) {
    error_ = BookReadError::kNoMemory;
    return false;
}

bool BookSession::SeekTxtPreview(uint32_t target) {
    if (!EnsureTxtFile()) return false;
    const uint32_t begin = target > 4096 ? target - 4096 : 0;
    if (std::fseek(txt_fp_, begin, SEEK_SET) != 0) return false;
    uint8_t bytes[4096];
    const size_t got = std::fread(bytes, 1, target - begin, txt_fp_);
    if (got != target - begin) { error_ = BookReadError::kFileIo; return false; }
    uint32_t off = target;
    bool paragraph = target == 0;
    for (size_t i = 0; i < got; ++i) if (bytes[i] == '\n') { off = begin + i + 1; paragraph = true; }
    if (!paragraph && !txt_as_gb18030_ && got) {
        size_t i = got - 1;
        while (i > 0 && (bytes[i] & 0xC0) == 0x80) --i;
        const uint8_t lead = bytes[i];
        const size_t size = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (got - i < size) off = begin + static_cast<uint32_t>(i);
    }
    live_preview_hist_.clear();
    const bool ok = FillTxtLivePreview(off, paragraph);
    return ok;
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

bool BookSession::CaptureBookmarkRange(BookSourcePosition& first,BookSourcePosition& end) const {
    if(!open_)return false;
    if (txt_mode_ && live_preview_active_) {
        first = {0, 0, live_preview_byte_off_};
        end = {0, 0, live_preview_has_next_ ? live_preview_next_off_ :
            static_cast<uint32_t>(std::min<size_t>(info_.file_size, UINT32_MAX))};
        return SourceBefore(first, end);
    }
    if(txt_mode_) {
        if(txt_empty_ || !txt_lines_.IsOpen() || !txt_page_first_line_.IsOpen() || page_index_<0 ||
           static_cast<size_t>(page_index_)>=txt_page_count_)return false;
        const uint32_t line=txt_page_first_line_[page_index_];
        if(line>=txt_lines_len_)return false;
        first={0,0,txt_lines_[line].off};
        uint32_t end_offset=static_cast<uint32_t>(std::min<size_t>(info_.file_size,UINT32_MAX));
        if(static_cast<size_t>(page_index_+1)<txt_page_count_) {
            const uint32_t next=txt_page_first_line_[page_index_+1];
            if(next<txt_lines_len_)end_offset=txt_lines_[next].off;
        }
        end={0,0,end_offset};return SourceBefore(first,end);
    }
    if(page_index_<0 || static_cast<size_t>(page_index_)>=pages_.size())return false;
    const uint32_t chapter=info_.format == BookFormat::kEbook?static_cast<uint32_t>(ebook_spine_[chapter_index_]):static_cast<uint32_t>(chapter_index_);
    bool found=false;
    for(const auto& item:pages_[page_index_].items) {
        if(item.source_block==UINT32_MAX)continue;
        if(!found) { first={chapter,item.source_block,item.source_begin};found=true; }
        end={chapter,item.source_block,item.source_end};
    }
    if(!found)return false;
    if(static_cast<size_t>(page_index_+1)<pages_.size()) {
        for(const auto& item:pages_[page_index_+1].items)if(item.source_block!=UINT32_MAX) {
            end={chapter,item.source_block,item.source_begin};break;
        }
    }
    return SourceBefore(first,end);
}
uint32_t BookSession::CurrentPageBookmarkId() const {
    BookSourcePosition first,end;if(!CaptureBookmarkRange(first,end))return 0;
    for(const auto& entry:bookmarks_.Entries())if(SourceInPage(entry.start,first,end))return entry.id;
    return 0;
}
bool BookSession::AddCurrentPageBookmark(std::string& error) {
    if(CurrentPageBookmarkId())return true;
    try {
        BookBookmark entry;
        if(!CaptureBookmarkRange(entry.start,entry.end)) { error=UiBookmarkLayoutBusy();return false; }
        entry.page=std::max(0,BookCurrentPage());entry.added_at=std::time(nullptr);
        const auto* page=CurrentPageData();size_t characters=0;
        if(page)for(const auto& item:page->items) {
            if(item.kind!=ContentKind::kText || item.chapter_title)continue;
            if(!entry.excerpt.empty() && item.para_gap_before && characters<150) {
                entry.excerpt+='\n';++characters;
            }
            size_t i=0;
            while(i<item.text.size() && characters<150) {
                const size_t n=Utf8Next(reinterpret_cast<const uint8_t*>(item.text.data()+i),item.text.size()-i,nullptr);
                if(!n || entry.excerpt.size()+n>BookBookmarkStore::kMaxExcerptBytes)break;
                entry.excerpt.append(item.text,i,n);i+=n;++characters;
            }
            if(characters>=150)break;
        }
        if(entry.excerpt.empty())entry.excerpt=CurrentTocTitle()+UiBookmarkImageSuffix();
        if(entry.excerpt.size()>BookBookmarkStore::kMaxExcerptBytes)entry.excerpt=UiBookmarkImage();
        return bookmarks_.Add(std::move(entry),error);
    } catch(const std::bad_alloc&) { error=UiBookmarkAddMemory();return false; }
}
bool BookSession::GoToBookmark(uint32_t id) {
    if(!bookmarks_.Error().empty())return false;
    const auto& entries=bookmarks_.Entries();
    const auto found=std::find_if(entries.begin(),entries.end(),[id](const BookBookmark& entry){return entry.id==id;});
    if(found==entries.end())return false;
    return GoToBookmarkPosition(found->start);
}

bool BookSession::GoToBookmarkPosition(const BookSourcePosition& position) {
    if (!open_) return false;
    if (txt_mode_ && live_preview_active_) {
        if (position.offset >= info_.file_size || !EnsureTxtFile()) return false;
        bool paragraph = position.offset == 0;
        if (!paragraph) {
            if (std::fseek(txt_fp_, position.offset - 1, SEEK_SET) != 0) { error_ = BookReadError::kFileIo; return false; }
            const int previous = std::fgetc(txt_fp_);
            if (previous == EOF) { error_ = BookReadError::kFileIo; return false; }
            paragraph = previous == '\n' || previous == '\r';
        }
        live_preview_hist_.clear();
        return FillTxtLivePreview(position.offset, paragraph);
    }
    if (live_preview_active_) return false;
    if(txt_mode_)return position.offset<info_.file_size && GoToPage(PageIndexForTxtByteOff(position.offset));
    int chapter=position.chapter;
    if(info_.format == BookFormat::kEbook) {
        const auto original=std::find(ebook_spine_.begin(),ebook_spine_.end(),static_cast<int>(position.chapter));
        if(original==ebook_spine_.end())return false;
        chapter=static_cast<int>(original-ebook_spine_.begin());
    }
    if(chapter<0 || chapter>=chapter_count_)return false;
    const int original_chapter=chapter_index_,original_page=page_index_;
    const bool change_chapter=chapter!=chapter_index_;
    BookPageStore original_pages;
    if(change_chapter) {
        original_pages=std::move(pages_);
        if(!(LoadChapter(chapter))) {
            pages_=std::move(original_pages);chapter_index_=original_chapter;page_index_=original_page;
            return false;
        }
    }
    // Pick the new page containing the immutable content position, not old page ratio.
    const int previous_page=page_index_;
    for(size_t p=0;p<pages_.size();++p) {
        page_index_=p;BookSourcePosition first,end;
        if(CaptureBookmarkRange(first,end) && SourceInPage(position,first,end)) {
            page_index_=previous_page;return GoToPage(p);
        }
    }
    if(change_chapter)pages_=std::move(original_pages);
    chapter_index_=original_chapter;page_index_=original_page;return false;
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
        return LoadChapter(index);
    }
    return false;
}

std::string BookSession::CurrentTocTitle() const {
    if (page_images_mode_) {
        char title[48];
        std::snprintf(title, sizeof(title), UiPageFmt(), BookCurrentPage() + 1);
        return title;
    }
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

bool BookSession::GoToReadingProgress(int progress_x10) {
    if (!open_) return false;
    if (txt_mode_ && info_.file_size) {
        const uint32_t target = static_cast<uint64_t>(info_.file_size - 1) * std::clamp(progress_x10, 0, 1000) / 1000;
        if (live_preview_active_) return SeekTxtPreview(target);
        const int page = PageIndexForTxtByteOff(target);
        if (!txt_lines_.Good() || !txt_page_first_line_.Good()) {
            error_ = BookReadError::kCacheIo;
            return false;
        }
        return GoToPage(page);
    }
    if (live_preview_active_) return false;
    const int pct = std::clamp(progress_x10, 0, 1000);
    const int target = static_cast<int>(static_cast<int64_t>(std::max(1, BookPageCount()) - 1) * pct / 1000);
    if (!IsChapteredBook()) return GoToPage(target);
    int offset = target;
    const int fallback = std::max(1, PageCount());
    for (int ch = 0; ch < chapter_count_; ++ch) {
        const int count = chapter_page_counts_.size() == static_cast<size_t>(chapter_count_)
            ? (chapter_page_counts_[ch] ? static_cast<int>(chapter_page_counts_[ch]) : fallback) : fallback;
        if (offset < count || ch + 1 == chapter_count_) {
            const bool loaded = LoadChapter(ch);
            if (!loaded) return false;
            return GoToPage(std::clamp(offset, 0, std::max(1, PageCount()) - 1));
        }
        offset -= count;
    }
    return false;
}

int BookSession::ReadingProgressX10() const {
    if (txt_mode_ && !txt_empty_ && info_.file_size) {
        if ((live_preview_active_ && !live_preview_has_next_) ||
            (!live_preview_active_ && page_index_ + 1 >= PageCount())) return 1000;
        const auto anchor = CaptureLayoutAnchor();
        return static_cast<int>(std::min<uint64_t>(999, static_cast<uint64_t>(anchor.txt_byte_off) * 1000 / info_.file_size));
    }
    if (IsChapteredBook() && ChapterCount() > 1) {
        const int page = BookCurrentPage() + 1;
        const int pages = BookPageCount();
        if (pages <= 0) {
            return 0;
        }
        int pct_x10 = static_cast<int>(static_cast<int64_t>(page) * 1000 / pages);
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
    int pct_x10 = static_cast<int>(static_cast<int64_t>(page) * 1000 / pages);
    if (pct_x10 < 0) {
        pct_x10 = 0;
    }
    if (pct_x10 > 1000) {
        pct_x10 = 1000;
    }
    return pct_x10;
}


}  // namespace reader
