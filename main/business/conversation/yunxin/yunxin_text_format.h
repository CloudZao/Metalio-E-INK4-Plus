#pragma once

#include <string>
#include <string_view>
#include "business/conversation/chat_memory.h"

// 格式化只追加内容，已展示的文本前缀保持稳定，供增量排版使用。
class YunxinTextFormatter {
public:
    /** @brief 追加文本，每累计四个中文句号后在后续正文前分段，不回改已展示前缀 */
    void Append(std::string_view text) {
        for (size_t i = 0; i < text.size();) {
            if (pending_break_) {
                if (text[i] == '\r' || text[i] == '\n' || text[i] == ' ' || text[i] == '\t') {
                    ++i;
                    continue;
                }
                if (text.substr(i, 3) == "　") {
                    i += 3;
                    continue;
                }
                display_ += "\n　　";
                pending_break_ = false;
            }
            if (text.substr(i, 3) == "。") {
                display_ += "。";
                i += 3;
                if (++periods_ == 4) {
                    periods_ = 0;
                    pending_break_ = true;
                }
            } else {
                display_ += text[i++];
            }
        }
    }

    /** @brief 借用累计的格式化文本 */
    const ChatString& Text() const {
        return display_;
    }

    /** @brief 清空文本、句号计数和待插入的段落边界 */
    void Reset() {
        display_.clear();
        periods_ = 0;
        pending_break_ = false;
    }

private:
    ChatString display_;
    unsigned periods_ = 0;
    bool pending_break_ = false;
};
