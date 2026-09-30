#pragma GCC optimize("O1")

#include "book_screen/settings/book_ttf_actions.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/settings/book_font_multi.h"
#include "book_screen/settings/book_tap_ui.h"
#include "book_screen/settings/book_ttf_panel.h"
#include "book_screen/reader/book_reader_overlay.h"
#include "book_screen/settings/book_settings_sheet.h"

#include <lvgl.h>
#include <cstdint>
#include <cstdio>
#include <string>

#include "display/font/ttf_convert.h"
#include "sd_paths.h"

void OnFontImportClick(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = Book_State();
    if (st.reader.opening.load() || ttf_convert::Busy()) {
        return;
    }
    if (st.settings.font_multi) {
        return;
    }
    // 与触摸分区浮层互斥
    TapZonesPanelClose();
    TtfScanFiles();
    st.ttf.ttf_page = 0;
    st.ttf.ttf_mode = BookUiState::TtfMode::kList;
    TtfEnsurePanel();
}

void OnTtfRowClick(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = Book_State();
    if (st.reader.opening.load()) {
        return;
    }
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    if (idx < 0 || idx >= static_cast<int>(st.ttf.ttf_files.size())) {
        return;
    }
    st.ttf.ttf_pick = st.ttf.ttf_files[static_cast<size_t>(idx)];
    st.ttf.ttf_size_px = kTtfConvertSizeDefault;
    st.ttf.ttf_mode = BookUiState::TtfMode::kConfirm;
    TtfRebuildContent();
}

void ClampTtfSizePx() {
    auto& st = Book_State();
    if (st.ttf.ttf_size_px < kTtfConvertSizeMin) {
        st.ttf.ttf_size_px = kTtfConvertSizeMin;
    } else if (st.ttf.ttf_size_px > kTtfConvertSizeMax) {
        st.ttf.ttf_size_px = kTtfConvertSizeMax;
    }
}

void RefreshTtfConfirmTexts() {
    auto& st = Book_State();
    ClampTtfSizePx();
    if (st.ttf.ttf_size_value != nullptr && lv_obj_is_valid(st.ttf.ttf_size_value)) {
        char size_txt[16];
        std::snprintf(size_txt, sizeof(size_txt), "%dpx", st.ttf.ttf_size_px);
        lv_label_set_text(st.ttf.ttf_size_value, size_txt);
    }
    SetSheetBtnEnabled(st.ttf.ttf_size_dec, st.ttf.ttf_size_px > kTtfConvertSizeMin);
    SetSheetBtnEnabled(st.ttf.ttf_size_inc, st.ttf.ttf_size_px < kTtfConvertSizeMax);
    if (st.ttf.ttf_msg_lbl != nullptr && lv_obj_is_valid(st.ttf.ttf_msg_lbl)) {
        const uint16_t sz = static_cast<uint16_t>(st.ttf.ttf_size_px);
        char out_name[80];
        ttf_convert::FormatOutputFileList(st.ttf.ttf_pick.c_str(), &sz, 1, out_name, sizeof(out_name));
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "将 %.32s 转为：\n%s\n约需 1-2 分钟，同名 .ef 将被覆盖。",
                      st.ttf.ttf_pick.c_str(), out_name);
        lv_label_set_text(st.ttf.ttf_msg_lbl, buf);
    }
}

void OnTtfSizeDec(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = Book_State();
    if (st.ttf.ttf_mode != BookUiState::TtfMode::kConfirm || st.reader.opening.load() ||
        ttf_convert::Busy()) {
        return;
    }
    if (st.ttf.ttf_size_px > kTtfConvertSizeMin) {
        --st.ttf.ttf_size_px;
        RefreshTtfConfirmTexts();
    }
}

void OnTtfSizeInc(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = Book_State();
    if (st.ttf.ttf_mode != BookUiState::TtfMode::kConfirm || st.reader.opening.load() ||
        ttf_convert::Busy()) {
        return;
    }
    if (st.ttf.ttf_size_px < kTtfConvertSizeMax) {
        ++st.ttf.ttf_size_px;
        RefreshTtfConfirmTexts();
    }
}

void OnTtfPagePrev(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = Book_State();
    if (st.ttf.ttf_page > 0) {
        --st.ttf.ttf_page;
        TtfRebuildContent();
    }
}

void OnTtfPageNext(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = Book_State();
    if (st.ttf.ttf_page + 1 < TtfPageCount()) {
        ++st.ttf.ttf_page;
        TtfRebuildContent();
    }
}

void TtfStartConvert() {
    auto& st = Book_State();
    ClampTtfSizePx();
    // 字集模板：当前选中 .ef；无则回退默认 misans
    std::string tmpl;
    const int fi = CurrentFontIndex();
    if (fi >= 0 && fi < static_cast<int>(st.settings.font_entries.size())) {
        tmpl = std::string(SD_PATH_FONTS) + "/" + st.settings.font_entries[static_cast<size_t>(fi)].file;
    } else {
        tmpl = SD_PATH_BOOK_FONT;
    }
    const std::string ttf_path = std::string(SD_PATH_FONT_SRC) + "/" + st.ttf.ttf_pick;
    const uint16_t sizes[1] = {static_cast<uint16_t>(st.ttf.ttf_size_px)};
    if (!ttf_convert::Start(ttf_path.c_str(), tmpl.c_str(), SD_PATH_FONTS, sizes, 1)) {
        st.ttf.ttf_result_ok = false;
        st.ttf.ttf_mode = BookUiState::TtfMode::kResult;
        TtfRebuildContent();
        return;
    }
    st.ttf.ttf_mode = BookUiState::TtfMode::kProgress;
    TtfRebuildContent();
    if (st.ttf.ttf_poll_timer == nullptr) {
        st.ttf.ttf_poll_timer = lv_timer_create(TtfPollTimerCb, 500, nullptr);
    }
}

void OnTtfConfirmYes(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = Book_State();
    if (st.reader.opening.load() || ttf_convert::Busy()) {
        return;
    }
    TtfStartConvert();
}

void OnTtfConfirmNo(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = Book_State();
    st.ttf.ttf_mode = BookUiState::TtfMode::kList;
    TtfRebuildContent();
}

void OnTtfClose(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    TtfPanelClose();
}

void OnTtfResultOk(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    TtfPanelClose();
}

void TtfPollTimerCb(lv_timer_t* t) {
    auto& st = Book_State();
    if (ttf_convert::Busy()) {
        if (st.ttf.ttf_bar != nullptr && lv_obj_is_valid(st.ttf.ttf_bar)) {
            lv_bar_set_value(st.ttf.ttf_bar, ttf_convert::Percent(), LV_ANIM_OFF);
        }
        if (st.ttf.ttf_pct_lbl != nullptr && lv_obj_is_valid(st.ttf.ttf_pct_lbl)) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d%%", ttf_convert::Percent());
            lv_label_set_text(st.ttf.ttf_pct_lbl, buf);
        }
        return;
    }
    const int s = ttf_convert::State();
    lv_timer_del(t);
    st.ttf.ttf_poll_timer = nullptr;
    // 阅读屏已拆掉：只收尾定时器，勿碰已销毁控件
    if (st.reader.read_scr == nullptr || !lv_obj_is_valid(st.reader.read_scr)) {
        st.ttf.ttf_mode = BookUiState::TtfMode::kNone;
        return;
    }
    ScanReadFonts();
    st.ttf.ttf_result_ok = (s == 2);
    if (st.ttf.ttf_mode == BookUiState::TtfMode::kProgress && st.settings.settings_sheet != nullptr &&
        lv_obj_is_valid(st.settings.settings_sheet) && st.ttf.ttf_sheet != nullptr &&
        lv_obj_is_valid(st.ttf.ttf_sheet)) {
        st.ttf.ttf_mode = BookUiState::TtfMode::kResult;
        TtfRebuildContent();
        ClampFontListPage();
        RefreshSettingsSheetFontList();
    } else {
        // 面板已关：静默收尾；字库仍刷新，下次打开设置可见
        st.ttf.ttf_mode = BookUiState::TtfMode::kNone;
        ClampFontListPage();
        if (st.settings.settings_sheet != nullptr && lv_obj_is_valid(st.settings.settings_sheet)) {
            RefreshSettingsSheetFontList();
        }
    }
}

