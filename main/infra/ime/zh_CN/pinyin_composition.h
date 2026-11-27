#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include "infra/ime/zh_CN/pinyin_dictionary.h"

class PinyinComposition {
public:
    static constexpr size_t kMaxLetters = PinyinDictionary::kMaxKeyBytes;  // 连续拼音及英文组合上限

    /** @brief 追加字母，保留实际大小写用于输入框显示 */
    bool TypeLetter(char letter) {
        const size_t length = std::strlen(letters_);
        if (length == kMaxLetters || !((letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z'))) {
            return false;
        }
        letters_[length] = letter;
        letters_[length + 1] = '\0';
        candidates_ = {};
        return true;
    }

    /** @brief 删除组合串最后一个字母 */
    void Backspace() {
        const size_t length = std::strlen(letters_);
        if (length) {
            letters_[length - 1] = '\0';
        }
        candidates_ = {};
    }

    /** @brief 清除候选与组合状态，不修改宿主输入框 */
    void Clear() {
        letters_[0] = '\0';
        candidates_ = {};
    }

    /** @brief 引用已校验的候选块，宿主须持有其内存直到下一次查询 */
    bool SetCandidates(std::string_view data) {
        candidates_ = {};
        if (!ValidWords(data)) {
            return false;
        }
        candidates_ = data;
        return true;
    }

    /** @brief 顺序取得完整词条；游标为候选块字节位置，英数模式仅返回英文单词 */
    std::string_view NextCandidate(size_t& cursor, bool chinese) const {
        if (!letters_[0]) {
            return {};
        }
        const bool literal_first = !chinese || candidates_.empty() ||
                                   static_cast<unsigned char>(candidates_[0]) < 0x80;
        if (literal_first && cursor == 0) {
            cursor = 1;
            return letters_;
        }
        size_t position = literal_first ? cursor - 1 : cursor;
        while (position < candidates_.size()) {
            const size_t end = candidates_.find(' ', position);
            const size_t next = end == std::string_view::npos ? candidates_.size() : end + 1;
            const size_t word_end = end == std::string_view::npos ? candidates_.size() : end;
            const auto word = candidates_.substr(position, word_end - position);
            position = next;
            cursor = literal_first ? next + 1 : next;
            const bool english = static_cast<unsigned char>(word[0]) < 0x80;
            if ((!chinese && !english) || MatchesLetters(word)) {
                continue;
            }
            return word;
        }
        if (!literal_first && cursor <= candidates_.size()) {
            cursor = candidates_.size() + 1;
            return letters_;
        }
        return {};
    }

    /** @brief 获取输入框内尚未提交的字母片段 */
    const char* Letters() const {
        return letters_;
    }

private:
    bool MatchesLetters(std::string_view word) const {
        if (word.size() != std::strlen(letters_)) {
            return false;
        }
        for (size_t i = 0; i < word.size(); ++i) {
            const char c = letters_[i] >= 'A' && letters_[i] <= 'Z' ? letters_[i] + ('a' - 'A') : letters_[i];
            if (word[i] != c) {
                return false;
            }
        }
        return true;
    }

    static bool ValidWords(std::string_view data) {
        size_t characters = 0;
        for (size_t i = 0; i < data.size();) {
            const auto first = static_cast<unsigned char>(data[i]);
            if (first == ' ') {
                if (characters == 0) {
                    return false;
                }
                characters = 0;
                ++i;
                continue;
            }
            uint32_t cp;
            size_t width;
            if (first >= 'a' && first <= 'z') {
                cp = first;
                width = 1;
            } else if (first >= 0xe0 && first <= 0xef) {
                cp = first & 0x0f;
                width = 3;
            } else if (first >= 0xf0 && first <= 0xf4) {
                cp = first & 7;
                width = 4;
            } else {
                return false;
            }
            if (width > data.size() - i || ++characters > 64) {
                return false;
            }
            for (size_t j = 1; j < width; ++j) {
                const auto byte = static_cast<unsigned char>(data[i + j]);
                if ((byte & 0xc0) != 0x80) {
                    return false;
                }
                cp = (cp << 6) | (byte & 0x3f);
            }
            if ((width == 3 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) ||
                (width == 4 && (cp < 0x10000 || cp > 0x10ffff))) {
                return false;
            }
            i += width;
        }
        return data.empty() || characters != 0;
    }

    char letters_[kMaxLetters + 1] = {};  // 输入框内尚未提交的字母片段
    std::string_view candidates_;  // 查询结果按空格分隔，保持词条边界
};
