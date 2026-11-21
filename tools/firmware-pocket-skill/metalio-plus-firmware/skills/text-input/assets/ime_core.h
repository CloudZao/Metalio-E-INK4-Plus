// Project-authored minimal input core. Data notices are in licenses/ATTRIBUTION.md.
// No drawing, GPIO, persistence, network, or font dependency. Generate pinyin_table.h first.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "pinyin_table.h"

namespace metalio_plus_ime {
inline bool ValidUtf8(const std::string& value) {
    std::size_t pos = 0;
    while (pos < value.size()) {
        const auto first = static_cast<unsigned char>(value[pos]);
        std::size_t width = 0;
        std::uint32_t cp = 0;
        if (first > 0 && first < 0x80) { width = 1; cp = first; }
        else if (first >= 0xC2 && first <= 0xDF) { width = 2; cp = first & 0x1FU; }
        else if (first >= 0xE0 && first <= 0xEF) { width = 3; cp = first & 0x0FU; }
        else if (first >= 0xF0 && first <= 0xF4) { width = 4; cp = first & 0x07U; }
        else { return false; }
        if (width > value.size() - pos) { return false; }
        for (std::size_t j = 1; j < width; ++j) {
            const auto next = static_cast<unsigned char>(value[pos + j]);
            if ((next & 0xC0U) != 0x80U) { return false; }
            cp = (cp << 6U) | (next & 0x3FU);
        }
        if ((width == 2 && cp < 0x80U) || (width == 3 && cp < 0x800U) ||
            (width == 4 && cp < 0x10000U) || cp > 0x10FFFFU ||
            (cp >= 0xD800U && cp <= 0xDFFFU)) { return false; }
        pos += width;
    }
    return true;
}

inline std::string Normalize(const std::string& input) {
    if (input.empty() || input.size() > 6) { return {}; }
    std::string key;
    for (char c : input) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
        if (c < 'a' || c > 'z') { return {}; }
        key.push_back(c);
    }
    return key;
}

inline const Entry* Find(const std::string& key) {
    return std::lower_bound(kPinyin, kPinyin + kPinyinCount, key,
        [](const Entry& entry, const std::string& needle) {
            return std::strcmp(entry.syllable, needle.c_str()) < 0;
        });
}

inline std::vector<std::string> Candidates(const std::string& input) {
    const std::string key = Normalize(input);
    if (key.empty()) { return {}; }
    const Entry* entry = Find(key);
    if (entry == kPinyin + kPinyinCount || key != entry->syllable) { return {}; }
    const std::string text(entry->candidates_utf8);
    if (!ValidUtf8(text)) { return {}; }
    std::vector<std::string> result;
    for (std::size_t pos = 0; pos < text.size();) {
        const auto first = static_cast<unsigned char>(text[pos]);
        const std::size_t width = first < 0x80 ? 1U : first < 0xE0 ? 2U : first < 0xF0 ? 3U : 4U;
        result.push_back(text.substr(pos, width));
        pos += width;
    }
    return result;
}

inline bool IsPrefix(const std::string& input) {
    const std::string key = Normalize(input);
    if (key.empty()) { return false; }
    const Entry* entry = Find(key);
    return entry != kPinyin + kPinyinCount &&
           std::strncmp(entry->syllable, key.c_str(), key.size()) == 0;
}

enum class Result { Changed, Unchanged, Invalid, ByteLimit, CompositionPending, StaleView, Confirm };
enum class Mode { Pinyin, Latin, Symbols };

class Editor {
public:
    explicit Editor(std::size_t byte_limit, bool multiline, std::size_t page_size = 5)
        : limit_(byte_limit), multiline_(multiline), page_size_(page_size ? page_size : 5) {}
    const std::string& Text() const { return text_; }
    const std::string& Composition() const { return composition_; }
    std::size_t Cursor() const { return cursor_; }
    std::size_t PageIndex() const { return page_; }
    std::uint64_t Revision() const { return revision_; }
    Mode InputMode() const { return mode_; }

    Result Reset(const std::string& value, std::size_t cursor) {
        if (!ValidUtf8(value) || !Boundary(value, cursor) ||
            (!multiline_ && value.find('\n') != std::string::npos)) { return Result::Invalid; }
        if (value.size() > limit_) { return Result::ByteLimit; }
        text_ = value; cursor_ = cursor; composition_.clear(); page_ = 0;
        mode_ = Mode::Pinyin; ++revision_;
        return Result::Changed;
    }
    Result MoveCursor(std::size_t cursor, std::uint64_t displayed_revision) {
        if (displayed_revision != revision_) { return Result::StaleView; }
        if (!Boundary(text_, cursor)) { return Result::Invalid; }
        if (cursor_ == cursor) { return Result::Unchanged; }
        cursor_ = cursor; ++revision_; return Result::Changed;
    }
    Result SetMode(Mode mode) {
        if (!composition_.empty()) { return Result::CompositionPending; }
        if (mode_ == mode) { return Result::Unchanged; }
        mode_ = mode; page_ = 0; ++revision_; return Result::Changed;
    }
    Result TypeLetter(char letter) {
        if (!((letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z'))) {
            return Result::Invalid;
        }
        if (mode_ != Mode::Pinyin) { return InsertLiteral(std::string(1, letter)); }
        if (composition_.size() >= 6) { return Result::ByteLimit; }
        if (letter >= 'A' && letter <= 'Z') { letter = static_cast<char>(letter - 'A' + 'a'); }
        composition_.push_back(letter); page_ = 0; ++revision_; return Result::Changed;
    }
    Result InsertLiteral(const std::string& text) {
        if (!composition_.empty()) { return Result::CompositionPending; }
        return Insert(text);
    }
    Result Backspace() {
        if (!composition_.empty()) {
            composition_.pop_back(); page_ = 0; ++revision_; return Result::Changed;
        }
        if (cursor_ == 0) { return Result::Unchanged; }
        std::size_t start = cursor_ - 1;
        while (start > 0 && (static_cast<unsigned char>(text_[start]) & 0xC0U) == 0x80U) { --start; }
        text_.erase(start, cursor_ - start); cursor_ = start; ++revision_; return Result::Changed;
    }
    Result ClearComposition() {
        if (composition_.empty()) { return Result::Unchanged; }
        composition_.clear(); page_ = 0; ++revision_; return Result::Changed;
    }
    std::vector<std::string> VisibleCandidates() const {
        auto all = Candidates(composition_);
        const std::size_t begin = page_ * page_size_;
        if (begin >= all.size()) { return {}; }
        const std::size_t end = begin + std::min(page_size_, all.size() - begin);
        return {all.begin() + static_cast<std::ptrdiff_t>(begin), all.begin() + static_cast<std::ptrdiff_t>(end)};
    }
    Result AdvancePage(int direction, std::uint64_t displayed_revision) {
        if (displayed_revision != revision_) { return Result::StaleView; }
        if (direction != 1 && direction != -1) { return Result::Invalid; }
        if (direction < 0) {
            if (!page_) { return Result::Unchanged; }
            --page_;
        } else {
            const auto count = Candidates(composition_).size();
            if (!count || page_ >= (count - 1) / page_size_) { return Result::Unchanged; }
            ++page_;
        }
        ++revision_; return Result::Changed;
    }
    Result SelectVisible(std::size_t index, std::uint64_t displayed_revision) {
        if (displayed_revision != revision_) { return Result::StaleView; }
        const auto candidates = VisibleCandidates();
        if (index >= candidates.size()) { return Result::Invalid; }
        const Result result = Insert(candidates[index]);
        if (result == Result::Changed) { composition_.clear(); page_ = 0; }
        return result;
    }
    Result Space(std::uint64_t displayed_revision) {
        if (displayed_revision != revision_) { return Result::StaleView; }
        return composition_.empty() ? InsertLiteral(" ") : SelectVisible(0, displayed_revision);
    }
    Result Digit(char digit, std::uint64_t displayed_revision) {
        if (displayed_revision != revision_) { return Result::StaleView; }
        if (digit < '0' || digit > '9') { return Result::Invalid; }
        if (composition_.empty()) { return InsertLiteral(std::string(1, digit)); }
        if (digit < '1' || digit > '5') { return Result::CompositionPending; }
        return SelectVisible(static_cast<std::size_t>(digit - '1'), displayed_revision);
    }
    Result Enter(std::uint64_t displayed_revision) {
        if (displayed_revision != revision_) { return Result::StaleView; }
        if (!composition_.empty()) {
            const Result result = Insert(composition_);
            if (result == Result::Changed) { composition_.clear(); page_ = 0; }
            return result;
        }
        return multiline_ ? Insert("\n") : Result::Confirm;
    }
private:
    static bool Boundary(const std::string& text, std::size_t offset) {
        return offset <= text.size() && (offset == text.size() ||
               (static_cast<unsigned char>(text[offset]) & 0xC0U) != 0x80U);
    }
    Result Insert(const std::string& value) {
        if (!ValidUtf8(value) || (!multiline_ && value.find('\n') != std::string::npos)) { return Result::Invalid; }
        if (value.size() > limit_ - text_.size()) { return Result::ByteLimit; }
        if (value.empty()) { return Result::Unchanged; }
        text_.insert(cursor_, value); cursor_ += value.size(); ++revision_;
        return Result::Changed;
    }
    std::size_t limit_, cursor_ = 0, page_ = 0;
    bool multiline_;
    std::size_t page_size_;
    Mode mode_ = Mode::Pinyin;
    std::uint64_t revision_ = 1;
    std::string text_, composition_;
};
}  // namespace metalio_plus_ime
