#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace reader {

constexpr size_t kMaxTxtChapterTitleChars = 60; // 章节标题最多字符数
constexpr size_t kMaxTxtChapterTitleBytes = kMaxTxtChapterTitleChars * 4; // UTF-8 最坏字节数

// TXT 目录条目（显示行索引 → 打开后绑定 first_page；first_byte_off 供编排中定位）
struct TocEntry {
    std::string title; // 章节标题，保留源文
    uint32_t first_line = 0;      // 对应 txt_lines_ 下标
    uint32_t first_byte_off = 0;  // 源文件字节锚点
    int first_page = 0;           // 绑定后的页码
};

/**
 * @brief 判断顶格、已去首尾空白的 UTF-8 行是否为章节标题，最多 60 个字符
 * @note 支持中文序数、特殊章名、英文序数和数字开头章名；设备侧使用手写匹配。
 */
bool IsTxtChapterTitleLine(const char* utf8, size_t len);

/** @brief 判断 UTF-8 字符串是否为章节标题 */
inline bool IsTxtChapterTitleLine(const std::string& s) {
    return IsTxtChapterTitleLine(s.data(), s.size());
}

/**
 * @brief 拆成「第N章」与后随章节名；无「第N章/卷N」前缀则失败
 * @param title 顶格章名原文
 * @param ordinal 输出序数行（如「第18章」）
 * @param name 输出章节名，可为空（空则不显示第二行）
 * @return 是否识别到序数前缀
 */
bool SplitChapterHeading(const std::string& title, std::string& ordinal, std::string& name);

/**
 * @brief 正文展示：序数单独一行，有章节名则换行紧跟；无法拆分则原样
 */
std::string FormatChapterHeadingDisplay(const std::string& title);

}  // namespace reader
