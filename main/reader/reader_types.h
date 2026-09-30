#pragma once

#include <cstdint>
#include <string>
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
    bool chapter_title = false; // 章名段：分页后加粗居中；可含「第N章」换行接章节名
};

// 一页上的一个绘制单元
struct PageItem {
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

// 阅读排版默认值（与 BookReaderPrefs 标准档一致；经 BookSession::SetGaps 注入）
constexpr lv_coord_t kReaderLineGapDefault = 6;
constexpr lv_coord_t kReaderParaGapDefault = 14;
constexpr lv_coord_t kReaderChapterTitleAfterGap = 10; // 章名与后文额外间距
/** @deprecated 兼容旧引用；新代码用 Default 或 prefs */
constexpr lv_coord_t kReaderLineGap = kReaderLineGapDefault;
constexpr lv_coord_t kReaderParaGap = kReaderParaGapDefault;

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
