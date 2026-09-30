// network_keyboard.cc — split from parent .cc
#include "network_screen/network_screen.h"
#include "network_screen_priv.h"
#include "network_keyboard.h"
#include "network_screen/network_screen_priv.h"

#include "application.h"
#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "power_policy.h"
#include "screen_common.h"
#include "ssid_manager.h"
#include "vk_key_handler.h"
#include "wifi_station.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

void Network_RefreshPasswordDisplay() {
    if (Network_State().ui.pwd_lbl == nullptr) {
        return;
    }
    if (Network_State().password[0] == '\0') {
        lv_label_set_text(Network_State().ui.pwd_lbl, Lang::Strings::NETWORK_PWD_PLACEHOLDER);
        lv_obj_set_style_text_opa(Network_State().ui.pwd_lbl, LV_OPA_50, 0);
    } else {
        lv_label_set_text(Network_State().ui.pwd_lbl, Network_State().password);
        lv_obj_set_style_text_opa(Network_State().ui.pwd_lbl, LV_OPA_COVER, 0);
    }
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

void Network_FillKbLayout(KbKeyDesc out[kKbRows * kKbCols]) {
    for (int i = 0; i < kKbRows * kKbCols; ++i) {
        out[i] = {"", nullptr, KbAction::kEmpty};
    }

    auto put = [&](int idx, const char* label, const char* insert, KbAction act) {
        if (idx < 0 || idx >= kKbRows * kKbCols) {
            return;
        }
        out[idx] = {label, insert, act};
    };

    constexpr int kCharSlots = 6 * kKbCols;  // 前 6 行

    if (Network_State().kb_mode == KbMode::kEn) {
        // 数字 + 字母 a–z（共 36 格）
        static char bufs[kCharSlots][2];
        int n = 0;
        for (char c = '1'; c <= '9' && n < kCharSlots; ++c, ++n) {
            bufs[n][0] = c;
            bufs[n][1] = '\0';
            put(n, bufs[n], bufs[n], KbAction::kChar);
        }
        if (n < kCharSlots) {
            bufs[n][0] = '0';
            bufs[n][1] = '\0';
            put(n, bufs[n], bufs[n], KbAction::kChar);
            ++n;
        }
        for (char c = 'a'; c <= 'z' && n < kCharSlots; ++c, ++n) {
            bufs[n][0] = Network_State().kb_upper ? static_cast<char>(c - 'a' + 'A') : c;
            bufs[n][1] = '\0';
            put(n, bufs[n], bufs[n], KbAction::kChar);
        }
    } else {
        // 符：ASCII + 中文标点合并
        static const char* const sym[] = {
            "!", "@", "#", "$", "%", "^",
            "&", "*", "(", ")", "-", "_",
            "=", "+", "[", "]", "{", "}",
            "\\", "|", ";", ":", "'", "\"",
            ",", ".", "<", ">", "/", "?",
            "`", "~", "，", "。", "？", "！",
            "、", "；", "：", "…", "“", "”",
            "‘", "’", "（", "）", "【", "】",
            "《", "》", "—", "·", "￥", "～",
        };
        const int nsym = static_cast<int>(sizeof(sym) / sizeof(sym[0]));
        for (int i = 0; i < nsym && i < kCharSlots; ++i) {
            put(i, sym[i], sym[i], KbAction::kChar);
        }
    }

    // 底行：英 符 大小写 空格 空格 删
    const int base = 6 * kKbCols;
    put(base + 0, "英", nullptr, KbAction::kModeEn);
    put(base + 1, "符", nullptr, KbAction::kModeSym);
    put(base + 2, Network_State().kb_upper ? "大" : "小", nullptr, KbAction::kShift);
    put(base + 3, "␣", " ", KbAction::kSpace);
    put(base + 4, "␣", " ", KbAction::kSpace);
    put(base + 5, "删", nullptr, KbAction::kBackspace);
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
        case KbAction::kEmpty:
        default:
            break;
    }
}

// 切布局：松手再建盘，避免 PRESSED 里 lv_obj_clean 删掉当前按键。
void Network_OnCustomKeyClicked(lv_event_t* e) {
    const auto action = static_cast<KbAction>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));

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
        default:
            break;
    }
}

void Network_StyleKbKey(lv_obj_t* btn, lv_obj_t* lbl, KbAction action, bool active_mode) {
    const bool highlight = active_mode || action == KbAction::kShift;
    if (highlight && (action == KbAction::kModeEn || action == KbAction::kModeSym ||
                      (action == KbAction::kShift && Network_State().kb_upper))) {
        lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    }
}

void Network_LayoutCustomKeyboardSquares() {
    if (Network_State().ui.pwd_keyboard == nullptr) {
        return;
    }
    const lv_coord_t w = lv_obj_get_content_width(Network_State().ui.pwd_keyboard);
    const lv_coord_t h = lv_obj_get_content_height(Network_State().ui.pwd_keyboard);
    if (w <= 0 || h <= 0) {
        return;
    }
    const lv_coord_t cell_w = (w - kKbGap * (kKbCols - 1)) / kKbCols;
    const lv_coord_t cell_h = (h - kKbGap * (kKbRows - 1)) / kKbRows;
    const lv_coord_t side = (cell_w < cell_h) ? cell_w : cell_h;
    if (side <= 0) {
        return;
    }
    const lv_coord_t total_w = side * kKbCols + kKbGap * (kKbCols - 1);
    const lv_coord_t total_h = side * kKbRows + kKbGap * (kKbRows - 1);
    const lv_coord_t ox = (w - total_w) / 2;
    const lv_coord_t oy = (h - total_h) / 2;

    for (int r = 0; r < kKbRows; ++r) {
        for (int c = 0; c < kKbCols; ++c) {
            lv_obj_t* key = Network_State().kb_keys[r * kKbCols + c];
            if (key == nullptr) {
                continue;
            }
            lv_obj_set_size(key, side, side);
            lv_obj_set_pos(key, ox + c * (side + kKbGap), oy + r * (side + kKbGap));
        }
    }
}

void Network_OnKbAreaSizeChanged(lv_event_t* /*e*/) {
    Network_LayoutCustomKeyboardSquares();
}

void Network_RebuildCustomKeyboard() {
    if (Network_State().ui.pwd_keyboard == nullptr) {
        return;
    }
    lv_obj_clean(Network_State().ui.pwd_keyboard);
    for (int i = 0; i < kKbRows * kKbCols; ++i) {
        Network_State().kb_keys[i] = nullptr;
    }

    KbKeyDesc layout[kKbRows * kKbCols];
    Network_FillKbLayout(layout);

    const lv_font_t* font = Network_KbFont();
    for (int i = 0; i < kKbRows * kKbCols; ++i) {
        const KbKeyDesc& d = layout[i];
        lv_obj_t* btn = lv_obj_create(Network_State().ui.pwd_keyboard);
        Network_Strip(btn);
        Network_State().kb_keys[i] = btn;
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(btn);
        void* ud = reinterpret_cast<void*>(static_cast<uintptr_t>(d.action));
        // 输入键按下即上屏；切 英/符/大小写 仍 CLICKED，防止重建时销毁按中的键
        if (d.action == KbAction::kChar || d.action == KbAction::kSpace ||
            d.action == KbAction::kBackspace) {
            lv_obj_add_event_cb(btn, Network_OnCustomKeyPressed, LV_EVENT_PRESSED, ud);
        } else if (d.action == KbAction::kShift || d.action == KbAction::kModeEn ||
                   d.action == KbAction::kModeSym) {
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

        const bool mode_on =
            (d.action == KbAction::kModeEn && Network_State().kb_mode == KbMode::kEn) ||
            (d.action == KbAction::kModeSym && Network_State().kb_mode == KbMode::kSym);
        Network_StyleKbKey(btn, lbl, d.action, mode_on);
    }
    Network_LayoutCustomKeyboardSquares();
}

void Network_OnPwdConnect(lv_event_t* /*e*/) {
    Network_ScheduleConnect(Network_State().pending_ssid, Network_State().password);
}

void Network_OnPwdCancel(lv_event_t* /*e*/) { Network_ClosePasswordPage(); }

void Network_ClosePasswordPage() {
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

    Network_State().ui.pwd_ssid_lbl = lv_label_create(top);
    lv_label_set_text(Network_State().ui.pwd_ssid_lbl, "");
    lv_obj_set_width(Network_State().ui.pwd_ssid_lbl, LV_PCT(100));
    lv_label_set_long_mode(Network_State().ui.pwd_ssid_lbl, LV_LABEL_LONG_DOT);
    if (ui != nullptr) {
        lv_obj_set_style_text_font(Network_State().ui.pwd_ssid_lbl, ui, 0);
    }

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
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    Network_State().ui.pwd_lbl = lv_label_create(box);
    lv_obj_set_width(Network_State().ui.pwd_lbl, LV_PCT(100));
    // 墨水屏勿用滚动动画（会连刷）；超长直接裁切
    lv_label_set_long_mode(Network_State().ui.pwd_lbl, LV_LABEL_LONG_CLIP);
    if (ui != nullptr) {
        lv_obj_set_style_text_font(Network_State().ui.pwd_lbl, ui, 0);
    }
    lv_obj_set_style_text_color(Network_State().ui.pwd_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(Network_State().ui.pwd_lbl, LV_OBJ_FLAG_CLICKABLE);
    Network_State().password[0] = '\0';
    Network_RefreshPasswordDisplay();

    lv_obj_t* row = lv_obj_create(top);
    Network_Strip(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, kPwdBtnRowH);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    Network_MakeBtn(row, Lang::Strings::NETWORK_CANCEL, kPwdBtnW, kPwdBtnH, Network_OnPwdCancel);
    Network_MakeBtn(row, Lang::Strings::NETWORK_CONNECT, kPwdBtnW, kPwdBtnH, Network_OnPwdConnect);

    // 自定义键盘区域：6×6 均分大方键
    lv_obj_t* kb = lv_obj_create(page);
    Network_Strip(kb);
    Network_State().ui.pwd_keyboard = kb;
    lv_obj_set_width(kb, LV_PCT(100));
    lv_obj_set_flex_grow(kb, 1);
    lv_obj_set_style_bg_color(kb, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(kb, Network_OnKbAreaSizeChanged, LV_EVENT_SIZE_CHANGED, nullptr);

    Network_State().kb_mode = KbMode::kEn;
    Network_State().kb_upper = false;
    Network_RebuildCustomKeyboard();
}

void Network_OpenPasswordPage(const std::string& ssid, wifi_auth_mode_t authmode) {
    if (Network_State().ui.pwd_page == nullptr || Network_State().ui.pwd_ssid_lbl == nullptr) {
        return;
    }
    Network_State().pending_ssid = ssid;
    Network_State().pending_authmode = authmode;
    Network_State().kb_mode = KbMode::kEn;
    Network_State().kb_upper = false;

    char ttext[96];
    snprintf(ttext, sizeof(ttext), Lang::Strings::NETWORK_CONNECT_TO_FMT, ssid.c_str());
    lv_label_set_text(Network_State().ui.pwd_ssid_lbl, ttext);

    Network_ClearPassword();

    Network_RebuildCustomKeyboard();

    if (Network_State().ui.list_panel != nullptr) {
        lv_obj_add_flag(Network_State().ui.list_panel, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(Network_State().ui.pwd_page, LV_OBJ_FLAG_HIDDEN);
    // 显示后再按实际尺寸均分方键
    Network_LayoutCustomKeyboardSquares();
}

