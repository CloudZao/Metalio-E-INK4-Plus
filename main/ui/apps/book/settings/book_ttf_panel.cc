#include "ui/common/widgets/storage_error_text.h"
#include "assets/lang_config.h"
#include "assets/lang_reader.h"


#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <esp_timer.h>
#include <lvgl.h>
#include <strings.h>
#include "ui/apps/book/settings/book_ttf_panel.h"
#include "boards/common/storage/sd_access.h"
#include "boards/common/storage/sd_directory_page.h"
#include "infra/storage/sd_paths.h"
#include "ui/common/drawing/font/ttf_convert.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/apps/book/book_font_runtime.h"
#include "ui/apps/book/book_screen_priv.h"
#include "ui/apps/book/book_text_util.h"
#include "ui/apps/book/reader/book_reader_overlay.h"
#include "ui/apps/book/settings/book_settings_sheet.h"
#include "ui/apps/book/settings/book_ttf_actions.h"
#include "ui/apps/book/shelf/book_bookshelf.h"
#include "ui/common/widgets/screen_text.h"
#include "ui/common/drawing/ui_theme.h"

static lv_obj_t* MakeTtfFileInfo(lv_obj_t* parent) {
    auto make_row = [parent](const char* caption, const char* value) {
        auto* row = lv_obj_create(parent);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, UiSx(10), 0);
        Book_DisableScroll(row);
        auto* title = lv_label_create(row);
        lv_obj_set_style_text_font(title, Book_ItemFont(), 0);
        lv_obj_set_width(title, std::max<int>(UiSx(64), ScreenMeasureTextWidth(Book_ItemFont(), caption)));
        lv_label_set_text(title, caption);
        auto* name = lv_label_create(row);
        lv_obj_set_style_text_font(name, Book_ItemFont(), 0);
        lv_obj_set_width(name, 0);
        lv_obj_set_flex_grow(name, 1);
        Book_SetFilenameMiddle(name, value);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        return name;
    };
    make_row(LangString_TTF_SOURCE, Book_State().ttf.ttf_pick.c_str());
    const uint16_t size = static_cast<uint16_t>(Book_State().ttf.ttf_size_px);
    char output[SD_FILE_NAME_CAPACITY + 10];
    TtfConvertFormatOutputFileList(Book_State().ttf.ttf_pick.c_str(), &size, 1, output, sizeof(output));
    return make_row(LangString_TTF_OUTPUT, output);
}

static lv_obj_t* MakeTtfTip(lv_obj_t* parent, const char* text) {
    auto* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, UiColorForeground(), 0);
    lv_obj_set_style_radius(box, UiSx(4), 0);
    lv_obj_set_style_pad_all(box, UiSx(8), 0);
    lv_obj_set_style_pad_column(box, UiSx(10), 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    Book_DisableScroll(box);
    const int diameter = std::max<int>(18, UiSx(18));
    auto* icon = lv_obj_create(box);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, diameter, diameter);
    lv_obj_set_style_border_width(icon, 2, 0);
    lv_obj_set_style_border_color(icon, UiColorForeground(), 0);
    lv_obj_set_style_radius(icon, LV_RADIUS_CIRCLE, 0);
    Book_DisableScroll(icon);
    auto* stem = lv_obj_create(icon);
    lv_obj_remove_style_all(stem);
    lv_obj_set_size(stem, 2, diameter / 3);
    lv_obj_set_style_bg_color(stem, UiColorForeground(), 0);
    lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, 0);
    lv_obj_align(stem, LV_ALIGN_CENTER, 0, -2);
    auto* dot = lv_obj_create(icon);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 2, 2);
    lv_obj_set_style_bg_color(dot, UiColorForeground(), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_CENTER, 0, (diameter / 3 + 2) / 2);
    auto* label = lv_label_create(box);
    lv_obj_set_width(label, 0);
    lv_obj_set_flex_grow(label, 1);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, Book_ItemFont(), 0);
    lv_label_set_text(label, text);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

static SdDirectoryPage s_ttf_directory;
static lv_timer_t* s_ttf_directory_timer = nullptr;

static bool TtfDirectoryFilter(const char* name, bool directory) {
    const char* relative = name + std::strlen(SD_PATH_FONT_SRC) + 1;
    return !directory && SdIsValidFileName(relative) && IsTtfFileName(relative);
}

static void TtfDirectoryTick(lv_timer_t*) {
    auto& st = Book_State();
    if (st.ttf.ttf_mode == BookUiState::TtfMode::kNone || !st.ttf.ttf_sheet || SdAccessIsQuiescing()) {
        s_ttf_directory.Cancel();
        lv_timer_del(s_ttf_directory_timer);
        s_ttf_directory_timer = nullptr;
        return;
    }
    if (st.ttf.ttf_mode != BookUiState::TtfMode::kList) {
        return;
    }
    if (st.ttf.ttf_page_label && lv_obj_is_valid(st.ttf.ttf_page_label)) {
        if (s_ttf_directory.Counted() && !st.ttf.ttf_loading) {
            lv_label_set_text_fmt(st.ttf.ttf_page_label, "%d/%d", st.ttf.ttf_page + 1, TtfPageCount());
            lv_timer_del(s_ttf_directory_timer);
            s_ttf_directory_timer = nullptr;
            return;
        } else {
            char text[32];
            std::snprintf(text, sizeof(text), "%d/…", st.ttf.ttf_page + 1);
            if (std::strcmp(lv_label_get_text(st.ttf.ttf_page_label), text)) {
                lv_label_set_text(st.ttf.ttf_page_label, text);
            }
        }
    }
    SdDirectoryPage::Result result;
    if (s_ttf_directory.Poll(result)) {
        if (result.ok && result.entries.empty() && result.page > 0) {
            --st.ttf.ttf_page;
            TtfScanFiles();
            return;
        }
        st.ttf.ttf_files.clear();
        for (const auto& item : result.entries) {
            const std::string relative = item.path.substr(std::strlen(SD_PATH_FONT_SRC) + 1);
            if (!SdIsValidFileName(relative.c_str())) {
                continue;
            }
            st.ttf.ttf_files.push_back(relative);
        }
        st.ttf.ttf_loading = false;
        st.ttf.ttf_failed = !result.ok;
        st.ttf.directory_error = result.error;
        TtfRebuildContent();
        if (!result.ok) {
            lv_timer_del(s_ttf_directory_timer);
            s_ttf_directory_timer = nullptr;
        }
    }
}

bool IsTtfFileName(const char* name) {
    const size_t n = std::strlen(name);
    if (n < 5 || !SdIsValidFileName(name)) {
        return false;
    }
    return strcasecmp(name + n - 4, ".ttf") == 0 || strcasecmp(name + n - 4, ".otf") == 0;
}

int TtfPageCount() {
    return s_ttf_directory.Pages();
}

void TtfScanFiles() {
    auto& st = Book_State();
    st.ttf.ttf_files.clear();
    st.ttf.ttf_loading = true;
    st.ttf.ttf_failed = false;
    s_ttf_directory.Request({SD_PATH_FONT_SRC}, TtfDirectoryFilter, st.ttf.ttf_page, kFontListPageSize);
    if (!s_ttf_directory_timer) {
        s_ttf_directory_timer = lv_timer_create(TtfDirectoryTick, 100, nullptr);
    }
    if (!s_ttf_directory_timer) {
        st.ttf.ttf_loading = false;
        st.ttf.ttf_failed = true;
        ESP_LOGE(TAG, "font import page timer creation failed");
    }
}

void TtfPanelClose() {
    s_ttf_directory.Cancel();
    if (s_ttf_directory_timer) {
        lv_timer_del(s_ttf_directory_timer);
    }
    s_ttf_directory_timer = nullptr;
    StopTtfPollTimer();
    auto& st = Book_State();
    if (st.ttf.ttf_sheet != nullptr && lv_obj_is_valid(st.ttf.ttf_sheet)) {
        lv_obj_del(st.ttf.ttf_sheet);
    }
    st.ttf.ttf_sheet = nullptr;
    st.ttf.ttf_page_label = nullptr;
    st.ttf.ttf_bar = nullptr;
    st.ttf.ttf_pct_lbl = nullptr;
    st.ttf.ttf_msg_lbl = nullptr;
    st.ttf.ttf_size_value = nullptr;
    st.ttf.ttf_size_dec = nullptr;
    st.ttf.ttf_size_inc = nullptr;
    for (int i = 0; i < kFontListPageSize; ++i) {
        st.ttf.ttf_rows[i] = {};
    }
    st.ttf.ttf_mode = BookUiState::TtfMode::kNone;
}

void StopTtfPollTimer() {
    TtfConvertCancel();
    auto& st = Book_State();
    st.ttf.ttf_selected.clear();
    st.ttf.ttf_multi = false;
    if (st.ttf.ttf_poll_timer != nullptr) {
        lv_timer_del(st.ttf.ttf_poll_timer);
        st.ttf.ttf_poll_timer = nullptr;
    }
}

void TtfEnsurePanel() {
    auto& st = Book_State();
    if (st.settings.settings_sheet == nullptr || !lv_obj_is_valid(st.settings.settings_sheet)) {
        return;
    }
    auto* body = st.settings.reader_sheet_body;
    if (body == nullptr || !lv_obj_is_valid(body)) {
        return;
    }
    lv_obj_update_layout(st.settings.settings_sheet);
    const int panel_height = lv_obj_get_y(body) + lv_obj_get_height(body);
    if (st.ttf.ttf_sheet != nullptr && lv_obj_is_valid(st.ttf.ttf_sheet)) {
        lv_obj_set_size(st.ttf.ttf_sheet, lv_pct(100), panel_height);
        lv_obj_align(st.ttf.ttf_sheet, LV_ALIGN_TOP_LEFT, 0, 0);
        TtfRebuildContent();
        return;
    }
    st.ttf.ttf_sheet = lv_obj_create(st.settings.settings_sheet);
    lv_obj_remove_style_all(st.ttf.ttf_sheet);
    lv_obj_add_flag(st.ttf.ttf_sheet, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(st.ttf.ttf_sheet, lv_pct(100), panel_height);
    lv_obj_align(st.ttf.ttf_sheet, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(st.ttf.ttf_sheet, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(st.ttf.ttf_sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(st.ttf.ttf_sheet, 0, 0);
    lv_obj_set_style_pad_hor(st.ttf.ttf_sheet, UiSx(16), 0);
    lv_obj_set_style_pad_ver(st.ttf.ttf_sheet, 0, 0);
    lv_obj_set_style_pad_row(st.ttf.ttf_sheet, 0, 0);
    lv_obj_set_flex_flow(st.ttf.ttf_sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(st.ttf.ttf_sheet, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(st.ttf.ttf_sheet);
    lv_obj_set_scrollbar_mode(st.ttf.ttf_sheet, LV_SCROLLBAR_MODE_OFF);
    lv_obj_move_foreground(st.ttf.ttf_sheet);
    TtfRebuildContent();
}

void TtfRebuildContent() {
    auto& st = Book_State();
    if (st.ttf.ttf_sheet == nullptr || !lv_obj_is_valid(st.ttf.ttf_sheet)) {
        return;
    }
    st.ttf.ttf_page_label = nullptr;
    lv_obj_clean(st.ttf.ttf_sheet);

    for (int i = 0; i < kFontListPageSize; ++i) {
        st.ttf.ttf_rows[i] = {};
    }
    st.ttf.ttf_bar = nullptr;
    st.ttf.ttf_pct_lbl = nullptr;
    st.ttf.ttf_msg_lbl = nullptr;
    st.ttf.ttf_size_value = nullptr;
    st.ttf.ttf_size_dec = nullptr;
    st.ttf.ttf_size_inc = nullptr;

    auto make_button = [](lv_obj_t* parent, const char* text, lv_event_cb_t cb, int width,
                          bool repeat = false) -> lv_obj_t* {
        auto* btn = MakeSheetIconBtn(parent, text, cb, true);
        const int height = std::max<int>(UiSy(30), Book_ItemFont()->line_height + 12);
        lv_obj_set_size(btn, width, height);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_radius(btn, height / 2, 0);
        lv_obj_set_style_text_font(lv_obj_get_child(btn, 0), Book_ItemFont(), 0);
        if (repeat) {
            lv_obj_remove_event_cb(btn, cb);
            lv_obj_add_event_cb(btn, cb, LV_EVENT_PRESSED, nullptr);
            lv_obj_add_event_cb(btn, cb, LV_EVENT_LONG_PRESSED_REPEAT, nullptr);
        }
        return btn;
    };

    lv_obj_t* head = lv_obj_create(st.ttf.ttf_sheet);
    lv_obj_remove_style_all(head);
    lv_obj_set_width(head, lv_pct(100));
    lv_obj_set_height(head, lv_obj_get_y(st.settings.reader_sheet_body) +
                                lv_obj_get_style_pad_top(st.settings.reader_sheet_body, LV_PART_MAIN));
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 16, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(head);
    const char* title = "";
    switch (st.ttf.ttf_mode) {
        case BookUiState::TtfMode::kList:
            title = LangString_BOOK_IMPORT_TTF;
            break;
        case BookUiState::TtfMode::kConfirm:
            title = LangString_BOOK_IMPORT_TTF;
            break;
        case BookUiState::TtfMode::kProgress:
            title = LangString_TTF_CONVERTING;
            break;
        case BookUiState::TtfMode::kResult:
            title = LangString_TTF_FAILED;
            break;
        default:
            break;
    }
    {
        auto* back = lv_obj_create(head);
        lv_obj_remove_style_all(back);
        lv_obj_set_size(back, 48, 48);
        lv_obj_set_ext_click_area(back, 12);
        Book_DisableScroll(back);
        HapticAttachClick(back);
        lv_obj_add_event_cb(back, OnTtfClose, LV_EVENT_CLICKED, nullptr);
        static const lv_point_precise_t left_points[] = {{10, 0}, {0, 12}, {10, 24}};
        auto* left = lv_line_create(back);
        lv_obj_add_flag(left, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_line_set_points(left, left_points, 3);
        lv_obj_set_style_line_width(left, 4, 0);
        lv_obj_set_style_line_color(left, UiColorForeground(), 0);
        lv_obj_set_style_line_rounded(left, true, 0);
        lv_obj_clear_flag(left, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(left);
        lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t* title_lbl = lv_label_create(head);
    if (st.ttf.ttf_multi) {
        char selected[48];
        std::snprintf(selected, sizeof(selected), LangString_BOOK_SELECTED_FMT, TtfSelectedCount());
        lv_label_set_text(title_lbl, selected);
    } else {
        lv_label_set_text(title_lbl, title);
    }
    lv_obj_set_style_text_font(title_lbl, UiFont(UiFontRole::TitleEmphasis), 0);
    lv_obj_clear_flag(title_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_update_layout(st.settings.reader_sheet_body);
    const int frame_height = std::max<int>(1, lv_obj_get_content_height(st.settings.reader_sheet_body) - 6);
    st.ttf.ttf_page = std::clamp(st.ttf.ttf_page, 0, TtfPageCount() - 1);
    const bool list_mode = st.ttf.ttf_mode == BookUiState::TtfMode::kList;
    const int nav_height = list_mode ? std::max<int>(UiSy(44), Book_ItemFont()->line_height + 16) : 0;
    auto* frame = lv_obj_create(st.ttf.ttf_sheet);
    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, lv_pct(100), frame_height);
    lv_obj_set_style_border_width(frame, 2, 0);
    lv_obj_set_style_border_color(frame, UiColorForeground(), 0);
    lv_obj_set_style_border_post(frame, true, 0);
    lv_obj_set_style_radius(frame, UiSx(6), 0);
    lv_obj_set_style_clip_corner(frame, true, 0);
    Book_DisableScroll(frame);
    auto* content = lv_obj_create(frame);
    lv_obj_remove_style_all(content);
    lv_obj_set_pos(content, 2, 2);
    lv_obj_set_size(content, lv_obj_get_content_width(st.settings.reader_sheet_body) - 4,
                    std::max(1, frame_height - nav_height - 4));
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, st.ttf.ttf_mode == BookUiState::TtfMode::kList ? 0 : UiSx(12), 0);
    lv_obj_set_style_pad_row(content, UiSy(4), 0);
    Book_DisableScroll(content);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    if (list_mode) {
        auto* pager = lv_obj_create(frame);
        lv_obj_remove_style_all(pager);
        lv_obj_set_size(pager, lv_obj_get_content_width(st.settings.reader_sheet_body) - 4, nav_height);
        lv_obj_set_pos(pager, 2, frame_height - nav_height - 2);
        lv_obj_set_style_border_side(pager, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(pager, 1, 0);
        lv_obj_set_style_border_color(pager, UiColorForeground(), 0);
        lv_obj_set_flex_flow(pager, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(pager, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        Book_DisableScroll(pager);
        auto* previous = make_button(pager, "<<", OnTtfPagePrev, UiSx(88));
        lv_obj_set_style_border_width(previous, 0, 0);
        auto* meta = lv_label_create(pager);
        lv_obj_set_style_text_font(meta, Book_ItemFont(), 0);
        char page_text[32];
        std::snprintf(page_text, sizeof(page_text), "%d / %d", st.ttf.ttf_page + 1, TtfPageCount());
        lv_label_set_text(meta, page_text);
        st.ttf.ttf_page_label = meta;
        auto* next = make_button(pager, ">>", OnTtfPageNext, UiSx(88));
        lv_obj_set_style_border_width(next, 0, 0);
        SetSheetBtnEnabled(previous, st.ttf.ttf_page > 0);
        SetSheetBtnEnabled(next, st.ttf.ttf_page + 1 < TtfPageCount());
    }
    auto* top_rule = lv_obj_create(st.ttf.ttf_sheet);
    lv_obj_remove_style_all(top_rule);
    lv_obj_add_flag(top_rule, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(top_rule, lv_obj_get_width(st.ttf.ttf_sheet) - 16, 2);
    lv_obj_set_pos(top_rule, 8 - UiSx(16), 0);
    lv_obj_set_style_bg_color(top_rule, UiColorForeground(), 0);
    lv_obj_set_style_bg_opa(top_rule, LV_OPA_COVER, 0);
    Book_DisableScroll(top_rule);
    lv_obj_clear_flag(top_rule, LV_OBJ_FLAG_CLICKABLE);

    switch (st.ttf.ttf_mode) {
        case BookUiState::TtfMode::kList: {
            if (st.ttf.ttf_loading || st.ttf.ttf_failed) {
                Book_ShowMessage(content,
                                 st.ttf.ttf_loading ? LangString_BOOK_FONT_LOADING : StorageErrorText(st.ttf.directory_error),
                                 Book_ItemFont());
                break;
            }
            if (st.ttf.ttf_files.empty()) {
                Book_ShowMessage(content, LangString_TTF_EMPTY, Book_ItemFont());
                break;
            }
            auto* list = content;
            lv_obj_set_style_pad_row(list, 0, 0);
            const int start = 0;
            lv_obj_update_layout(list);
            const int page_height = lv_obj_get_height(list);
            const int minimum_row = ScreenTextHeight(Book_ItemFont()) + 8 + kTocLineThin;
            if (minimum_row * kFontListPageSize > page_height) {
                lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_set_scroll_dir(list, LV_DIR_VER);
            }
            for (int i = 0; i < kFontListPageSize; ++i) {
                const int idx = start + i;
                lv_obj_t* row = lv_obj_create(list);
                lv_obj_remove_style_all(row);
                lv_obj_set_width(row, lv_pct(100));
                lv_obj_set_height(row, std::max<int>(minimum_row, (i + 1) * page_height / kFontListPageSize -
                                                                      i * page_height / kFontListPageSize));
                lv_obj_set_style_pad_hor(row, 12, 0);
                lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
                lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
                if (i + 1 < kFontListPageSize) {
                    lv_obj_set_style_border_width(row, kTocLineThin, 0);
                    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
                    lv_obj_set_style_border_color(row, UiColorForeground(), 0);
                }
                Book_DisableScroll(row);
                lv_obj_t* name = lv_label_create(row);
                lv_obj_set_flex_grow(name, 1);
                lv_obj_set_width(name, 0);
                lv_obj_set_style_text_font(name, Book_ItemFont(), 0);
                lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);
                st.ttf.ttf_rows[i].row = row;
                st.ttf.ttf_rows[i].name = name;
                if (idx >= static_cast<int>(st.ttf.ttf_files.size())) {
                    lv_label_set_text(name, "");
                    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
                    lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
                    continue;
                }
                HapticAttachClick(row);
                lv_obj_add_event_cb(row, OnTtfRowClick, LV_EVENT_SHORT_CLICKED, nullptr);
                lv_obj_add_event_cb(row, OnTtfRowLongPressed, LV_EVENT_LONG_PRESSED, nullptr);
                lv_obj_add_event_cb(row, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
                lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(idx)));
                if (st.ttf.ttf_multi) {
                    auto* check = lv_obj_create(row);
                    lv_obj_remove_style_all(check);
                    lv_obj_set_size(check, UiSx(24), UiSx(24));
                    lv_obj_set_style_border_width(check, 2, 0);
                    lv_obj_set_style_border_color(check, UiColorForeground(), 0);
                    lv_obj_set_style_radius(check, UiSx(4), 0);
                    lv_obj_set_style_bg_color(check, TtfItemSelected(idx) ? UiColorForeground() : UiColorBackground(),
                                              0);
                    lv_obj_set_style_bg_opa(check, LV_OPA_COVER, 0);
                    Book_DisableScroll(check);
                    lv_obj_clear_flag(check, LV_OBJ_FLAG_CLICKABLE);
                    lv_obj_set_style_pad_column(row, UiSx(10), 0);
                }
                const std::string basename = st.ttf.ttf_files[idx].substr(st.ttf.ttf_files[idx].find_last_of('/') + 1);
                Book_SetFilenameMiddle(name, basename.c_str());
            }
            break;
        }
        case BookUiState::TtfMode::kConfirm: {
            ClampTtfSizePx();
            st.ttf.ttf_msg_lbl = MakeTtfFileInfo(content);

            // 字号按单档转换，长按沿用 LVGL 连续调节事件。
            lv_obj_t* size_row = lv_obj_create(content);
            lv_obj_remove_style_all(size_row);
            lv_obj_set_width(size_row, lv_pct(100));
            lv_obj_set_height(size_row, LV_SIZE_CONTENT);
            lv_obj_set_style_border_side(size_row,
                                         static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM), 0);
            lv_obj_set_style_border_width(size_row, 1, 0);
            lv_obj_set_style_pad_ver(size_row, UiSy(8), 0);
            lv_obj_set_flex_flow(size_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(size_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_clear_flag(size_row, LV_OBJ_FLAG_CLICKABLE);
            Book_DisableScroll(size_row);
            lv_obj_t* size_lab = lv_label_create(size_row);
            lv_label_set_text(size_lab, LangString_TTF_SIZE_RANGE);
            lv_obj_set_style_text_font(size_lab, Book_ItemFont(), 0);
            lv_obj_clear_flag(size_lab, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t* stepper = lv_obj_create(size_row);
            lv_obj_remove_style_all(stepper);
            lv_obj_set_height(stepper, LV_SIZE_CONTENT);
            lv_obj_set_width(stepper, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(stepper, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(stepper, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(stepper, 10, 0);
            lv_obj_clear_flag(stepper, LV_OBJ_FLAG_CLICKABLE);
            Book_DisableScroll(stepper);
            st.ttf.ttf_size_dec =
                make_button(stepper, LangString_SETTINGS_FRONTLIGHT_BRIGHTNESS_DEC, OnTtfSizeDec, UiSx(60), true);
            lv_obj_t* size_val = lv_label_create(stepper);
            char size_txt[16];
            std::snprintf(size_txt, sizeof(size_txt), "%dpx", st.ttf.ttf_size_px);
            lv_label_set_text(size_val, size_txt);
            lv_obj_set_style_text_font(size_val, Book_ListFont(), 0);
            lv_obj_clear_flag(size_val, LV_OBJ_FLAG_CLICKABLE);
            st.ttf.ttf_size_value = size_val;
            st.ttf.ttf_size_inc =
                make_button(stepper, LangString_SETTINGS_FRONTLIGHT_BRIGHTNESS_INC, OnTtfSizeInc, UiSx(60), true);

            MakeTtfTip(content, LangString_TTF_OVERWRITE_HINT);
            RefreshTtfConfirmTexts();

            lv_obj_t* btns = lv_obj_create(content);
            lv_obj_remove_style_all(btns);
            lv_obj_set_width(btns, lv_pct(100));
            lv_obj_set_height(btns, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_clear_flag(btns, LV_OBJ_FLAG_CLICKABLE);
            Book_DisableScroll(btns);
            lv_obj_set_style_pad_hor(btns, 0, 0);
            lv_obj_set_style_pad_column(btns, UiSx(18), 0);
            auto* cancel = make_button(btns, LangString_COMMON_CANCEL, OnTtfConfirmNo, 0);
            auto* convert = make_button(btns, LangString_TTF_START, OnTtfConfirmYes, 0);
            lv_obj_set_flex_grow(cancel, 1);
            lv_obj_set_flex_grow(convert, 2);

            lv_obj_update_layout(btns);
            auto* apply_row = lv_obj_create(content);
            lv_obj_remove_style_all(apply_row);
            lv_obj_set_size(apply_row, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(apply_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(apply_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_clear_flag(apply_row, LV_OBJ_FLAG_CLICKABLE);
            Book_DisableScroll(apply_row);
            auto* apply = make_button(apply_row, LangString_TTF_APPLY, OnTtfApply, lv_obj_get_width(convert));
            lv_obj_set_style_bg_color(apply, UiColorForeground(), 0);
            lv_obj_set_style_text_color(lv_obj_get_child(apply, 0), UiColorBackground(), 0);
            break;
        }
        case BookUiState::TtfMode::kProgress: {
            ClampTtfSizePx();
            st.ttf.ttf_msg_lbl = MakeTtfFileInfo(content);

            auto* progress_head = lv_obj_create(content);
            lv_obj_remove_style_all(progress_head);
            lv_obj_set_size(progress_head, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(progress_head, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(progress_head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_border_side(progress_head, LV_BORDER_SIDE_TOP, 0);
            lv_obj_set_style_border_width(progress_head, 1, 0);
            lv_obj_set_style_pad_top(progress_head, UiSy(12), 0);
            Book_DisableScroll(progress_head);
            auto* stage = lv_label_create(progress_head);
            lv_label_set_text(stage, LangString_TTF_GENERATING);
            lv_obj_set_style_text_font(stage, Book_ItemFont(), 0);
            lv_obj_t* pct = lv_label_create(progress_head);
            char pbuf[16];
            std::snprintf(pbuf, sizeof(pbuf), "%d%%", TtfConvertPercent());
            lv_label_set_text(pct, pbuf);
            lv_obj_set_style_text_font(pct, Book_ListFont(), 0);
            lv_obj_clear_flag(pct, LV_OBJ_FLAG_CLICKABLE);
            st.ttf.ttf_pct_lbl = pct;

            lv_obj_t* bar = lv_bar_create(content);
            lv_obj_set_size(bar, lv_pct(100), UiSy(12));
            lv_bar_set_range(bar, 0, 100);
            lv_bar_set_value(bar, TtfConvertPercent(), LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bar, UiColorBackground(), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(bar, UiColorForeground(), 0);
            lv_obj_set_style_border_width(bar, 2, 0);
            lv_obj_set_style_radius(bar, 4, 0);
            lv_obj_set_style_bg_color(bar, UiColorForeground(), LV_PART_INDICATOR);
            lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
            st.ttf.ttf_bar = bar;
            st.ttf.ttf_progress_ui_us = esp_timer_get_time();

            auto* separator = lv_obj_create(content);
            lv_obj_remove_style_all(separator);
            lv_obj_set_size(separator, lv_pct(100), 1);
            lv_obj_set_style_bg_color(separator, UiColorForeground(), 0);
            lv_obj_set_style_bg_opa(separator, LV_OPA_COVER, 0);
            Book_DisableScroll(separator);
            lv_obj_clear_flag(separator, LV_OBJ_FLAG_CLICKABLE);

            MakeTtfTip(content, LangString_TTF_PROGRESS_HINT);
            break;
        }
        case BookUiState::TtfMode::kResult: {
            char buf[96];
            std::snprintf(buf, sizeof(buf), LangString_TTF_FAILED_FMT, TtfConvertError());
            st.ttf.ttf_msg_lbl = MakeTtfTip(content, buf);
            lv_obj_t* btns = lv_obj_create(content);
            lv_obj_remove_style_all(btns);
            lv_obj_set_width(btns, lv_pct(100));
            lv_obj_set_height(btns, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_clear_flag(btns, LV_OBJ_FLAG_CLICKABLE);
            Book_DisableScroll(btns);
            make_button(btns, LangString_TTF_BACK_TO_LIST, OnTtfConfirmNo, UiSx(180));
            break;
        }
        default:
            break;
    }
}

void TtfShowSuccessNotice() {
    auto* text = MakeTtfTip(lv_layer_top(), LangString_TTF_SUCCESS);
    auto* box = lv_obj_get_parent(text);
    lv_obj_set_width(box, std::min<int>(LV_HOR_RES, LV_VER_RES) * 2 / 3);
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(box, UiColorForeground(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(text, UiColorBackground(), 0);
    auto* icon = lv_obj_get_child(box, 0);
    lv_obj_set_style_border_color(icon, UiColorBackground(), 0);
    for (uint32_t i = 0; i < lv_obj_get_child_count(icon); ++i) {
        lv_obj_set_style_bg_color(lv_obj_get_child(icon, i), UiColorBackground(), 0);
    }
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, Book_State().reader.status_h + UiSy(8));
    lv_obj_delete_delayed(box, 3000);
}
