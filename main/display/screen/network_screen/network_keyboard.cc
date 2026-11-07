#include "network_screen/network_screen.h"
#include "network_screen_priv.h"
#include "network_keyboard.h"
#include "screen_keyboard.h"
#include "book_screen/book_text_util.h"

#include "assets/lang_config.h"
#include "screen_common.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>

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

void Network_OnPwdConnect(lv_event_t* /*e*/) {
    Network_ScheduleConnect(Network_State().pending_ssid, Network_State().password);
}

void Network_OnPwdCancel(lv_event_t* /*e*/) { Network_ClosePasswordPage(); }

void Network_ClosePasswordPage() {
    ScreenKeyboardPause(Network_State().ui.pwd_keyboard);
    if (Network_State().ui.pwd_page != nullptr) {
        lv_obj_add_flag(Network_State().ui.pwd_page, LV_OBJ_FLAG_HIDDEN);
    }
    if (Network_State().ui.list_panel != nullptr) {
        lv_obj_clear_flag(Network_State().ui.list_panel, LV_OBJ_FLAG_HIDDEN);
    }
    Network_ClearPassword();
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
    Network_State().ui.pwd_keyboard = ScreenKeyboardCreate(page,
        [](ScreenKeyboardEvent event, const char* text, void*) {
            switch (event) {
                case ScreenKeyboardEvent::kInsert: Network_AppendToPassword(text); break;
                case ScreenKeyboardEvent::kBackspace: Network_BackspacePassword(); break;
                case ScreenKeyboardEvent::kClear: Network_ClearPassword(); break;
                case ScreenKeyboardEvent::kConfirm: Network_OnPwdConnect(nullptr); break;
                case ScreenKeyboardEvent::kLayout: Network_RefreshPasswordDisplay(); break;
            }
        });
    if (!Network_State().transfer_mode) ScreenKeyboardReset(Network_State().ui.pwd_keyboard);
}

void Network_OpenPasswordPage(const std::string& ssid, wifi_auth_mode_t authmode) {
    if (Network_State().ui.pwd_page == nullptr || Network_State().ui.pwd_ssid_lbl == nullptr) {
        return;
    }
    Network_State().pending_ssid = ssid;
    Network_State().pending_authmode = authmode;

    // 先清除旧名称，固定标题行仅按实际可用宽度显示新 SSID。
    lv_label_set_text(Network_State().ui.pwd_ssid_lbl, "");

    Network_ClearPassword();

    ScreenKeyboardReset(Network_State().ui.pwd_keyboard);

    if (Network_State().ui.list_panel != nullptr) {
        lv_obj_add_flag(Network_State().ui.list_panel, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(Network_State().ui.pwd_page, LV_OBJ_FLAG_HIDDEN);
    // 先出布局再刷省略，确保按实际分配宽度裁切
    lv_obj_update_layout(Network_State().ui.pwd_page);
    Book_SetFilenameMiddle(Network_State().ui.pwd_ssid_lbl, ssid.c_str());
    Network_RefreshPasswordDisplay();
}

