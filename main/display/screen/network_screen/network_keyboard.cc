// network_keyboard.cc — split from parent .cc
#include "network_screen/network_screen.h"
#include "network_screen_priv.h"
#include "network_keyboard.h"
#include "book_screen/book_text_util.h"

#include "assets/lang_config.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "epd_refresh.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>

static lv_timer_t* s_key_feedback_timer = nullptr;
struct KeyFeedback {
    uint32_t tick = 0; // 当前键最近一次按下时间
    uint32_t sequence = 0; // 按下时已提交的刷新序号
};
static KeyFeedback s_key_feedback[kKbKeysCap];
static KbAction s_key_pending_action = KbAction::kEmpty;

static void Network_ApplyKeyAction(KbAction action);

static void Network_StopKeyFeedback() {
    if (s_key_feedback_timer != nullptr) {
        lv_timer_delete(s_key_feedback_timer);
        s_key_feedback_timer = nullptr;
    }
    s_key_pending_action = KbAction::kEmpty;
}

static void Network_KeyFeedbackTick(lv_timer_t*) {
    if (Network_State().ui.pwd_keyboard == nullptr ||
        !lv_obj_is_visible(Network_State().ui.pwd_keyboard)) {
        Network_StopKeyFeedback();
        return;
    }
    const auto stats = EpdGetStats();
    bool pending = false;
    for (int i = 0; i < kKbKeysCap; ++i) {
        lv_obj_t* key = Network_State().kb_keys[i];
        if (key == nullptr || !lv_obj_has_state(key, LV_STATE_USER_1)) continue;
        const auto& feedback = s_key_feedback[i];
        // 每个键独立等待自身灰底落墨，不受后续按键计时或刷新忙状态影响。
        if (lv_obj_has_state(key, LV_STATE_PRESSED) ||
            lv_tick_elaps(feedback.tick) < 30 ||
            static_cast<int32_t>(stats.completed - feedback.sequence) <= 0) {
            pending = true;
        } else {
            lv_obj_remove_state(key, LV_STATE_USER_1);
        }
    }
    if (!pending) {
        const KbAction action = s_key_pending_action;
        Network_StopKeyFeedback();
        Network_ApplyKeyAction(action);
    }
}

static void Network_OnKeyFeedback(lv_event_t* e) {
    auto* key = static_cast<lv_obj_t*>(lv_event_get_target(e));
    for (int i = 0; i < kKbKeysCap; ++i) {
        if (Network_State().kb_keys[i] == key) {
            s_key_feedback[i] = {lv_tick_get(), EpdGetStats().submitted};
            break;
        }
    }
    lv_obj_add_state(key, LV_STATE_USER_1);
    lv_obj_invalidate(key);
    if (s_key_feedback_timer == nullptr) {
        s_key_feedback_timer = lv_timer_create(Network_KeyFeedbackTick, 30, nullptr);
    }
}

static const lv_image_dsc_t* Network_KeyPressedImage() {
    // 仅使用黑白像素合成 25% 灰度，保持 DU 刷新，不引入原生灰阶。
    static const uint8_t pixels[] = {
        0x00, 0xFF, 0x00, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF,
        0x00, 0xFF, 0x00, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF,
    };
    static const lv_image_dsc_t image = [] {
        lv_image_dsc_t dsc = {};
        dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc.header.cf = LV_COLOR_FORMAT_L8;
        dsc.header.w = 4;
        dsc.header.h = 4;
        dsc.header.stride = 4;
        dsc.data_size = sizeof(pixels);
        dsc.data = pixels;
        return dsc;
    }();
    return &image;
}

void Network_RefreshPasswordDisplay() {
    if (Network_State().ui.pwd_lbl == nullptr) {
        return;
    }
    // 光标仅用于显示，不写入密码缓冲；静态显示避免墨水屏周期刷新。
    const char* tail = Network_State().password;
    const lv_font_t* font = Network_UiFont();
    if (font == nullptr) {
        font = LV_FONT_DEFAULT;
    }
    const int width = lv_obj_get_content_width(Network_State().ui.pwd_box) - UiSx(12);
    lv_point_t size;
    while (true) {
        lv_text_get_size(&size, tail, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (width <= 0 || size.x <= width || *tail == '\0') {
            break;
        }
        do {
            ++tail;
        } while ((static_cast<unsigned char>(*tail) & 0xC0) == 0x80);
    }
    lv_label_set_text(Network_State().ui.pwd_lbl, tail);
    lv_obj_set_width(Network_State().ui.pwd_lbl, size.x > 0 ? size.x : 1);
}

void Network_AppendToPassword(const char* text) {
    if (Network_State().ui.pwd_lbl == nullptr || text == nullptr || text[0] == '\0') {
        return;
    }
    const size_t cur = std::strlen(Network_State().password);
    const size_t add = std::strlen(text);
    if (add == 0 || cur + add > kMaxPasswordLen) {
        return;
    }
    std::memcpy(Network_State().password + cur, text, add + 1);
    Network_RefreshPasswordDisplay();
}

void Network_BackspacePassword() {
    size_t len = std::strlen(Network_State().password);
    if (len == 0) {
        return;
    }
    // UTF-8：退掉末尾完整码点（中文标点等）
    do {
        --len;
    } while (len > 0 && (static_cast<unsigned char>(Network_State().password[len]) & 0xC0) == 0x80);
    Network_State().password[len] = '\0';
    Network_RefreshPasswordDisplay();
}

void Network_ClearPassword() {
    Network_State().password[0] = '\0';
    Network_RefreshPasswordDisplay();
}

void Network_OnBackspaceLongPressed(lv_event_t* /*e*/) {
    Network_ClearPassword();
}

void Network_FillKbLayout(KbKeyDesc out[kKbKeysCap]) {
    for (int i = 0; i < kKbKeysCap; ++i) {
        out[i] = {"", KbAction::kEmpty};
    }
    auto put = [&](int index, const char* label, KbAction action) {
        out[index] = {label, action};
    };
    auto chars = [&](int row, const char* text) {
        static char labels[kKbKeysCap][2];
        for (int c = 0; text[c] != '\0' && c < kKbCols; ++c) {
            const int index = row * kKbCols + c;
            labels[index][0] = text[c];
            labels[index][1] = '\0';
            put(index, labels[index], KbAction::kChar);
        }
    };
    if (Network_State().kb_mode == KbMode::kEn) {
        chars(0, Network_State().kb_upper ? "QWERTYUIOP" : "qwertyuiop");
        chars(1, Network_State().kb_upper ? "ASDFGHJKL" : "asdfghjkl");
        chars(2, Network_State().kb_upper ? " ZXCVBNM" : " zxcvbnm");
        put(20, "", KbAction::kShift);
        put(28, "", KbAction::kBackspace);
        put(30, "?123", KbAction::kModeSym);
        put(31, "Space", KbAction::kSpace);
        put(32, "", KbAction::kConfirm);
    } else {
        const bool more = Network_State().kb_mode == KbMode::kMore;
        chars(0, more ? "[]{}<>^*+=" : "1234567890");
        chars(1, more ? "_\\|~\x60%" : "-/:;()$&@");
        chars(2, ".,?!'\"#");
        put(27, "", KbAction::kBackspace);
        put(30, "abc", KbAction::kModeEn);
        put(31, more ? "123" : "#+=", more ? KbAction::kModeSym : KbAction::kModeMore);
        put(32, "Space", KbAction::kSpace);
        put(33, "", KbAction::kConfirm);
    }
}

// 字符/空格/退格：按下即写入（跟盖板键「按下即 Click」、拨号盘跟手一致）。
void Network_OnCustomKeyPressed(lv_event_t* e) {
    const auto action = static_cast<KbAction>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_obj_t* lbl = (btn != nullptr) ? lv_obj_get_child(btn, 0) : nullptr;
    const char* label = (lbl != nullptr) ? lv_label_get_text(lbl) : nullptr;

    switch (action) {
        case KbAction::kBackspace:
            Network_BackspacePassword();
            break;
        case KbAction::kSpace:
            Network_AppendToPassword(" ");
            break;
        case KbAction::kChar:
            if (label != nullptr) {
                Network_AppendToPassword(label);
            }
            break;
        case KbAction::kShift:
        case KbAction::kModeEn:
        case KbAction::kModeSym:
        case KbAction::kModeMore:
        case KbAction::kConfirm:
        case KbAction::kEmpty:
        default:
            break;
    }
}

// 切布局与确认等按下反馈完成后执行，避免重建覆盖尚未落墨的灰底。
void Network_OnCustomKeyClicked(lv_event_t* e) {
    const auto action = static_cast<KbAction>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    if (s_key_feedback_timer != nullptr) {
        s_key_pending_action = action;
        return;
    }
    Network_ApplyKeyAction(action);
}

static void Network_ApplyKeyAction(KbAction action) {
    switch (action) {
        case KbAction::kShift:
            Network_State().kb_upper = !Network_State().kb_upper;
            Network_RebuildCustomKeyboard();
            break;
        case KbAction::kModeEn:
            Network_State().kb_mode = KbMode::kEn;
            Network_RebuildCustomKeyboard();
            break;
        case KbAction::kModeSym:
            Network_State().kb_mode = KbMode::kSym;
            Network_RebuildCustomKeyboard();
            break;
        case KbAction::kModeMore:
            Network_State().kb_mode = KbMode::kMore;
            Network_RebuildCustomKeyboard();
            break;
        case KbAction::kConfirm:
            Network_OnPwdConnect(nullptr);
            break;
        default:
            break;
    }
}

static void Network_AddKeyIcon(lv_obj_t* btn, KbAction action) {
    static const lv_point_precise_t check[] = {{0, UiSx(11)}, {UiSx(8), UiSx(19)}, {UiSx(25), 0}};
    static const lv_point_precise_t shift[] = {
        {0, UiSx(12)}, {UiSx(12), 0}, {UiSx(24), UiSx(12)},
        {UiSx(17), UiSx(12)}, {UiSx(17), UiSx(24)},
        {UiSx(7), UiSx(24)}, {UiSx(7), UiSx(12)}, {0, UiSx(12)}};
    static const lv_point_precise_t erase[] = {
        {0, UiSx(10)}, {UiSx(9), 0}, {UiSx(29), 0},
        {UiSx(29), UiSx(20)}, {UiSx(9), UiSx(20)}, {0, UiSx(10)}};
    static const lv_point_precise_t cross_a[] = {{UiSx(14), UiSx(5)}, {UiSx(23), UiSx(15)}};
    static const lv_point_precise_t cross_b[] = {{UiSx(23), UiSx(5)}, {UiSx(14), UiSx(15)}};
    auto line = [&](lv_obj_t* parent, const lv_point_precise_t* points, int count) {
        lv_obj_t* obj = lv_line_create(parent);
        lv_line_set_points(obj, points, count);
        lv_obj_set_style_line_color(obj, lv_color_black(), 0);
        lv_obj_set_style_line_width(obj, UiSx(2), 0);
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        return obj;
    };
    lv_obj_t* icon = nullptr;
    if (action == KbAction::kConfirm) {
        icon = line(btn, check, 3);
    } else if (action == KbAction::kShift) {
        icon = line(btn, shift, 8);
    } else if (action == KbAction::kBackspace) {
        icon = line(btn, erase, 6);
        lv_obj_t* a = line(icon, cross_a, 2);
        lv_obj_t* b = line(icon, cross_b, 2);
        lv_obj_set_pos(a, 0, 0);
        lv_obj_set_pos(b, 0, 0);
    }
    if (icon != nullptr) {
        lv_obj_center(icon);
    }
}

void Network_LayoutCustomKeyboard() {
    if (Network_State().ui.pwd_keyboard == nullptr) {
        return;
    }
    const int w = lv_obj_get_content_width(Network_State().ui.pwd_keyboard);
    const int h = lv_obj_get_content_height(Network_State().ui.pwd_keyboard);
    const int key_h = (h - kKbGap * (kKbRows - 1)) / kKbRows;
    if (w <= 0 || key_h <= 0) {
        return;
    }
    for (int r = 0; r < kKbRows; ++r) {
        int count = 0;
        int weights[kKbCols] = {};
        int total = 0;
        for (int c = 0; c < kKbCols; ++c) {
            if (Network_State().kb_keys[r * kKbCols + c] == nullptr) {
                break;
            }
            int weight = 100;
            if (r == 2 && Network_State().kb_mode == KbMode::kEn && (c == 0 || c == 8)) {
                weight = 148;
            } else if (r == 2 && c == 7 && Network_State().kb_mode != KbMode::kEn) {
                weight = 148;
            } else if (r == 3) {
                const int space = Network_State().kb_mode == KbMode::kEn ? 1 : 2;
                weight = c < space ? 170 : c == space ? 480 : 200;
            }
            weights[count++] = weight;
            total += weight;
        }
        if (count == 0) {
            continue;
        }
        const bool inset = r == 1 || (r == 2 && Network_State().kb_mode != KbMode::kEn);
        const int row_w = inset ? w * 905 / 1000 : w;
        const int usable = row_w - kKbGap * (count - 1);
        if (usable <= 0) {
            continue;
        }
        int sum = 0;
        for (int c = 0; c < count; ++c) {
            const int left = usable * sum / total;
            sum += weights[c];
            lv_obj_t* key = Network_State().kb_keys[r * kKbCols + c];
            lv_obj_set_size(key, usable * sum / total - left, key_h);
            lv_obj_set_pos(key, (w - row_w) / 2 + left + c * kKbGap, r * (key_h + kKbGap));
        }
    }
    Network_RefreshPasswordDisplay();
}

void Network_OnKbAreaSizeChanged(lv_event_t* /*e*/) {
    Network_LayoutCustomKeyboard();
}

void Network_RebuildCustomKeyboard() {
    if (Network_State().ui.pwd_keyboard == nullptr) {
        return;
    }
    Network_StopKeyFeedback();
    lv_obj_clean(Network_State().ui.pwd_keyboard);
    for (int i = 0; i < kKbKeysCap; ++i) {
        Network_State().kb_keys[i] = nullptr;
    }

    const int nkeys = kKbKeysCap;

    KbKeyDesc layout[kKbKeysCap];
    Network_FillKbLayout(layout);

    const lv_font_t* font = Network_KbFont();
    for (int i = 0; i < nkeys; ++i) {
        const KbKeyDesc& d = layout[i];
        if (d.action == KbAction::kEmpty) {
            continue;
        }
        lv_obj_t* btn = lv_obj_create(Network_State().ui.pwd_keyboard);
        Network_Strip(btn);
        Network_State().kb_keys[i] = btn;
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, UiSx(7), 0);
        lv_obj_set_style_bg_image_src(btn, Network_KeyPressedImage(), LV_STATE_USER_1);
        lv_obj_set_style_bg_image_tiled(btn, true, LV_STATE_USER_1);
        lv_obj_set_style_bg_image_opa(btn, LV_OPA_COVER, LV_STATE_USER_1);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, Network_OnKeyFeedback, LV_EVENT_PRESSED, nullptr);
        HapticAttachClick(btn);
        void* ud = reinterpret_cast<void*>(static_cast<uintptr_t>(d.action));
        // 输入键按下即上屏；切 英/符/大小写 仍 CLICKED，防止重建时销毁按中的键
        if (d.action == KbAction::kChar || d.action == KbAction::kSpace ||
            d.action == KbAction::kBackspace) {
            lv_obj_add_event_cb(btn, Network_OnCustomKeyPressed, LV_EVENT_PRESSED, ud);
        } else if (d.action == KbAction::kShift || d.action == KbAction::kModeEn ||
                   d.action == KbAction::kModeSym || d.action == KbAction::kModeMore ||
                   d.action == KbAction::kConfirm) {
            lv_obj_add_event_cb(btn, Network_OnCustomKeyClicked, LV_EVENT_CLICKED, ud);
        }
        if (d.action == KbAction::kBackspace) {
            // 与电话拨号页一致：短按删一字，长按清空
            lv_obj_add_event_cb(btn, Network_OnBackspaceLongPressed, LV_EVENT_LONG_PRESSED, nullptr);
        }

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, d.label != nullptr ? d.label : "");
        if (font != nullptr) {
            lv_obj_set_style_text_font(lbl, font, 0);
        }
        lv_obj_center(lbl);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        Network_AddKeyIcon(btn, d.action);
        if (d.action == KbAction::kShift && Network_State().kb_upper) {
            lv_obj_set_style_border_width(btn, UiSx(3), 0);
        }
    }
    Network_LayoutCustomKeyboard();
}

void Network_OnPwdConnect(lv_event_t* /*e*/) {
    Network_ScheduleConnect(Network_State().pending_ssid, Network_State().password);
}

void Network_OnPwdCancel(lv_event_t* /*e*/) { Network_ClosePasswordPage(); }

void Network_ClosePasswordPage() {
    Network_StopKeyFeedback();
    for (lv_obj_t* key : Network_State().kb_keys) {
        if (key != nullptr) lv_obj_remove_state(key, LV_STATE_USER_1);
    }
    if (Network_State().transfer_mode && Network_State().ui.pwd_keyboard != nullptr) {
        lv_obj_clean(Network_State().ui.pwd_keyboard);
        for (auto& key : Network_State().kb_keys) key = nullptr;
    }
    if (Network_State().ui.pwd_page != nullptr) {
        lv_obj_add_flag(Network_State().ui.pwd_page, LV_OBJ_FLAG_HIDDEN);
    }
    if (Network_State().ui.list_panel != nullptr) {
        lv_obj_clear_flag(Network_State().ui.list_panel, LV_OBJ_FLAG_HIDDEN);
    }
    Network_ClearPassword();
    Network_State().kb_mode = KbMode::kEn;
    Network_State().kb_upper = false;
}

void Network_BuildPasswordPage(lv_obj_t* parent, lv_coord_t top_y) {
    lv_obj_t* page = lv_obj_create(parent);
    Network_Strip(page);
    Network_State().ui.pwd_page = page;
    lv_obj_set_size(page, LV_HOR_RES, LV_VER_RES - top_y);
    lv_obj_align(page, LV_ALIGN_TOP_MID, 0, top_y);
    lv_obj_set_style_bg_color(page, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(page, kPwdPagePad, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, kPwdPageRowGap, 0);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);

    const lv_font_t* ui = Network_UiFont();

    lv_obj_t* top = lv_obj_create(page);
    Network_Strip(top);
    lv_obj_set_width(top, LV_PCT(100));
    lv_obj_set_height(top, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(top, kPwdPageRowGap, 0);

    // 标题行显示连接目标，长 SSID 单行省略。
    lv_obj_t* title_row = lv_obj_create(top);
    Network_Strip(title_row);
    lv_obj_set_width(title_row, LV_PCT(100));
    lv_obj_set_height(title_row, UiSy(34));
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(title_row, UiSx(4), 0);
    lv_obj_clear_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);

    // 前缀固定宽度，长 SSID 时仍能看见「连接到：」
    lv_obj_t* prefix_lbl = lv_label_create(title_row);
    {
        char prefix[40];
        snprintf(prefix, sizeof(prefix), Lang::Strings::NETWORK_CONNECT_TO_FMT, "");
        lv_label_set_text(prefix_lbl, prefix);
    }
    if (ui != nullptr) {
        lv_obj_set_style_text_font(prefix_lbl, ui, 0);
    }
    lv_obj_set_style_text_color(prefix_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(prefix_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_flex_grow(prefix_lbl, 0, 0);

    Network_State().ui.pwd_ssid_lbl = lv_label_create(title_row);
    lv_label_set_text(Network_State().ui.pwd_ssid_lbl, "");
    // 宽度由 flex 分配，长 SSID 按实际可用宽度省略。
    lv_obj_set_flex_grow(Network_State().ui.pwd_ssid_lbl, 1);
    lv_obj_set_width(Network_State().ui.pwd_ssid_lbl, 1);
    lv_obj_set_style_min_width(Network_State().ui.pwd_ssid_lbl, UiSx(24), 0);
    lv_obj_set_height(Network_State().ui.pwd_ssid_lbl, UiSy(34));
    lv_label_set_long_mode(Network_State().ui.pwd_ssid_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(Network_State().ui.pwd_ssid_lbl, LV_TEXT_ALIGN_LEFT, 0);
    if (ui != nullptr) {
        lv_obj_set_style_text_font(Network_State().ui.pwd_ssid_lbl, ui, 0);
    }
    lv_obj_set_style_text_color(Network_State().ui.pwd_ssid_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(Network_State().ui.pwd_ssid_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(Network_State().ui.pwd_ssid_lbl, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* hint = lv_label_create(top);
    lv_label_set_text(hint, Lang::Strings::NETWORK_PWD_HINT);
    if (ui != nullptr) {
        lv_obj_set_style_text_font(hint, ui, 0);
    }

    // 与拨号号码行一致：label + 缓冲，按下即可刷字，无 textarea 光标/布局开销
    lv_obj_t* box = lv_obj_create(top);
    Network_Strip(box);
    Network_State().ui.pwd_box = box;
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_height(box, kPwdBoxH);
    lv_obj_set_style_bg_color(box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);
    lv_obj_set_style_border_width(box, kBtnBorderW, 0);
    lv_obj_set_style_pad_hor(box, kPwdBoxPadH, 0);
    lv_obj_set_style_pad_ver(box, kPwdBoxPadV, 0);
    lv_obj_set_style_pad_column(box, UiSx(2), 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    Network_State().ui.pwd_lbl = lv_label_create(box);
    lv_obj_set_width(Network_State().ui.pwd_lbl, 1);
    // 超长密码显示末尾，保留静态光标，不触发滚动动画。
    lv_label_set_long_mode(Network_State().ui.pwd_lbl, LV_LABEL_LONG_CLIP);
    if (ui != nullptr) {
        lv_obj_set_style_text_font(Network_State().ui.pwd_lbl, ui, 0);
    }
    lv_obj_set_style_text_color(Network_State().ui.pwd_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(Network_State().ui.pwd_lbl, LV_OBJ_FLAG_CLICKABLE);
    static const lv_point_precise_t cursor_points[] = {
        {0, 0}, {UiSx(8), 0}, {UiSx(4), 0},
        {UiSx(4), UiSy(38)}, {0, UiSy(38)}, {UiSx(8), UiSy(38)}};
    lv_obj_t* cursor = lv_line_create(box);
    lv_line_set_points(cursor, cursor_points, 6);
    lv_obj_set_style_line_width(cursor, UiSx(2), 0);
    lv_obj_set_style_line_color(cursor, lv_color_black(), 0);
    lv_obj_clear_flag(cursor, LV_OBJ_FLAG_CLICKABLE);
    Network_State().password[0] = '\0';
    Network_RefreshPasswordDisplay();

    lv_obj_t* row = lv_obj_create(top);
    Network_Strip(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, kPwdBtnRowH);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    Network_MakeBtn(row, Lang::Strings::NETWORK_CANCEL, kPwdBtnW, kPwdBtnH, Network_OnPwdCancel);

    lv_obj_t* spacer = lv_obj_create(page);
    Network_Strip(spacer);
    lv_obj_set_size(spacer, 1, 0);
    lv_obj_set_flex_grow(spacer, 1);

    // 固定在底部直接显示，避免墨水屏滑动动画连续刷新。
    lv_obj_t* kb = lv_obj_create(page);
    Network_Strip(kb);
    Network_State().ui.pwd_keyboard = kb;
    lv_obj_set_width(kb, LV_PCT(100));
    lv_obj_set_height(kb, UiSx(260));
    lv_obj_set_style_bg_color(kb, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(kb, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(kb, 1, 0);
    lv_obj_set_style_border_color(kb, lv_color_black(), 0);
    lv_obj_set_style_pad_top(kb, UiSy(8), 0);
    lv_obj_add_event_cb(kb, Network_OnKbAreaSizeChanged, LV_EVENT_SIZE_CHANGED, nullptr);

    Network_State().kb_mode = KbMode::kEn;
    Network_State().kb_upper = false;
    if (!Network_State().transfer_mode) Network_RebuildCustomKeyboard();
}

void Network_OpenPasswordPage(const std::string& ssid, wifi_auth_mode_t authmode) {
    if (Network_State().ui.pwd_page == nullptr || Network_State().ui.pwd_ssid_lbl == nullptr) {
        return;
    }
    Network_State().pending_ssid = ssid;
    Network_State().pending_authmode = authmode;
    Network_State().kb_mode = KbMode::kEn;
    Network_State().kb_upper = false;

    // 先清除旧名称，固定标题行仅按实际可用宽度显示新 SSID。
    lv_label_set_text(Network_State().ui.pwd_ssid_lbl, "");

    Network_ClearPassword();

    Network_RebuildCustomKeyboard();

    if (Network_State().ui.list_panel != nullptr) {
        lv_obj_add_flag(Network_State().ui.list_panel, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(Network_State().ui.pwd_page, LV_OBJ_FLAG_HIDDEN);
    // 先出布局再刷省略，确保按实际分配宽度裁切
    lv_obj_update_layout(Network_State().ui.pwd_page);
    Book_SetFilenameMiddle(Network_State().ui.pwd_ssid_lbl, ssid.c_str());
    Network_LayoutCustomKeyboard();
}

