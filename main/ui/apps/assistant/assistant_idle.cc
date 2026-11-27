#include <algorithm>
#include <cstring>
#include "assets/lang_config.h"
#include "ui/apps/assistant/assistant_screen_priv.h"
#include "business/conversation/chat_cloud.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/drawing/ui_theme.h"

// Layout coordinates use a 480 x 800 reference; content is editable independently
// of the resource partition. Icons are LVGL strokes based on the supplied artwork.
static constexpr int kReferenceWidth = 480;
static constexpr int kReferenceHeight = 800;
static constexpr int kTalkRightMargin = 16;
static constexpr int kRuleWidth = 2;

static bool IsEnglish() {
    return LangCode && std::strcmp(LangCode, "en-US") == 0;
}

struct AssistantIdleFeature {
    const char* title_zh;
    const char* detail_zh;
    const char* title_en;
    const char* detail_en;
};

static constexpr AssistantIdleFeature kFeatures[] = {
    {"记录待办", "帮我把重要的事项整理成待办", "To-do list", "Organize important things into tasks"},
    {"旅游助手", "攻略、美食、天气、交通，一键规划", "Travel assistant", "Plan food, weather and transport"},
    {"探索世界", "解答各种疑问，探索未知", "Explore the world", "Ask questions and discover more"},
    {"陪伴聊天", "放松交流、分享心情、打发时间", "Let's chat", "Relax, share feelings and pass the time"},
};

static void DrawIcon(lv_event_t* e) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_area_t bounds;
    lv_obj_get_coords(obj, &bounds);
    lv_layer_t* layer = lv_event_get_layer(e);
    const int icon_size = lv_obj_get_width(obj);
    auto scale = [icon_size](int value) { return value * icon_size / 52; };
    const int kind = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    auto line = [&](int x1, int y1, int x2, int y2) {
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.color = UiColorForeground();
        d.width = std::max(2, scale(3));
        d.round_start = d.round_end = true;
        d.p1.x = bounds.x1 + scale(x1);
        d.p1.y = bounds.y1 + scale(y1);
        d.p2.x = bounds.x1 + scale(x2);
        d.p2.y = bounds.y1 + scale(y2);
        lv_draw_line(layer, &d);
    };
    auto arc = [&](int x, int y, int radius, int start, int end, int width = 3) {
        lv_draw_arc_dsc_t d;
        lv_draw_arc_dsc_init(&d);
        d.color = UiColorForeground();
        d.width = std::max(2, scale(width));
        d.center = {bounds.x1 + scale(x), bounds.y1 + scale(y)};
        d.radius = scale(radius);
        d.start_angle = start;
        d.end_angle = end;
        lv_draw_arc(layer, &d);
    };
    auto box = [&](int x, int y, int w, int h) {
        line(x, y, x + w, y);
        line(x + w, y, x + w, y + h);
        line(x + w, y + h, x, y + h);
        line(x, y + h, x, y);
    };
    switch (kind) {
        case 0:
            arc(25, 22, 17, 0, 360);
            arc(25, 22, 12, 240, 320);
            line(20, 43, 30, 43);
            line(22, 48, 28, 48);
            line(4, 7, 8, 9);
            line(3, 14, 7, 15);
            line(42, 9, 46, 7);
            line(43, 15, 47, 14);
            break;
        case 1:
            box(8, 4, 13, 41);
            box(21, 14, 22, 31);
            line(33, 8, 43, 8);
            line(43, 8, 43, 45);
            for (int y = 20; y < 42; y += 7) {
                line(27, y, 33, y);
            }
            break;
        case 2:
            line(18, 6, 45, 6);
            line(45, 6, 45, 27);
            line(45, 27, 40, 27);
            line(6, 16, 36, 16);
            line(36, 16, 36, 38);
            line(36, 38, 23, 38);
            line(23, 38, 17, 44);
            line(17, 44, 17, 38);
            line(17, 38, 6, 38);
            line(6, 38, 6, 16);
            arc(21, 25, 5, 180, 360);
            line(26, 25, 21, 30);
            line(21, 30, 21, 32);
            line(21, 35, 21, 35);
            break;
        case 3:
            arc(13, 15, 11, 0, 360);
            arc(39, 15, 11, 0, 360, 11);
            arc(23, 42, 15, 180, 265);
            arc(39, 42, 13, 180, 360);
            break;
    }
}

static lv_obj_t* Label(lv_obj_t* host, const char* text, int x, int y, int width, const lv_font_t* font) {
    lv_obj_t* label = lv_label_create(host);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, UiColorForeground(), 0);
    lv_obj_set_style_text_line_space(label, 3, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    lv_obj_clear_flag(label, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    return label;
}

static lv_obj_t* PlainObject(lv_obj_t* parent, int width, int height) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, width, height);
    lv_obj_clear_flag(obj, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    return obj;
}

static void Rule(lv_obj_t* parent, int x, int y, int width, int height) {
    lv_obj_t* rule = PlainObject(parent, width, height);
    lv_obj_set_pos(rule, x, y);
    lv_obj_set_style_bg_color(rule, UiColorForeground(), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
}

const char* AssistantScreen::DisplayName(bool compact) {
    const bool en = IsEnglish();
    switch (ChatCloudActiveProvider()) {
        case ChatCloudProvider::Xiaozhi:
            return en ? (compact ? "XZ AI" : "Xiaozhi AI") : "小智AI";
        case ChatCloudProvider::Yunxin:
            return en ? (compact ? "NE AI" : "NetEase AI") : "网易AI";
        default:
            return en ? (compact ? "BW AI" : "Baiwen AI") : "百问AI";
    }
}

void Assistant_HideIdleHint() {
    lv_obj_t* hint = Assistant_State().chrome.idle_hint;
    if (hint != nullptr && lv_obj_is_valid(hint)) {
        lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
    }
}

void Assistant_ShowIdleHint() {
    lv_obj_t* hint = Assistant_State().chrome.idle_hint;
    if (hint != nullptr && lv_obj_is_valid(hint)) {
        lv_obj_remove_flag(hint, LV_OBJ_FLAG_HIDDEN);
    }
}

void Assistant_BuildIdleHint(lv_obj_t* host) {
    const bool en = IsEnglish();
    const auto provider = ChatCloudActiveProvider();
    const bool baiwen = provider == ChatCloudProvider::Baiwen;
    lv_obj_update_layout(host);
    const int width = lv_obj_get_width(host);
    const int height = lv_obj_get_height(host);
    auto x = [width](int value) { return value * width / kReferenceWidth; };
    auto y = [height](int value) { return value * height / kReferenceHeight; };
    const lv_font_t* regular = UiFont();  // Existing 25px UI font.
    if (!regular) {
        regular = Assistant_UiFont();
    }
    const lv_font_t* bold = Assistant_UiFontBold();
    lv_obj_set_style_bg_color(host, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(host, LV_OPA_COVER, 0);
    const int pad = x(25), gap = std::max(4, y(12));
    lv_obj_add_flag(host, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(host, LV_DIR_VER);
    auto* talk = Label(host, en ? "Hold\nto talk" : "长\n按\n对\n话", 0, y(135), LV_SIZE_CONTENT, regular);
    lv_obj_set_style_text_align(talk, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_update_layout(talk);
    const int short_line_x = width - kTalkRightMargin - kRuleWidth;
    const int long_line_x = short_line_x - 12;
    const int talk_left = long_line_x - 7 - lv_obj_get_width(talk);
    lv_obj_set_x(talk, talk_left);
    const int talk_center = lv_obj_get_y(talk) + lv_obj_get_height(talk) / 2;
    Rule(host, long_line_x, talk_center - y(32), kRuleWidth, y(64));
    Rule(host, short_line_x, talk_center - y(18), kRuleWidth, y(36));
    const int heading_width = std::max(1, talk_left - 2 * pad);
    auto* name = Label(host, AssistantScreen::DisplayName(), pad, y(65), heading_width, bold);
    lv_obj_update_layout(name);
    auto* greeting = Label(host, en ? "Hello,\nI'm your AI assistant" : "你好，\n我是你的智能助手", pad,
                           lv_obj_get_y(name) + lv_obj_get_height(name) + gap, heading_width, bold);
    lv_obj_update_layout(greeting);
    const int prompt_top = std::max<int>(lv_obj_get_y(greeting) + lv_obj_get_height(greeting),
                                         lv_obj_get_y(talk) + lv_obj_get_height(talk)) +
                           gap;
    auto* prompt =
        Label(host, en ? "How can I help you?" : "有什么可以帮你？", pad, prompt_top, width - 2 * pad, regular);
    lv_obj_update_layout(prompt);
    const int first = baiwen ? 0 : 1;
    const int count = 4 - first;
    int top = lv_obj_get_y(prompt) + lv_obj_get_height(prompt) + gap;
    const int list_bottom = height - kFooterH - kPageIndicatorH - y(8);
    const int minimum_row = std::max(1, (list_bottom - top) / count);
    for (int row = 0; row < count; ++row) {
        const int i = first + row;
        const int item_width = width - 2 * pad;
        auto* item = PlainObject(host, item_width, LV_SIZE_CONTENT);
        lv_obj_set_pos(item, pad, top);
        lv_obj_set_style_min_height(item, minimum_row, 0);
        lv_obj_set_style_pad_ver(item, gap, 0);
        lv_obj_set_style_pad_column(item, x(24), 0);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        const int icon_edge = std::min(x(52), y(52));
        auto* icon = PlainObject(item, icon_edge, icon_edge);
        lv_obj_add_event_cb(icon, DrawIcon, LV_EVENT_DRAW_MAIN, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        auto* text = PlainObject(item, item_width - icon_edge - x(24), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(text, 4, 0);
        Label(text, en ? kFeatures[i].title_en : kFeatures[i].title_zh, 0, 0, item_width - icon_edge - x(24), bold);
        Label(text, en ? kFeatures[i].detail_en : kFeatures[i].detail_zh, 0, 0, item_width - icon_edge - x(24),
              regular);
        if (row + 1 < count) {
            lv_obj_set_style_border_side(item, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(item, kRuleWidth, 0);
            lv_obj_set_style_border_color(item, UiColorForeground(), 0);
        }
        lv_obj_update_layout(item);
        top += lv_obj_get_height(item);
    }
}
