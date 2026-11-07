#include "screen_keyboard.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "epd_refresh.h"
#include "ui_scale.h"
#include <cstdint>
#include <new>

namespace {
enum class KbMode : uint8_t { kEn = 0, kSym = 1, kMore = 2 }; // 英文、数字符号、扩展符号
constexpr int kKbCols = 10; // 每行键位上限
constexpr int kKbRows = 4; // 固定四行
constexpr int kKbKeysCap = kKbCols * kKbRows; // 键位容量
constexpr lv_coord_t kKbGap = UiSx(7); // 按键间距

enum class KbAction : uint8_t {
    kChar = 0, // 输入字符
    kBackspace, // 删除末尾字符
    kShift, // 切换大小写
    kModeEn, // 切换英文
    kModeSym, // 切换数字与常用符号
    kModeMore, // 切换扩展符号
    kConfirm, // 确认当前输入
    kSpace, // 输入空格
    kEmpty, // 不创建按键
};

struct KbKeyDesc {
    const char* label = nullptr;  // 显示
    KbAction action = KbAction::kChar; // 按键动作
};

struct KeyFeedback {
    uint32_t tick = 0; // 按下时间。
    uint32_t sequence = 0; // 按下时的落墨序号。
};
void Strip(lv_obj_t* obj) {
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}
struct Keyboard {
    lv_obj_t* root = nullptr; // 键盘容器。
    lv_obj_t* keys[kKbKeysCap] = {}; // 按键对象。
    KbMode mode = KbMode::kEn; // 当前字符布局。
    bool upper = false; // 大写状态。
    lv_timer_t* timer = nullptr; // 按下反馈计时器。
    KeyFeedback key_feedback[kKbKeysCap]; // 各键落墨状态。
    KbAction pending_action = KbAction::kEmpty; // 待反馈完成的操作。
    ScreenKeyboardCallback callback = nullptr; // 输入事件回调。
    void* context = nullptr; // 调用方上下文。
    void Stop();
    void Tick();
    void Feedback(lv_event_t* e);
    static const lv_image_dsc_t* PressedImage();
    void Fill(KbKeyDesc out[kKbKeysCap]);
    void Pressed(lv_event_t* e);
    void Clicked(lv_event_t* e);
    void Apply(KbAction action);
    static void AddIcon(lv_obj_t* btn, KbAction action);
    void Layout();
    void Rebuild();
};
void Keyboard::Stop() {
    if (timer != nullptr) {
        lv_timer_delete(timer);
        timer = nullptr;
    }
    pending_action = KbAction::kEmpty;
}

void Keyboard::Tick() {
    if (root == nullptr ||
        !lv_obj_is_visible(root)) {
        Stop();
        return;
    }
    const auto stats = EpdGetStats();
    bool pending = false;
    for (int i = 0; i < kKbKeysCap; ++i) {
        lv_obj_t* key = keys[i];
        if (key == nullptr || !lv_obj_has_state(key, LV_STATE_USER_1)) continue;
        const auto& feedback = key_feedback[i];
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
        const KbAction action = pending_action;
        Stop();
        Apply(action);
    }
}

void Keyboard::Feedback(lv_event_t* e) {
    auto* key = static_cast<lv_obj_t*>(lv_event_get_target(e));
    for (int i = 0; i < kKbKeysCap; ++i) {
        if (keys[i] == key) {
            key_feedback[i] = {lv_tick_get(), EpdGetStats().submitted};
            break;
        }
    }
    lv_obj_add_state(key, LV_STATE_USER_1);
    lv_obj_invalidate(key);
    if (timer == nullptr) {
        timer = lv_timer_create([](lv_timer_t* t) { static_cast<Keyboard*>(lv_timer_get_user_data(t))->Tick(); }, 30, this);
    }
}

const lv_image_dsc_t* Keyboard::PressedImage() {
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

void Keyboard::Fill(KbKeyDesc out[kKbKeysCap]) {
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
    if (mode == KbMode::kEn) {
        chars(0, upper ? "QWERTYUIOP" : "qwertyuiop");
        chars(1, upper ? "ASDFGHJKL" : "asdfghjkl");
        chars(2, upper ? " ZXCVBNM" : " zxcvbnm");
        put(20, "", KbAction::kShift);
        put(28, "", KbAction::kBackspace);
        put(30, "?123", KbAction::kModeSym);
        put(31, "Space", KbAction::kSpace);
        put(32, "", KbAction::kConfirm);
    } else {
        const bool more = mode == KbMode::kMore;
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

void Keyboard::Pressed(lv_event_t* e) {
    const auto action = static_cast<KbAction>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(e))));
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_obj_t* lbl = (btn != nullptr) ? lv_obj_get_child(btn, 0) : nullptr;
    const char* label = (lbl != nullptr) ? lv_label_get_text(lbl) : nullptr;

    switch (action) {
        case KbAction::kBackspace:
            callback(ScreenKeyboardEvent::kBackspace, "", context);
            break;
        case KbAction::kSpace:
            callback(ScreenKeyboardEvent::kInsert, " ", context);
            break;
        case KbAction::kChar:
            if (label != nullptr) {
                callback(ScreenKeyboardEvent::kInsert, label, context);
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

void Keyboard::Clicked(lv_event_t* e) {
    const auto action = static_cast<KbAction>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(e))));
    if (timer != nullptr) {
        pending_action = action;
        return;
    }
    Apply(action);
}

void Keyboard::Apply(KbAction action) {
    switch (action) {
        case KbAction::kShift:
            upper = !upper;
            Rebuild();
            break;
        case KbAction::kModeEn:
            mode = KbMode::kEn;
            Rebuild();
            break;
        case KbAction::kModeSym:
            mode = KbMode::kSym;
            Rebuild();
            break;
        case KbAction::kModeMore:
            mode = KbMode::kMore;
            Rebuild();
            break;
        case KbAction::kConfirm:
            callback(ScreenKeyboardEvent::kConfirm, "", context);
            break;
        default:
            break;
    }
}

void Keyboard::AddIcon(lv_obj_t* btn, KbAction action) {
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

void Keyboard::Layout() {
    if (root == nullptr) {
        return;
    }
    const int w = lv_obj_get_content_width(root);
    const int h = lv_obj_get_content_height(root);
    const int key_h = (h - kKbGap * (kKbRows - 1)) / kKbRows;
    if (w <= 0 || key_h <= 0) {
        return;
    }
    for (int r = 0; r < kKbRows; ++r) {
        int count = 0;
        int weights[kKbCols] = {};
        int total = 0;
        for (int c = 0; c < kKbCols; ++c) {
            if (keys[r * kKbCols + c] == nullptr) {
                break;
            }
            int weight = 100;
            if (r == 2 && mode == KbMode::kEn && (c == 0 || c == 8)) {
                weight = 148;
            } else if (r == 2 && c == 7 && mode != KbMode::kEn) {
                weight = 148;
            } else if (r == 3) {
                const int space = mode == KbMode::kEn ? 1 : 2;
                weight = c < space ? 170 : c == space ? 480 : 200;
            }
            weights[count++] = weight;
            total += weight;
        }
        if (count == 0) {
            continue;
        }
        const bool inset = r == 1 || (r == 2 && mode != KbMode::kEn);
        const int row_w = inset ? w * 905 / 1000 : w;
        const int usable = row_w - kKbGap * (count - 1);
        if (usable <= 0) {
            continue;
        }
        int sum = 0;
        for (int c = 0; c < count; ++c) {
            const int left = usable * sum / total;
            sum += weights[c];
            lv_obj_t* key = keys[r * kKbCols + c];
            lv_obj_set_size(key, usable * sum / total - left, key_h);
            lv_obj_set_pos(key, (w - row_w) / 2 + left + c * kKbGap, r * (key_h + kKbGap));
        }
    }
    callback(ScreenKeyboardEvent::kLayout, "", context);
}

void Keyboard::Rebuild() {
    if (root == nullptr) {
        return;
    }
    Stop();
    lv_obj_clean(root);
    for (int i = 0; i < kKbKeysCap; ++i) {
        keys[i] = nullptr;
    }

    const int nkeys = kKbKeysCap;

    KbKeyDesc layout[kKbKeysCap];
    Fill(layout);

    const lv_font_t* font = fontpack_lv_font_get(30, 2);
    if (font == nullptr) font = fontpack_lv_font_ui();
    for (int i = 0; i < nkeys; ++i) {
        const KbKeyDesc& d = layout[i];
        if (d.action == KbAction::kEmpty) {
            continue;
        }
        lv_obj_t* btn = lv_obj_create(root);
        Strip(btn);
        keys[i] = btn;
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, UiSx(7), 0);
        lv_obj_set_style_bg_image_src(btn, PressedImage(), LV_STATE_USER_1);
        lv_obj_set_style_bg_image_tiled(btn, true, LV_STATE_USER_1);
        lv_obj_set_style_bg_image_opa(btn, LV_OPA_COVER, LV_STATE_USER_1);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) { static_cast<Keyboard*>(lv_event_get_user_data(e))->Feedback(e); }, LV_EVENT_PRESSED, this);
        HapticAttachClick(btn);
        lv_obj_set_user_data(btn, reinterpret_cast<void*>(static_cast<uintptr_t>(d.action)));
        // 输入键按下即上屏；切 英/符/大小写 仍 CLICKED，防止重建时销毁按中的键
        if (d.action == KbAction::kChar || d.action == KbAction::kSpace ||
            d.action == KbAction::kBackspace) {
            lv_obj_add_event_cb(btn, [](lv_event_t* e) { static_cast<Keyboard*>(lv_event_get_user_data(e))->Pressed(e); }, LV_EVENT_PRESSED, this);
        } else if (d.action == KbAction::kShift || d.action == KbAction::kModeEn ||
                   d.action == KbAction::kModeSym || d.action == KbAction::kModeMore ||
                   d.action == KbAction::kConfirm) {
            lv_obj_add_event_cb(btn, [](lv_event_t* e) { static_cast<Keyboard*>(lv_event_get_user_data(e))->Clicked(e); }, LV_EVENT_CLICKED, this);
        }
        if (d.action == KbAction::kBackspace) {
            // 与电话拨号页一致：短按删一字，长按清空
            lv_obj_add_event_cb(btn, [](lv_event_t* e) {
                auto* kb = static_cast<Keyboard*>(lv_event_get_user_data(e));
                kb->callback(ScreenKeyboardEvent::kClear, "", kb->context);
            }, LV_EVENT_LONG_PRESSED, this);
        }

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, d.label != nullptr ? d.label : "");
        if (font != nullptr) {
            lv_obj_set_style_text_font(lbl, font, 0);
        }
        lv_obj_center(lbl);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        AddIcon(btn, d.action);
        if (d.action == KbAction::kShift && upper) {
            lv_obj_set_style_border_width(btn, UiSx(3), 0);
        }
    }
    Layout();
}
} // namespace

lv_obj_t* ScreenKeyboardCreate(lv_obj_t* parent, ScreenKeyboardCallback callback, void* context) {
    auto* kb = new (std::nothrow) Keyboard;
    if (kb == nullptr) return nullptr;
    kb->root = lv_obj_create(parent);
    kb->callback = callback;
    kb->context = context;
    Strip(kb->root);
    lv_obj_set_user_data(kb->root, kb);
    lv_obj_set_width(kb->root, lv_pct(100));
    lv_obj_set_height(kb->root, UiSx(260));
    lv_obj_set_style_bg_color(kb->root, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(kb->root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(kb->root, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(kb->root, 1, 0);
    lv_obj_set_style_border_color(kb->root, lv_color_black(), 0);
    lv_obj_set_style_pad_top(kb->root, UiSy(8), 0);
    lv_obj_add_event_cb(kb->root, [](lv_event_t* e) {
        auto* kb = static_cast<Keyboard*>(lv_event_get_user_data(e));
        if (lv_event_get_target_obj(e) != kb->root) return;
        if (lv_event_get_code(e) == LV_EVENT_DELETE) {
            kb->Stop();
            delete kb;
        } else if (lv_event_get_code(e) == LV_EVENT_SIZE_CHANGED) kb->Layout();
    }, LV_EVENT_ALL, kb);
    return kb->root;
}

void ScreenKeyboardReset(lv_obj_t* root) {
    if (root == nullptr) return;
    auto* kb = static_cast<Keyboard*>(lv_obj_get_user_data(root));
    kb->mode = KbMode::kEn;
    kb->upper = false;
    kb->Rebuild();
}

void ScreenKeyboardPause(lv_obj_t* root) {
    if (root == nullptr) return;
    auto* kb = static_cast<Keyboard*>(lv_obj_get_user_data(root));
    kb->Stop();
    for (auto* key : kb->keys) if (key != nullptr) lv_obj_remove_state(key, LV_STATE_USER_1);
}
