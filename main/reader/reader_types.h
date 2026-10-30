#pragma once

#include <cstdint>
#include <algorithm>
#include <string>
#include <functional>
#include "book_read_error.h"
#include <vector>

#include "lvgl.h"

namespace reader {

enum class BookFormat : uint8_t {
    kUnknown = 0,
    kTxt,
    kEpub,
    kEbook,
};

enum class ContentKind : uint8_t {
    kText = 0,
    kImage,
};

struct BookInfo {
    std::string path;       // 绝对路径，如 /sdcard/metalio/e-ink/books/foo.epub
    std::string title;      // 显示名（文件名或 OPF 标题）
    std::string author;
    BookFormat format = BookFormat::kUnknown;
    size_t file_size = 0;
    int64_t mtime = 0;      // 文件修改时间（秒），书库列表按最新优先排序
};

// 章节/正文中的一段内容：纯文本或图片（EPUB 内路径 / 外部文件路径）
struct ContentBlock {
    ContentKind kind = ContentKind::kText;
    std::string text;       // kText
    std::string image_href; // kImage：相对 EPUB 根或绝对文件路径
    int image_width = 0, image_height = 0; // kImage：已知原图尺寸，未知为 0
    bool chapter_title = false; // 章名段：分页后加粗居中；可含「第N章」换行接章节名
    bool continuation = false; // 同一段正文的后续片段
    bool continues = false; // 此片段之后同一段正文还有内容
};

using ContentBlockSink = std::function<bool(ContentBlock&&)>; // 同步消费正文，返回 false 停止读取

// 一页上的一个绘制单元
struct PageItem {
    uint32_t source_block = UINT32_MAX;
    uint32_t source_begin = 0, source_end = 0;
    ContentKind kind = ContentKind::kText;
    std::string text;
    std::string image_href;
    lv_coord_t image_w = 0;
    lv_coord_t image_h = 0;
    // 段首行：渲染侧 pad_left 两字宽（不依赖源文空格/U+3000 字形）
    bool para_indent = false;
    // 段前加间距（段首行，含不缩进的章名）；渲染侧非页首时插入垂直 spacer
    bool para_gap_before = false;
    // 章名行：加粗居中；「第N章」与章节名可分行；误识别也不丢句
    bool chapter_title = false;
};

struct Page {
    std::vector<PageItem> items;
};

// 阅读排版默认值（与 BookReaderPrefs 标准档一致；经 BookSession::SetLineSpacing 注入）
constexpr int kReaderLineSpacingPercentDefault = 10; // 标准行疏密，按字体行框比例计算
constexpr lv_coord_t kReaderParaGapDefault = 0; // 普通段落不增加额外段距，保持正文页行数稳定
constexpr lv_coord_t kReaderChapterTitleAfterGap = 10; // 章名与后文额外间距
constexpr int kReaderUnderlineGap = 2; // 横排字形下沿与下划线之间的净空像素数
constexpr int kReaderUnderlineThickness = 2; // 横排下划线的像素线宽

inline int ReaderGlyphInkBottom(const lv_font_t* font, const lv_font_glyph_dsc_t& glyph) {
    return static_cast<int>(font->line_height) - static_cast<int>(font->base_line) - glyph.ofs_y - 1;
}

inline int ReaderUnderlineTop(int ink_bottom) {
    return ink_bottom + kReaderUnderlineGap + 1;
}

struct TextLineLayout {
    lv_coord_t extent = 0; // 完整阅读视口沿换行方向的长度
    lv_coord_t line_height = 0; // 包含下划线的字形行框
    int count = 0; // 同一视口最多容纳的行或列数
    lv_coord_t CellHeight(int row) const {
        if (count <= 0 || row < 0 || row >= count) return 0;
        return static_cast<lv_coord_t>((static_cast<int64_t>(row + 1) * extent) / count -
            (static_cast<int64_t>(row) * extent) / count);
    }
    lv_coord_t TextOffset(int row) const { return std::max<lv_coord_t>(0, (CellHeight(row) - line_height) / 2); }
};

/** @brief 按字体行框、相对疏密及完整视口生成均分余量的行布局。 */
inline TextLineLayout MakeTextLineLayout(const lv_font_t* font, lv_coord_t extent,
                                         int spacing_percent, bool vertical = false) {
    TextLineLayout layout;
    layout.extent = std::max<lv_coord_t>(0, extent);
    const int line_h = font && font->line_height > 0 ? font->line_height : 24;
    // 行内始终预留装饰空间，开关下划线不改变文字位置和分页。
    const int frame = line_h + (vertical ? 1 : kReaderUnderlineGap + kReaderUnderlineThickness);
    layout.line_height = static_cast<lv_coord_t>(frame);
    const int desired = std::max(frame, line_h + static_cast<int>(
        (static_cast<int64_t>(line_h) * std::max(0, spacing_percent) + 50) / 100));
    if (layout.extent >= frame) layout.count = std::max(1, static_cast<int>(layout.extent) / std::max(1, desired));
    return layout;
}

/** @brief 段落在普通行距之外增加的间距，章名后附加章后距。 */
inline lv_coord_t ReaderParagraphGap(lv_coord_t para_gap, bool previous_title) {
    return static_cast<lv_coord_t>(std::max<lv_coord_t>(0, para_gap) +
        (previous_title ? kReaderChapterTitleAfterGap : 0));
}

// 解码后的封面/插图（L8，适合 I1 墨水屏）
struct RasterImage {
    lv_image_dsc_t dsc{};
    std::vector<uint8_t> pixels;  // 拥有像素；dsc.data 指向此处
    uint16_t width = 0;
    uint16_t height = 0;

    bool empty() const { return pixels.empty() || width == 0 || height == 0; }

    void Reset() {
        pixels.clear();
        width = 0;
        height = 0;
        dsc = {};
    }

    void BindDsc() {
        dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc.header.cf = LV_COLOR_FORMAT_L8;
        dsc.header.flags = 0;
        dsc.header.w = width;
        dsc.header.h = height;
        dsc.header.stride = width;
        dsc.data_size = pixels.size();
        dsc.data = pixels.data();
    }
};

inline const char* FormatLabel(BookFormat f) {
    switch (f) {
        case BookFormat::kTxt:
            return "TXT";
        case BookFormat::kEpub:
            return "EPUB";
        case BookFormat::kEbook:
            return "EBOOK";
        default:
            return "?";
    }
}

}  // namespace reader
