#include "assets/lang_config.h"
#include "assets/lang_reader.h"


#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <esp_timer.h>
#include <lvgl.h>
#include <unistd.h>
#include "ui/apps/book/settings/book_ttf_actions.h"
#include "boards/common/storage/sd_access.h"
#include "infra/storage/sd_paths.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "ui/common/drawing/font/ttf_convert.h"
#include "ui/apps/book/book_font_runtime.h"
#include "ui/apps/book/book_nav.h"
#include "ui/apps/book/book_screen_priv.h"
#include "ui/apps/book/book_text_util.h"
#include "ui/apps/book/reader/book_layout_debounce.h"
#include "ui/apps/book/reader/book_reader_overlay.h"
#include "business/reader/book_reader_prefs.h"
#include "ui/apps/book/settings/book_font_multi.h"
#include "ui/apps/book/settings/book_settings_sheet.h"
#include "ui/apps/book/settings/book_ttf_panel.h"
#include "ui/common/widgets/screen_common.h"

static void ToggleTtfItemSelected(int idx) try {
    auto& ttf = Book_State().ttf;
    if (ttf.ttf_loading || idx < 0 || idx >= static_cast<int>(ttf.ttf_files.size())) {
        return;
    }
    const auto& name = ttf.ttf_files[idx];
    const auto found = std::find(ttf.ttf_selected.begin(), ttf.ttf_selected.end(), name);
    if (found == ttf.ttf_selected.end()) {
        ttf.ttf_selected.push_back(name);
    } else {
        ttf.ttf_selected.erase(found);
    }
} catch (const std::bad_alloc&) {
    ESP_LOGW(TAG, "source font selection allocation failed; selection unchanged");
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
        char out_name[SD_FILE_NAME_CAPACITY + 10];
        TtfConvertFormatOutputFileList(st.ttf.ttf_pick.c_str(), &sz, 1, out_name, sizeof(out_name));
        Book_SetFilenameMiddle(st.ttf.ttf_msg_lbl, out_name);
    }
}

void TtfStartConvert() {
    auto& st = Book_State();
    ClampTtfSizePx();
    // 已有字体复用其字集；空列表使用转换器内置阅读字集。
    std::string tmpl;
    const int fi = CurrentFontIndex();
    if (fi >= 0 && fi < static_cast<int>(st.settings.font_entries.size())) {
        tmpl = std::string(SD_PATH_FONTS) + "/" + st.settings.font_entries[static_cast<size_t>(fi)].file;
    }
    const std::string ttf_path = std::string(SD_PATH_FONT_SRC) + "/" + st.ttf.ttf_pick;
    const uint16_t sizes[1] = {static_cast<uint16_t>(st.ttf.ttf_size_px)};
    if (!TtfConvertStart(ttf_path.c_str(), tmpl.c_str(), SD_PATH_FONTS, sizes, 1)) {
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

void TtfPollTimerCb(lv_timer_t* t) {
    auto& st = Book_State();
    if (TtfConvertBusy()) {
        const int percent = TtfConvertPercent();
        if (st.ttf.ttf_bar && lv_obj_is_valid(st.ttf.ttf_bar) && lv_bar_get_value(st.ttf.ttf_bar) == percent) {
            return;
        }
        constexpr int64_t kProgressUiIntervalUs = 3 * 1000 * 1000;
        const int64_t now = esp_timer_get_time();
        if (percent > 0 && percent < 99 && st.ttf.ttf_progress_ui_us != 0 &&
            now - st.ttf.ttf_progress_ui_us < kProgressUiIntervalUs) {
            return;
        }
        st.ttf.ttf_progress_ui_us = now;
        if (st.ttf.ttf_bar != nullptr && lv_obj_is_valid(st.ttf.ttf_bar)) {
            lv_bar_set_value(st.ttf.ttf_bar, percent, LV_ANIM_OFF);
        }
        if (st.ttf.ttf_pct_lbl != nullptr && lv_obj_is_valid(st.ttf.ttf_pct_lbl)) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d%%", percent);
            lv_label_set_text(st.ttf.ttf_pct_lbl, buf);
        }
        return;
    }
    const int s = TtfConvertState();
    lv_timer_del(t);
    st.ttf.ttf_poll_timer = nullptr;
    // 阅读屏已拆掉：只收尾定时器，勿碰已销毁控件
    if (st.reader.read_scr == nullptr || !lv_obj_is_valid(st.reader.read_scr)) {
        st.ttf.ttf_mode = BookUiState::TtfMode::kNone;
        return;
    }
    st.ttf.ttf_result_ok = (s == 2);
    if (st.ttf.ttf_mode == BookUiState::TtfMode::kProgress && st.settings.settings_sheet != nullptr &&
        lv_obj_is_valid(st.settings.settings_sheet) && st.ttf.ttf_sheet != nullptr &&
        lv_obj_is_valid(st.ttf.ttf_sheet)) {
        st.ttf.ttf_mode = st.ttf.ttf_result_ok ? BookUiState::TtfMode::kList : BookUiState::TtfMode::kResult;
        TtfRebuildContent();
        if (st.ttf.ttf_result_ok) {
            TtfShowSuccessNotice();
        }
    } else {
        // 面板已关：静默收尾；字库仍刷新，下次打开设置可见
        st.ttf.ttf_mode = BookUiState::TtfMode::kNone;
        if (st.reader.read_chrome == BookUiState::ReadChrome::kSettings && st.settings.reader_tab == 1) {
            StartReadFontScan();
            RequestSettingsSheetPaint();
        }
    }
}

void OnTtfRowClick(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = Book_State();
    if (st.reader.opening.load() || TtfConvertBusy() || st.ttf.ttf_loading) {
        return;
    }
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    if (idx < 0 || idx >= static_cast<int>(st.ttf.ttf_files.size())) {
        return;
    }
    if (st.ttf.ttf_multi) {
        ToggleTtfItemSelected(idx);
        RebuildSettingsSheet();
        return;
    }
    st.ttf.ttf_pick = st.ttf.ttf_files[static_cast<size_t>(idx)];
    st.ttf.ttf_size_px = st.ttf.ttf_pick == BookReaderPrefsFontFile() && BookReaderPrefsFontSize() > 0
                             ? BookReaderPrefsFontSize()
                             : kTtfConvertSizeDefault;
    st.ttf.ttf_mode = BookUiState::TtfMode::kConfirm;
    TtfRebuildContent();
}

void OnTtfRowLongPressed(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = Book_State();
    if (st.reader.opening.load() || TtfConvertBusy() || st.ttf.ttf_loading) {
        return;
    }
    auto* row = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(row)));
    if (idx < 0 || idx >= static_cast<int>(st.ttf.ttf_files.size())) {
        return;
    }
    st.ttf.ttf_multi = true;
    ToggleTtfItemSelected(idx);
    RebuildSettingsSheet();
}

int TtfSelectedCount() {
    return static_cast<int>(Book_State().ttf.ttf_selected.size());
}

void OnTtfMultiCancel(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    Book_State().ttf.ttf_selected.clear();
    Book_State().ttf.ttf_multi = false;
    RebuildSettingsSheet();
}

void OnTtfMultiSelectAll(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& ttf = Book_State().ttf;
    if (!ttf.ttf_multi || ttf.ttf_loading) {
        return;
    }
    bool all = !ttf.ttf_files.empty();
    for (int i = 0; i < static_cast<int>(ttf.ttf_files.size()); ++i) {
        all = all && TtfItemSelected(i);
    }
    for (int i = 0; i < static_cast<int>(ttf.ttf_files.size()); ++i) {
        if (TtfItemSelected(i) == all) {
            ToggleTtfItemSelected(i);
        }
    }
    RebuildSettingsSheet();
}

static void DeleteSelectedTtfFiles() {
    auto& st = Book_State();
    if (st.ttf.ttf_selected.empty() || TtfConvertBusy() || st.ttf.ttf_loading) {
        return;
    }
    if (ReaderWorkersBusy() || st.reader.prefs_reopen_pending) {
        ScreenShowToast(LangString_BOOK_LAYOUT_WAIT);
        return;
    }
    SdAccessTransaction transaction;
    if (!transaction) {
        ScreenShowToast(LangString_BOOK_DELETE_FAILED);
        return;
    }
    PowerNeedHold awake(PowerNeed::StandbyInhibit);
    std::vector<std::string> files;
    try {
        files = st.ttf.ttf_selected;
    } catch (const std::bad_alloc&) {
        ESP_LOGW(TAG, "source font delete snapshot allocation failed; selection retained");
        return;
    }
    if (BookReaderPrefsFontSize() > 0 &&
        std::find(files.begin(), files.end(), BookReaderPrefsFontFile()) != files.end()) {
        if (!st.reader.session || !st.reader.session->IsOpen() || st.reader.standby_paused ||
            st.reader.viewport_w <= 0 || st.reader.viewport_h <= 0) {
            return;
        }
        const auto fallback = std::find_if(st.settings.font_entries.begin(), st.settings.font_entries.end(),
                                           [&](const ReadFontEntry& entry) {
                                               return std::find(files.begin(), files.end(), entry.file) == files.end();
                                           });
        const char* file =
            fallback == st.settings.font_entries.end() ? kBookReaderDefaultFontFile : fallback->file.c_str();
        BookReaderPrefsSetFontFile(file, IsTtfFileName(file) ? 40 : 0);
        // 同步完成旧字体关闭与 session/正文重绑，再删除源文件。
        ApplyReaderLayoutLive(true);
    }
    bool failed = false;
    for (const auto& name : files) {
        if (!SdIsValidFileName(name.c_str()) || !IsTtfFileName(name.c_str())) {
            failed = true;
            continue;
        }
        const std::string path = std::string(SD_PATH_FONT_SRC) + "/" + name;
        if (unlink(path.c_str()) != 0 && errno != ENOENT) {
            failed = true;
            ESP_LOGW(TAG, "source font delete failed errno=%d", errno);
        }
    }
    st.ttf.ttf_page = 0;
    TtfPanelClose();
    st.ttf.ttf_mode = BookUiState::TtfMode::kList;
    TtfScanFiles();
    RebuildSettingsSheet();
    if (failed) {
        ScreenShowToast(LangString_BOOK_DELETE_FAILED);
    }
}

void OnTtfMultiRemove(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const auto& st = Book_State();
    if (!st.ttf.ttf_selected.empty() && !TtfConvertBusy() && !st.ttf.ttf_loading) {
        ScreenConfirmDelete(lv_screen_active(), DeleteSelectedTtfFiles);
    }
}

void OnTtfSizeDec(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const auto code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_LONG_PRESSED_REPEAT) {
        return;
    }
    auto& st = Book_State();
    if (st.ttf.ttf_mode != BookUiState::TtfMode::kConfirm || st.reader.opening.load() || TtfConvertBusy()) {
        return;
    }
    if (st.ttf.ttf_size_px > kTtfConvertSizeMin) {
        --st.ttf.ttf_size_px;
        RefreshTtfConfirmTexts();
    }
}

void OnTtfSizeInc(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const auto code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_LONG_PRESSED_REPEAT) {
        return;
    }
    auto& st = Book_State();
    if (st.ttf.ttf_mode != BookUiState::TtfMode::kConfirm || st.reader.opening.load() || TtfConvertBusy()) {
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
    if (st.ttf.ttf_loading) {
        return;
    }
    if (st.ttf.ttf_page > 0) {
        --st.ttf.ttf_page;
        TtfScanFiles();
        TtfRebuildContent();
    }
}

void OnTtfPageNext(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = Book_State();
    if (st.ttf.ttf_page + 1 < TtfPageCount()) {
        if (st.ttf.ttf_loading) {
            return;
        }
        ++st.ttf.ttf_page;
        TtfScanFiles();
        TtfRebuildContent();
    }
}

void OnTtfConfirmYes(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = Book_State();
    if (st.reader.opening.load() || TtfConvertBusy()) {
        return;
    }
    TtfStartConvert();
}

void OnTtfApply(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = Book_State();
    if (st.ttf.ttf_mode != BookUiState::TtfMode::kConfirm || TtfConvertBusy()) {
        return;
    }
    ClampTtfSizePx();
    if (!PrepareBookFont(st.ttf.ttf_pick.c_str(), st.ttf.ttf_size_px)) {
        ScreenShowToast(LangString_TTF_APPLY_FAILED);
        return;
    }
    ScheduleLayoutApply(true);
    HideReadChrome();
}

void OnTtfConfirmNo(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    ReturnToTtfList();
}

void OnTtfClose(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    if (Book_State().ttf.ttf_multi) {
        OnTtfMultiCancel(e);
    } else if (Book_State().ttf.ttf_mode == BookUiState::TtfMode::kList) {
        SelectReadingSheetTab(1);
    } else {
        ReturnToTtfList();
    }
}

void ReturnToTtfList() {
    StopTtfPollTimer();
    Book_State().ttf.ttf_mode = BookUiState::TtfMode::kList;
    RebuildSettingsSheet();
}

bool TtfItemSelected(int idx) {
    const auto& ttf = Book_State().ttf;
    if (idx < 0 || idx >= static_cast<int>(ttf.ttf_files.size())) {
        return false;
    }
    return std::find(ttf.ttf_selected.begin(), ttf.ttf_selected.end(), ttf.ttf_files[idx]) != ttf.ttf_selected.end();
}
