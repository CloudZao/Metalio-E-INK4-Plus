#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "book_toc.h"

namespace reader {

constexpr size_t kMaxTxtChapterTitleChars = 60; // 章节标题最多字符数
constexpr size_t kMaxTxtChapterTitleBytes = kMaxTxtChapterTitleChars * 4; // UTF-8 最坏字节数

/**
 * @brief 判断已去首尾空白的中英文 UTF-8 单行是否为章节标题，最多 60 个字符
 * @note 支持中文序数、特殊章名、英文序数和数字开头章名；设备侧使用手写匹配。
 */
bool IsTxtChapterTitleLine(const char* utf8, size_t len);

/** @brief 判断 UTF-8 字符串是否为章节标题 */
inline bool IsTxtChapterTitleLine(const std::string& s) {
    return IsTxtChapterTitleLine(s.data(), s.size());
}

/**
 * @brief 拆分中英文章节序数与后随章节名，支持罗马数字
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

enum class TxtHeadingKind {
    kNone, // 普通正文
    kChapter, // 章节起始标题
    kSubtitle, // 紧随章节起始标题的副标题
};

/** @brief 有界识别英文缩进标题、副标题及 Contents 中声明的篇名。 */
class TxtChapterDetector {
public:
    /** @brief 判断当前段首是否需要有界标题探测。 */
    bool MayStart(uint32_t cp) const;
    /** @brief 消费一个完整逻辑行，超出预算的行按正文处理。 */
    TxtHeadingKind Classify(const std::string& text, bool indented, uint32_t source_offset, bool complete = true);
    /** @brief 记录空行，结束副标题关联或目录清单。 */
    void BlankLine();
private:
    std::vector<std::string> contents_; // 最多 128 个由书内 Contents 提供的篇名
    bool collecting_ = false, subtitle_pending_ = false, blank_before_ = true; // 有界跨行状态
    unsigned blank_count_ = 0; // 目录清单后的连续空行数
};

}  // namespace reader
