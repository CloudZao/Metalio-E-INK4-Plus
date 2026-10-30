#include "assets/lang_workbuddy.h"
#include "assets/lang_config.h"
#include "workbuddy_chat.h"

#include "fontpack_lvgl.h"
#include "home_icon_source.h"
#include "screen_common.h"
#include "text_encoding.h"
#include "ui_scale.h"
#include "vertical_text.h"
#include "workbuddy/workbuddy_client.h"

#include <algorithm>
#include <cstdio>
#include <new>
#include <string_view>
#include <esp_log.h>
#include <esp_random.h>

namespace {
constexpr size_t kWelcomeMessageCount = 3; // 欢迎语数量

const char* WelcomeMessage(size_t index) {
    const char* messages[] = {Lang::Strings::WB_WELCOME_READY, Lang::Strings::WB_WELCOME_TASK,
                              Lang::Strings::WB_WELCOME_IDEAS};
    static_assert(sizeof(messages) / sizeof(messages[0]) == kWelcomeMessageCount);
    return messages[index % kWelcomeMessageCount];
}
constexpr size_t kMessageBytes = 4096; // 单条消息的 UTF-8 字节上限
constexpr int32_t kPad = 20; // 气泡内边距
constexpr int32_t kLineSpace = 8; // 物理行间距
constexpr int32_t kGap = 24; // 相邻消息间距

std::string_view BoundedText(const std::string& text) {
    size_t size = std::min(text.size(), kMessageBytes);
    while (size && size < text.size() && (static_cast<uint8_t>(text[size]) & 0xc0) == 0x80) --size;
    return std::string_view(text.data(), size);
}

const lv_font_t* Font() {
    const auto* font = fontpack_lv_font_ui();
    return font ? font : LV_FONT_DEFAULT;
}

int32_t SpeakerHeight() { return Font()->line_height + 8; }

int32_t Width(std::string_view text) {
    int32_t width = 0;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = 0;
        const size_t n = reader::Utf8Next(reinterpret_cast<const uint8_t*>(text.data() + i), text.size() - i, &cp);
        if (!n) break;
        width += lv_font_get_glyph_width(Font(), cp, 0);
        i += n;
    }
    return width;
}

bool IsLatinWord(uint32_t cp) {
    return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
           (cp >= '0' && cp <= '9') || cp == '_' || cp == '\'';
}

void Passive(lv_obj_t* object) {
    lv_obj_remove_flag(object, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
}

lv_obj_t* Text(lv_obj_t* parent, const char* text, int32_t width, const lv_font_t* font = Font()) {
    auto* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_line_space(label, kLineSpace, 0);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    Passive(label);
    return label;
}

lv_obj_t* Rectangle(lv_obj_t* parent, int32_t x, int32_t y, int32_t width, int32_t height, uint32_t color) {
    auto* object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    Passive(object);
    return object;
}
}

void WorkBuddyChat::Create(lv_obj_t* parent) {
    welcome_index_ = esp_random() % kWelcomeMessageCount;
    intro_ = Lang::Strings::WB_LINK_CHECKING;
    lv_obj_update_layout(parent);
    body_ = lv_obj_create(parent);
    lv_obj_remove_style_all(body_);
    lv_obj_set_size(body_, lv_obj_get_content_width(parent), lv_obj_get_content_height(parent) - UiSy(44));
    Passive(body_);
    pager_ = Text(parent, "1 / 1", lv_obj_get_content_width(parent));
    lv_obj_set_style_text_align(pager_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(pager_, LV_ALIGN_BOTTOM_MID, 0, -UiSy(5));
    Paint();
}

void WorkBuddyChat::Clear() {
    decltype(messages_){}.swap(messages_);
    decltype(fragments_){}.swap(fragments_);
    intro_.clear();
    page_ = 0;
    page_count_ = 1;
    anchor_valid_ = false;
    Paint();
}

void WorkBuddyChat::ShowIntro(const std::string& text) {
    if (intro_ == text) return;
    intro_ = text;
    Paint();
}

bool WorkBuddyChat::DropOldest() {
    if (messages_.empty()) return false;
    if (!anchor_valid_) {
        for (const auto& fragment : fragments_) {
            if (fragment.page != page_ || fragment.message >= messages_.size()) continue;
            const auto& message = messages_[fragment.message];
            anchor_id_ = message.id;
            anchor_mine_ = message.mine;
            anchor_byte_ = fragment.text_begin;
            anchor_valid_ = true;
            break;
        }
    }
    messages_.erase(messages_.begin());
    decltype(fragments_){}.swap(fragments_);
    if (messages_.empty()) decltype(messages_){}.swap(messages_);
    return true;
}

bool WorkBuddyChat::StoreMessage(uint32_t id, bool mine, const std::string& text) {
    const auto bounded = BoundedText(text);
    for (;;) {
        try {
            auto found = std::find_if(messages_.begin(), messages_.end(), [id, mine](const Message& m) {
                return m.id == id && m.mine == mine;
            });
            workbuddy_history::String next(bounded.data(), bounded.size());
            if (found == messages_.end()) messages_.push_back({id, mine, std::move(next), {}, 0});
            else {
                found->text = std::move(next);
                found->layout_width = 0;
            }
            return true;
        } catch (const std::bad_alloc&) {
            // 分配包含字符串和容器扩容的峰值；先归还最旧消息，再尝试申请。
            if (!DropOldest()) return false;
        }
    }
}

bool WorkBuddyChat::ReflowWithTrim(bool follow) {
    for (;;) {
        try {
            Reflow(follow);
            return true;
        } catch (const std::bad_alloc&) {
            if (messages_.size() > 1) {
                DropOldest();
                continue;
            }
            decltype(fragments_){}.swap(fragments_);
            page_ = 0;
            page_count_ = 1;
            intro_ = Lang::Strings::WB_HISTORY_MEMORY;
            return false;
        }
    }
}

void WorkBuddyChat::SetMessage(uint32_t id, bool mine, const std::string& text, bool follow) {
    if (text.empty()) return;
    const auto found = std::find_if(messages_.begin(), messages_.end(), [id, mine](const Message& m) {
        return m.id == id && m.mine == mine;
    });
    if (found != messages_.end() && intro_.empty() &&
            std::string_view(found->text.data(), found->text.size()) == BoundedText(text)) {
        if (follow) { ReflowWithTrim(true); Paint(); }
        return;
    }
    if (!StoreMessage(id, mine, text)) {
        ScreenShowToast(Lang::Strings::WB_HISTORY_MEMORY);
        ReflowWithTrim(false);
        Paint();
        return;
    }
    intro_.clear();
    if (!ReflowWithTrim(follow)) ScreenShowToast(Lang::Strings::WB_HISTORY_MEMORY);
    Paint();
}

bool WorkBuddyChat::ReplaceMessages(const std::vector<workbuddy::LocalMessage>& messages) {
    // 网络结果仍持有完整快照，释放旧缓存后逐条接收，避免双份历史挤占预算。
    decltype(messages_){}.swap(messages_);
    decltype(fragments_){}.swap(fragments_);
    intro_.clear();
    page_ = 0;
    page_count_ = 1;
    anchor_valid_ = false;
    uint32_t id = 0;
    for (const auto& message : messages) {
        if (message.text.empty()) continue;
        if (!StoreMessage(++id, message.mine, message.text)) {
            ReflowWithTrim(false);
            Paint();
            return false;
        }
    }
    const bool ready = ReflowWithTrim(true);
    page_ = page_count_ - 1;
    Paint();
    return ready;
}

void WorkBuddyChat::Layout(Message& message, int32_t width) {
    if (message.layout_width == width) return;
    message.layout_width = 0;
    message.lines.clear();
    const std::string_view text(message.text);
    const int32_t room = std::max<int32_t>(1, width - 2);
    size_t begin = 0;
    for (;;) {
        size_t end = begin, last_break = std::string_view::npos;
        int32_t used = 0;
        uint32_t previous = 0;
        while (end < text.size() && text[end] != '\n' && text[end] != '\r') {
            uint32_t cp = 0;
            const size_t n = reader::Utf8Next(reinterpret_cast<const uint8_t*>(text.data() + end), text.size() - end, &cp);
            if (!n) break;
            // 复用阅读器标点禁则，优先在英文单词之间断行。
            if (previous && reader::VerticalCanBreak(previous, cp) &&
                    !(IsLatinWord(previous) && IsLatinWord(cp))) last_break = end;
            const int32_t advance = lv_font_get_glyph_width(Font(), cp, 0);
            if (used + advance > room && end != begin) break;
            used += advance;
            previous = cp;
            end += n;
        }
        if (end < text.size() && text[end] != '\n' && text[end] != '\r' &&
                last_break != std::string_view::npos && last_break > begin) end = last_break;
        message.lines.push_back({static_cast<uint16_t>(begin), static_cast<uint16_t>(end),
                         static_cast<int16_t>(Width(text.substr(begin, end - begin)))});
        if (end == text.size()) break;
        begin = end;
        if (text[begin] == '\r') {
            ++begin;
            if (begin < text.size() && text[begin] == '\n') ++begin;
        } else if (text[begin] == '\n') ++begin;
    }
    message.layout_width = width;
}

void WorkBuddyChat::Reflow(bool follow) {
    uint32_t anchor_id = anchor_id_;
    bool anchor_mine = anchor_mine_, have_anchor = anchor_valid_;
    size_t anchor_byte = anchor_byte_;
    for (const auto& f : fragments_) {
        if (have_anchor) break;
        if (f.page == page_ && f.message < messages_.size()) {
            anchor_id = messages_[f.message].id;
            anchor_mine = messages_[f.message].mine;
            anchor_byte = f.text_begin;
            have_anchor = true;
            break;
        }
    }
    if (follow || !have_anchor) {
        for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
            if (it->mine) {
                anchor_id = it->id;
                anchor_mine = true;
                anchor_byte = 0;
                have_anchor = true;
                break;
            }
        }
    }
    anchor_id_ = anchor_id;
    anchor_mine_ = anchor_mine;
    anchor_byte_ = anchor_byte;
    anchor_valid_ = have_anchor;
    fragments_.clear();
    lv_obj_update_layout(body_);
    const int32_t text_width = std::max<int32_t>(1, lv_obj_get_width(body_) - UiSx(74) - kPad * 2);
    const int32_t available = std::max<int32_t>(1, lv_obj_get_height(body_) - UiSy(12));
    size_t current_page = 0;
    int32_t y = UiSy(8);
    for (size_t i = 0; i < messages_.size(); ++i) {
        auto& message = messages_[i];
        Layout(message, text_width);
        size_t begin = 0;
        while (begin < message.lines.size()) {
            const int32_t room = available - y - SpeakerHeight() - kPad * 2 - kGap;
            int32_t used = 0;
            size_t end = begin;
            while (end < message.lines.size()) {
                const int32_t gap = end == begin ? 0 : kLineSpace;
                const int32_t height = Font()->line_height;
                if (used + gap + height > room) break;
                used += gap + height;
                ++end;
            }
            if (end == begin && y > UiSy(8)) { ++current_page; y = UiSy(8); continue; }
            if (end == begin) { end = begin + 1; used = Font()->line_height; }
            fragments_.push_back({i, begin, end, message.lines[begin].begin, current_page, y, used + kPad * 2});
            y += SpeakerHeight() + used + kPad * 2 + kGap;
            begin = end;
            if (begin < message.lines.size()) { ++current_page; y = UiSy(8); }
        }
    }
    page_count_ = current_page + 1;
    page_ = std::min(page_, page_count_ - 1);
    if (have_anchor) {
        for (const auto& f : fragments_) {
            const auto& m = messages_[f.message];
            if (m.id != anchor_id || m.mine != anchor_mine) continue;
            page_ = f.page;
            if (anchor_byte <= f.text_begin || anchor_byte < m.lines[f.end - 1].end) break;
        }
    } else if (follow) page_ = page_count_ - 1;
    anchor_valid_ = false;
}

void WorkBuddyChat::Paint() try {
    if (!body_) return;
    lv_obj_update_layout(body_);
    lv_obj_clean(body_);
    char number[32];
    std::snprintf(number, sizeof(number), "%u / %u", static_cast<unsigned>(page_ + 1), static_cast<unsigned>(page_count_));
    lv_label_set_text(pager_, number);
    if (!intro_.empty() && messages_.empty()) {
        auto* icon = lv_image_create(body_);
        lv_image_set_src(icon, HomeIconSource("A:ic_app_home_workbuddy.spng"));
        lv_obj_set_style_image_recolor(icon, lv_color_black(), 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        lv_obj_center(icon);
        Passive(icon);
        return;
    }
    if (!intro_.empty() || messages_.empty()) {
        auto* label = Text(body_, intro_.empty() ? WelcomeMessage(welcome_index_) : intro_.c_str(), lv_obj_get_width(body_) - UiSx(40));
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(label);
        return;
    }
    const int32_t max_bubble_width = lv_obj_get_width(body_) - UiSx(74);
    for (const auto& f : fragments_) {
        if (f.page != page_) continue;
        const auto& m = messages_[f.message];
        int32_t bubble_width = max_bubble_width;
        if (f.end == f.begin + 1) {
            bubble_width = std::clamp<int32_t>(m.lines[f.begin].width + kPad * 2 + 2, UiSx(60), max_bubble_width);
        }
        const int32_t x = m.mine ? lv_obj_get_width(body_) - UiSx(16) - bubble_width : UiSx(16);
        auto* name = Text(body_, m.mine ? Lang::Strings::WB_ME_PREFIX : Lang::Strings::WB_ASSISTANT_PREFIX, max_bubble_width);
        lv_obj_set_style_text_align(name, m.mine ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_pos(name, m.mine ? lv_obj_get_width(body_) - UiSx(16) - max_bubble_width : UiSx(16), f.y);
        auto* bubble = Rectangle(body_, x, f.y + SpeakerHeight(), bubble_width, f.height, m.mine ? 0xeeeeee : 0xffffff);
        lv_obj_set_style_radius(bubble, UiSx(7), 0);
        lv_obj_set_style_border_color(bubble, lv_color_black(), 0);
        lv_obj_set_style_border_width(bubble, 1, 0);
        int32_t y = kPad;
        for (size_t i = f.begin; i < f.end; ++i) {
            const auto& line = m.lines[i];
            if (i != f.begin) y += kLineSpace;
            auto* label = Text(bubble, "", bubble_width - kPad * 2);
            lv_label_set_text_fmt(label, "%.*s", static_cast<int>(line.end - line.begin), m.text.c_str() + line.begin);
            lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_letter_space(label, 0, 0);
            lv_obj_set_style_text_line_space(label, 0, 0);
            lv_obj_set_pos(label, kPad, y);
            lv_obj_set_height(label, Font()->line_height);
            y += Font()->line_height;
        }
    }
} catch (const std::bad_alloc&) {
    ESP_LOGW("WorkBuddyChat", "page paint allocation failed");
}

bool WorkBuddyChat::TurnPage(int delta) {
    if (!intro_.empty()) return false;
    const auto next = static_cast<size_t>(std::clamp(static_cast<int>(page_) + delta, 0, static_cast<int>(page_count_ - 1)));
    if (next == page_) return false;
    page_ = next;
    Paint();
    return true;
}
