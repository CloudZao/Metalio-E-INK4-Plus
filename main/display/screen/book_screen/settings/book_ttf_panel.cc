#pragma GCC optimize("O1")

#include "book_screen/settings/book_ttf_panel.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/shelf/book_bookshelf.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/book_text_util.h"
#include "book_screen/settings/book_ttf_actions.h"
#include "book_screen/reader/book_reader_overlay.h"
#include "book_screen/settings/book_settings_sheet.h"

#include <lvgl.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <strings.h>

#include "display/font/ttf_convert.h"
#include "haptic_feedback.h"
#include "sd_paths.h"

bool IsTtfFileName(const char* name) {
    const size_t n = std::strlen(name);
    if (n < 5 || n > 63) {
        return false;
    }
    return strcasecmp(name + n - 4, ".ttf") == 0 || strcasecmp(name + n - 4, ".otf") == 0;
}

int TtfPageCount() {
    const int n = static_cast<int>(Book_State().ttf.ttf_files.size());
    if (n <= 0) {
        return 1;
    }
    return (n + kFontListPageSize - 1) / kFontListPageSize;
}

void TtfScanFiles() {
    auto& st = Book_State();
    st.ttf.ttf_files.clear();
    DIR* dir = opendir(SD_PATH_FONT_SRC);
    if (dir == nullptr) {
        return;
    }
    while (dirent* ent = readdir(dir)) {
        if (ent->d_name[0] == '.' || !IsTtfFileName(ent->d_name)) {
            continue;
        }
        st.ttf.ttf_files.push_back(ent->d_name);
    }
    closedir(dir);
    std::sort(st.ttf.ttf_files.begin(), st.ttf.ttf_files.end());
}

void TtfPanelClose() {
    auto& st = Book_State();
    if (st.ttf.ttf_sheet != nullptr && lv_obj_is_valid(st.ttf.ttf_sheet)) {
        lv_obj_del(st.ttf.ttf_sheet);
    }
    st.ttf.ttf_sheet = nullptr;
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
    auto& st = Book_State();
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
    // 设置卡是 COLUMN + SIZE_CONTENT：overlay 必须 FLOATING，否则会被 flex 排到卡底，
    // 且相对 content 父级的 pct(100) 高度常为 0 → 点「导入 TTF」像没反应。
    lv_obj_update_layout(st.settings.settings_sheet);
    if (st.ttf.ttf_sheet != nullptr && lv_obj_is_valid(st.ttf.ttf_sheet)) {
        lv_obj_set_size(st.ttf.ttf_sheet, lv_pct(100), lv_pct(100));
        lv_obj_align(st.ttf.ttf_sheet, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_move_foreground(st.ttf.ttf_sheet);
        TtfRebuildContent();
        return;
    }
    st.ttf.ttf_sheet = lv_obj_create(st.settings.settings_sheet);
    lv_obj_remove_style_all(st.ttf.ttf_sheet);
    lv_obj_add_flag(st.ttf.ttf_sheet, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(st.ttf.ttf_sheet, lv_pct(100), lv_pct(100));
    lv_obj_align(st.ttf.ttf_sheet, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(st.ttf.ttf_sheet, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(st.ttf.ttf_sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(st.ttf.ttf_sheet, 12, 0);
    lv_obj_set_style_pad_all(st.ttf.ttf_sheet, 16, 0);
    lv_obj_set_style_pad_row(st.ttf.ttf_sheet, 10, 0);
    lv_obj_set_flex_flow(st.ttf.ttf_sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(st.ttf.ttf_sheet, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(st.ttf.ttf_sheet);
    lv_obj_move_foreground(st.ttf.ttf_sheet);
    TtfRebuildContent();
}

void TtfRebuildContent() {
    auto& st = Book_State();
    if (st.ttf.ttf_sheet == nullptr || !lv_obj_is_valid(st.ttf.ttf_sheet)) {
        return;
    }
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

    const bool is_progress = (st.ttf.ttf_mode == BookUiState::TtfMode::kProgress);
    auto make_action = [](lv_obj_t* parent, const char* text, lv_event_cb_t cb) -> lv_obj_t* {
        lv_obj_t* btn = lv_obj_create(parent);
        lv_obj_remove_style_all(btn);
        lv_obj_set_height(btn, 36);
        lv_obj_set_width(btn, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_hor(btn, 4, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        Book_DisableScroll(btn);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(btn);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_add_event_cb(
            btn, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* wrap = lv_obj_create(btn);
        lv_obj_remove_style_all(wrap);
        lv_obj_set_size(wrap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_border_side(wrap, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(wrap, kShelfUnderlineH, 0);
        lv_obj_set_style_border_color(wrap, lv_color_black(), 0);
        lv_obj_set_style_pad_bottom(wrap, 2, 0);
        lv_obj_set_style_pad_hor(wrap, kShelfUnderlinePadHor, 0);
        lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
        Book_DisableScroll(wrap);
        lv_obj_clear_flag(wrap, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* lbl = lv_label_create(wrap);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, Book_ItemFont(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        return btn;
    };

    // 头部：标题 + 关闭（进度模式不可关）
    lv_obj_t* head = lv_obj_create(st.ttf.ttf_sheet);
    lv_obj_remove_style_all(head);
    lv_obj_set_width(head, lv_pct(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(head);
    const char* title = "";
    switch (st.ttf.ttf_mode) {
        case BookUiState::TtfMode::kList: title = "选择 TTF 字体"; break;
        case BookUiState::TtfMode::kConfirm: title = "导入 TTF"; break;
        case BookUiState::TtfMode::kProgress: title = "转换中"; break;
        case BookUiState::TtfMode::kResult: title = st.ttf.ttf_result_ok ? "转换完成" : "转换失败"; break;
        default: break;
    }
    lv_obj_t* title_lbl = lv_label_create(head);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, Book_ListFont(), 0);
    lv_obj_clear_flag(title_lbl, LV_OBJ_FLAG_CLICKABLE);
    if (!is_progress) {
        make_action(head, "关闭", OnTtfClose);
    }

    switch (st.ttf.ttf_mode) {
        case BookUiState::TtfMode::kList: {
            if (st.ttf.ttf_files.empty()) {
                lv_obj_t* empty = lv_label_create(st.ttf.ttf_sheet);
                lv_label_set_text(
                    empty, "未找到 TTF/OTF 字体\n请将字体文件放入 SD 卡 metalio/e-ink/fonts_ttf 目录");
                lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
                lv_obj_set_width(empty, lv_pct(100));
                lv_obj_set_style_text_font(empty, Book_ItemFont(), 0);
                lv_obj_set_style_text_opa(empty, LV_OPA_70, 0);
                lv_obj_clear_flag(empty, LV_OBJ_FLAG_CLICKABLE);
                break;
            }
            lv_obj_t* list = lv_obj_create(st.ttf.ttf_sheet);
            lv_obj_remove_style_all(list);
            lv_obj_set_width(list, lv_pct(100));
            lv_obj_set_height(list, kFontListRowH * kFontListPageSize);
            lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
            lv_obj_clear_flag(list, LV_OBJ_FLAG_CLICKABLE);
            Book_DisableScroll(list);
            const int start = st.ttf.ttf_page * kFontListPageSize;
            for (int i = 0; i < kFontListPageSize; ++i) {
                const int idx = start + i;
                lv_obj_t* row = lv_obj_create(list);
                lv_obj_remove_style_all(row);
                lv_obj_set_width(row, lv_pct(100));
                lv_obj_set_height(row, kFontListRowH);
                lv_obj_set_style_pad_hor(row, 12, 0);
                lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
                lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                                      LV_FLEX_ALIGN_CENTER);
                if (i + 1 < kFontListPageSize) {
                    lv_obj_set_style_border_width(row, kTocLineThin, 0);
                    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
                    lv_obj_set_style_border_color(row, lv_color_black(), 0);
                }
                Book_DisableScroll(row);
                lv_obj_t* name = lv_label_create(row);
                lv_obj_set_flex_grow(name, 1);
                lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
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
                lv_obj_add_event_cb(row, OnTtfRowClick, LV_EVENT_CLICKED, nullptr);
                lv_obj_add_event_cb(
                    row, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED,
                    nullptr);
                lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(idx)));
                lv_label_set_text(name, st.ttf.ttf_files[static_cast<size_t>(idx)].c_str());
            }
            const int pages = TtfPageCount();
            if (pages > 1) {
                lv_obj_t* pager = lv_obj_create(st.ttf.ttf_sheet);
                lv_obj_remove_style_all(pager);
                lv_obj_set_width(pager, lv_pct(100));
                lv_obj_set_height(pager, 36);
                lv_obj_set_flex_flow(pager, LV_FLEX_FLOW_ROW);
                lv_obj_set_flex_align(pager, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                                      LV_FLEX_ALIGN_CENTER);
                lv_obj_clear_flag(pager, LV_OBJ_FLAG_CLICKABLE);
                Book_DisableScroll(pager);
                make_action(pager, "▲", OnTtfPagePrev);
                char meta[32];
                std::snprintf(meta, sizeof(meta), "%d / %d", st.ttf.ttf_page + 1, pages);
                lv_obj_t* lab = lv_label_create(pager);
                lv_label_set_text(lab, meta);
                lv_obj_set_style_text_font(lab, Book_ItemFont(), 0);
                lv_obj_set_style_text_opa(lab, LV_OPA_70, 0);
                lv_obj_clear_flag(lab, LV_OBJ_FLAG_CLICKABLE);
                make_action(pager, "▼", OnTtfPageNext);
            }
            break;
        }
        case BookUiState::TtfMode::kConfirm: {
            ClampTtfSizePx();
            lv_obj_t* msg = lv_label_create(st.ttf.ttf_sheet);
            lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(msg, lv_pct(100));
            lv_obj_set_style_text_font(msg, Book_ItemFont(), 0);
            lv_obj_clear_flag(msg, LV_OBJ_FLAG_CLICKABLE);
            st.ttf.ttf_msg_lbl = msg;

            // 字号步进器：20–40，步进 1，默认 25；只生成一档
            lv_obj_t* size_row = lv_obj_create(st.ttf.ttf_sheet);
            lv_obj_remove_style_all(size_row);
            lv_obj_set_width(size_row, lv_pct(100));
            lv_obj_set_height(size_row, kSheetIconBtn);
            lv_obj_set_flex_flow(size_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(size_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_clear_flag(size_row, LV_OBJ_FLAG_CLICKABLE);
            Book_DisableScroll(size_row);
            lv_obj_t* size_lab = lv_label_create(size_row);
            lv_label_set_text(size_lab, "字号");
            lv_obj_set_style_text_font(size_lab, Book_ItemFont(), 0);
            lv_obj_clear_flag(size_lab, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t* stepper = lv_obj_create(size_row);
            lv_obj_remove_style_all(stepper);
            lv_obj_set_height(stepper, kSheetIconBtn);
            lv_obj_set_width(stepper, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(stepper, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(stepper, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(stepper, 10, 0);
            lv_obj_clear_flag(stepper, LV_OBJ_FLAG_CLICKABLE);
            Book_DisableScroll(stepper);
            st.ttf.ttf_size_dec =
                MakeSheetIconBtn(stepper, "-", OnTtfSizeDec, st.ttf.ttf_size_px > kTtfConvertSizeMin);
            lv_obj_t* size_val = lv_label_create(stepper);
            char size_txt[16];
            std::snprintf(size_txt, sizeof(size_txt), "%dpx", st.ttf.ttf_size_px);
            lv_label_set_text(size_val, size_txt);
            lv_obj_set_style_text_font(size_val, Book_ListFont(), 0);
            lv_obj_clear_flag(size_val, LV_OBJ_FLAG_CLICKABLE);
            st.ttf.ttf_size_value = size_val;
            st.ttf.ttf_size_inc =
                MakeSheetIconBtn(stepper, "+", OnTtfSizeInc, st.ttf.ttf_size_px < kTtfConvertSizeMax);

            RefreshTtfConfirmTexts();

            lv_obj_t* btns = lv_obj_create(st.ttf.ttf_sheet);
            lv_obj_remove_style_all(btns);
            lv_obj_set_width(btns, lv_pct(100));
            lv_obj_set_height(btns, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_hor(btns, 24, 0);
            lv_obj_clear_flag(btns, LV_OBJ_FLAG_CLICKABLE);
            Book_DisableScroll(btns);
            make_action(btns, "开始转换", OnTtfConfirmYes);
            make_action(btns, "取消", OnTtfConfirmNo);
            break;
        }
        case BookUiState::TtfMode::kProgress: {
            ClampTtfSizePx();
            const uint16_t sz = static_cast<uint16_t>(st.ttf.ttf_size_px);
            char out_name[80];
            ttf_convert::FormatOutputFileList(st.ttf.ttf_pick.c_str(), &sz, 1, out_name,
                                              sizeof(out_name));
            lv_obj_t* msg = lv_label_create(st.ttf.ttf_sheet);
            char mbuf[200];
            std::snprintf(mbuf, sizeof(mbuf),
                          "正在转换：\n%.40s\n→ %s\n请勿拔出 SD 卡…", st.ttf.ttf_pick.c_str(),
                          out_name);
            lv_label_set_text(msg, mbuf);
            lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(msg, lv_pct(100));
            lv_obj_set_style_text_font(msg, Book_ItemFont(), 0);
            lv_obj_clear_flag(msg, LV_OBJ_FLAG_CLICKABLE);
            st.ttf.ttf_msg_lbl = msg;

            lv_obj_t* bar = lv_bar_create(st.ttf.ttf_sheet);
            lv_obj_set_size(bar, lv_pct(100), 16);
            lv_bar_set_range(bar, 0, 100);
            lv_bar_set_value(bar, ttf_convert::Percent(), LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(bar, lv_color_black(), 0);
            lv_obj_set_style_border_width(bar, 2, 0);
            lv_obj_set_style_radius(bar, 4, 0);
            lv_obj_set_style_bg_color(bar, lv_color_black(), LV_PART_INDICATOR);
            lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
            st.ttf.ttf_bar = bar;

            lv_obj_t* pct = lv_label_create(st.ttf.ttf_sheet);
            char pbuf[16];
            std::snprintf(pbuf, sizeof(pbuf), "%d%%", ttf_convert::Percent());
            lv_label_set_text(pct, pbuf);
            lv_obj_set_style_text_font(pct, Book_ListFont(), 0);
            lv_obj_clear_flag(pct, LV_OBJ_FLAG_CLICKABLE);
            st.ttf.ttf_pct_lbl = pct;

            lv_obj_t* tip = lv_label_create(st.ttf.ttf_sheet);
            lv_label_set_text(tip, "转换完成前，请勿退出页面");
            lv_label_set_long_mode(tip, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(tip, lv_pct(100));
            lv_obj_set_style_text_font(tip, Book_ItemFont(), 0);
            lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_clear_flag(tip, LV_OBJ_FLAG_CLICKABLE);
            break;
        }
        case BookUiState::TtfMode::kResult: {
            lv_obj_t* msg = lv_label_create(st.ttf.ttf_sheet);
            if (st.ttf.ttf_result_ok) {
                const uint16_t sz = static_cast<uint16_t>(st.ttf.ttf_size_px);
                char out_name[80];
                ttf_convert::FormatOutputFileList(st.ttf.ttf_pick.c_str(), &sz, 1, out_name,
                                                  sizeof(out_name));
                char buf[160];
                std::snprintf(buf, sizeof(buf), "转换完成：\n%s\n已加入字库列表。", out_name);
                lv_label_set_text(msg, buf);
            } else {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "转换失败：%.60s", ttf_convert::Error());
                lv_label_set_text(msg, buf);
            }
            lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(msg, lv_pct(100));
            lv_obj_set_style_text_font(msg, Book_ItemFont(), 0);
            lv_obj_clear_flag(msg, LV_OBJ_FLAG_CLICKABLE);
            st.ttf.ttf_msg_lbl = msg;
            lv_obj_t* btns = lv_obj_create(st.ttf.ttf_sheet);
            lv_obj_remove_style_all(btns);
            lv_obj_set_width(btns, lv_pct(100));
            lv_obj_set_height(btns, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_clear_flag(btns, LV_OBJ_FLAG_CLICKABLE);
            Book_DisableScroll(btns);
            make_action(btns, "确定", OnTtfResultOk);
            break;
        }
        default:
            break;
    }
}

