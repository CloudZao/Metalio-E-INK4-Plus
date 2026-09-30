#pragma GCC optimize("O1")

#include "book_screen/settings/book_font_multi.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/book_geom.h"
#include "book_screen/reader/book_layout_debounce.h"
#include "book_screen/reader/book_reader_prefs.h"
#include "book_screen/book_nav.h"
#include "book_screen/reader/book_reader_body.h"
#include "book_screen/reader/book_reader_overlay.h"
#include "book_screen/settings/book_settings_sheet.h"

#include <lvgl.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <esp_log.h>
#include <esp_timer.h>

#include "assets/lang_config.h"
#include "sd_paths.h"

bool ParseEfSizePx(const std::string& name, int* size_px) {
    if (size_px == nullptr || name.size() < 8) {
        return false;
    }
    const size_t dot = name.rfind(".ef");
    if (dot == std::string::npos || dot + 3 != name.size()) {
        return false;
    }
    const size_t u2 = name.rfind('_', dot);
    if (u2 == std::string::npos || u2 == 0) {
        return false;
    }
    const size_t u1 = name.rfind('_', u2 - 1);
    if (u1 == std::string::npos) {
        return false;
    }
    int sz = 0;
    for (size_t i = u1 + 1; i < u2; ++i) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
        sz = sz * 10 + (name[i] - '0');
    }
    if (sz <= 0) {
        return false;
    }
    *size_px = sz;
    return true;
}

bool ReadFontEntryLess(const ReadFontEntry& a, const ReadFontEntry& b) {
    if (a.size_px != b.size_px) {
        return a.size_px < b.size_px;
    }
    return a.file < b.file;
}

void ScanReadFonts() {
    auto& st = Book_State();
    st.settings.font_entries.clear();
    DIR* dir = opendir(SD_PATH_FONTS);
    if (dir == nullptr) {
        return;
    }
    while (dirent* ent = readdir(dir)) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        const size_t n = std::strlen(ent->d_name);
        if (n < 4 || n > 63 || std::strcmp(ent->d_name + (n - 3), ".ef") != 0) {
            continue;
        }
        char name[64];
        std::memcpy(name, ent->d_name, n + 1);
        char full[96];
        std::snprintf(full, sizeof(full), "%s/%s", SD_PATH_FONTS, name);
        struct stat st_info {};
        if (stat(full, &st_info) != 0 || !S_ISREG(st_info.st_mode)) {
            continue;
        }
        ReadFontEntry fe;
        fe.file = name;
        if (!ParseEfSizePx(fe.file, &fe.size_px)) {
            fe.size_px = 0;
        }
        st.settings.font_entries.push_back(std::move(fe));
    }
    closedir(dir);
    std::sort(st.settings.font_entries.begin(), st.settings.font_entries.end(), ReadFontEntryLess);
}

int CurrentFontIndex() {
    auto& st = Book_State();
    if (st.settings.font_entries.empty()) {
        ScanReadFonts();
    }
    const char* cur = BookReaderPrefsFontFile();
    for (int i = 0; i < static_cast<int>(st.settings.font_entries.size()); ++i) {
        if (st.settings.font_entries[static_cast<size_t>(i)].file == cur) {
            return i;
        }
    }
    return st.settings.font_entries.empty() ? -1 : 0;
}

int FontListPageCount() {
    const int n = static_cast<int>(Book_State().settings.font_entries.size());
    if (n <= 0) {
        return 1;
    }
    return (n + kFontListPageSize - 1) / kFontListPageSize;
}

void ClampFontListPage() {
    auto& st = Book_State();
    const int pages = FontListPageCount();
    if (st.settings.font_list_page < 0) {
        st.settings.font_list_page = 0;
    }
    if (st.settings.font_list_page >= pages) {
        st.settings.font_list_page = pages - 1;
    }
}

// ---------- TTF 导入转换面板（覆盖设置卡；转换 worker 独立，UI 只轮询） ----------

void EnsureFontListPageShowsSelection() {
    auto& st = Book_State();
    const int cur = CurrentFontIndex();
    if (cur < 0) {
        st.settings.font_list_page = 0;
        return;
    }
    st.settings.font_list_page = cur / kFontListPageSize;
    ClampFontListPage();
}

// 列表主文案：去掉 .ef；过长截断
void FormatFontListName(const std::string& file, char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    std::string stem = file;
    if (stem.size() > 3 && stem.compare(stem.size() - 3, 3, ".ef") == 0) {
        stem.resize(stem.size() - 3);
    }
    if (stem.empty()) {
        stem = file;
    }
    // 墨水屏行宽有限：过长用省略
    constexpr size_t kMax = 22;
    if (stem.size() <= kMax) {
        std::snprintf(out, out_len, "%s", stem.c_str());
        return;
    }
    std::snprintf(out, out_len, "%.19s...", stem.c_str());
}

void SyncFontSelectedSize() {
    auto& st = Book_State();
    if (st.settings.font_selected.size() != st.settings.font_entries.size()) {
        st.settings.font_selected.assign(st.settings.font_entries.size(), 0);
    }
}

int FontSelectedCount() {
    SyncFontSelectedSize();
    int n = 0;
    for (uint8_t v : Book_State().settings.font_selected) {
        if (v != 0) {
            ++n;
        }
    }
    return n;
}

bool FontItemSelected(int idx) {
    SyncFontSelectedSize();
    auto& st = Book_State();
    return idx >= 0 && idx < static_cast<int>(st.settings.font_selected.size()) &&
           st.settings.font_selected[static_cast<size_t>(idx)] != 0;
}

void ToggleFontItemSelected(int idx) {
    SyncFontSelectedSize();
    auto& st = Book_State();
    if (idx < 0 || idx >= static_cast<int>(st.settings.font_selected.size())) {
        return;
    }
    st.settings.font_selected[static_cast<size_t>(idx)] =
        st.settings.font_selected[static_cast<size_t>(idx)] ? 0 : 1;
}

void RefreshFontMultiFooter() {
    auto& st = Book_State();
    if (st.settings.font_multi) {
        if (st.settings.sheet_font_multi_bar != nullptr && lv_obj_is_valid(st.settings.sheet_font_multi_bar)) {
            lv_obj_clear_flag(st.settings.sheet_font_multi_bar, LV_OBJ_FLAG_HIDDEN);
        }
        if (st.settings.sheet_font_title != nullptr && lv_obj_is_valid(st.settings.sheet_font_title)) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), Lang::Strings::BOOK_SELECTED_FMT, FontSelectedCount());
            lv_label_set_text(st.settings.sheet_font_title, buf);
            lv_obj_set_style_text_opa(st.settings.sheet_font_title, LV_OPA_COVER, 0);
        }
    } else {
        if (st.settings.sheet_font_multi_bar != nullptr && lv_obj_is_valid(st.settings.sheet_font_multi_bar)) {
            lv_obj_add_flag(st.settings.sheet_font_multi_bar, LV_OBJ_FLAG_HIDDEN);
        }
        if (st.settings.sheet_font_title != nullptr && lv_obj_is_valid(st.settings.sheet_font_title)) {
            lv_label_set_text(st.settings.sheet_font_title, Lang::Strings::BOOK_FONT_TITLE);
            lv_obj_set_style_text_opa(st.settings.sheet_font_title, LV_OPA_70, 0);
        }
    }
}

void ExitFontMultiMode(bool paint) {
    auto& st = Book_State();
    st.settings.font_multi = false;
    st.settings.font_suppress_click_until_us = 0;
    st.settings.font_suppress_click_idx = -1;
    st.settings.font_suppress_next_click = false;
    st.settings.font_selected.assign(st.settings.font_entries.size(), 0);
    if (paint) {
        RequestSettingsSheetPaint();
    } else {
        RefreshFontMultiFooter();
    }
}

void EnterFontMultiModeSelect(int idx) {
    auto& st = Book_State();
    st.settings.font_multi = true;
    st.settings.font_selected.assign(st.settings.font_entries.size(), 0);
    if (idx >= 0 && idx < static_cast<int>(st.settings.font_selected.size())) {
        st.settings.font_selected[static_cast<size_t>(idx)] = 1;
    }
    RequestSettingsSheetPaint();
}

bool DeleteReadFontFile(const std::string& file, std::string& err_out) {
    err_out.clear();
    if (file.empty() || file.size() > 63 || file.find('/') != std::string::npos ||
        file == "." || file == ".." || file.size() < 4 ||
        file.compare(file.size() - 3, 3, ".ef") != 0) {
        err_out = Lang::Strings::BOOK_FONT_NAME_INVALID;
        return false;
    }
    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", SD_PATH_FONTS, file.c_str());
    if (unlink(path) != 0 && errno != ENOENT) {
        err_out = Lang::Strings::BOOK_DELETE_FAILED;
        return false;
    }
    unlink((std::string(path) + ".tmp").c_str());
    ESP_LOGI(TAG, "deleted font %s", path);
    return true;
}

void OnFontMultiCancel(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    if (Book_State().reader.opening.load()) {
        return;
    }
    ExitFontMultiMode(true);
}

void OnFontMultiSelectAll(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = Book_State();
    if (!st.settings.font_multi || st.settings.font_entries.empty() || st.reader.opening.load()) {
        return;
    }
    SyncFontSelectedSize();
    const int n = static_cast<int>(st.settings.font_entries.size());
    const bool clear = (FontSelectedCount() >= n);
    st.settings.font_selected.assign(static_cast<size_t>(n), clear ? 0 : 1);
    RequestSettingsSheetPaint();
}

void OnFontMultiRemove(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = Book_State();
    if (!st.settings.font_multi || st.reader.opening.load()) {
        return;
    }
    SyncFontSelectedSize();
    if (FontSelectedCount() <= 0) {
        return;
    }

    const std::string cur = BookReaderPrefsFontFile();
    bool touch_cur = false;
    for (int i = 0; i < static_cast<int>(st.settings.font_entries.size()); ++i) {
        if (i < static_cast<int>(st.settings.font_selected.size()) &&
            st.settings.font_selected[static_cast<size_t>(i)] != 0 &&
            st.settings.font_entries[static_cast<size_t>(i)].file == cur) {
            touch_cur = true;
            break;
        }
    }

    // 先卸当前 epdfont 并立刻把 session/正文绑到安全字体，再 unlink。
    // 只 Release 不重绑：正文 style 仍握已释放的 lv_font → InstrFetchProhibited。
    if (touch_cur) {
        if (st.reader.chapter_pages_busy.load() || st.reader.layout_busy.load()) {
            st.reader.layout_token.fetch_add(1);
            st.reader.chapter_pages_token.fetch_add(1);
            st.reader.layout_again = true;
            st.reader.layout_reload_font = true;
            st.reader.chapter_pages_again = true;
            if (st.reader.session) {
                st.reader.session->InvalidateChapterPageTable();
            }
        }
        ReleaseBookFont();
        if (st.reader.session && st.reader.session->IsOpen()) {
            BindSessionLayoutPrefs(*st.reader.session);
            const bool keep_sheet = (st.reader.read_chrome == BookUiState::ReadChrome::kSettings);
            RenderReaderPage();
            if (keep_sheet) {
                EnsureSettingsSheetAfterLayout();
            }
        }
    }

    for (int i = static_cast<int>(st.settings.font_entries.size()) - 1; i >= 0; --i) {
        if (i >= static_cast<int>(st.settings.font_selected.size()) ||
            st.settings.font_selected[static_cast<size_t>(i)] == 0) {
            continue;
        }
        std::string err;
        if (!DeleteReadFontFile(st.settings.font_entries[static_cast<size_t>(i)].file, err)) {
            ESP_LOGW(TAG, "font batch delete fail %s: %s",
                     st.settings.font_entries[static_cast<size_t>(i)].file.c_str(), err.c_str());
            continue;
        }
        st.settings.font_entries.erase(st.settings.font_entries.begin() + i);
    }

    bool need_layout = false;
    auto still_has = [&](const char* file) {
        if (file == nullptr || file[0] == '\0') {
            return false;
        }
        for (const auto& fe : st.settings.font_entries) {
            if (fe.file == file) {
                return true;
            }
        }
        return false;
    };
    if (!still_has(BookReaderPrefsFontFile())) {
        // 还有剩余则切到第一项；全删则保持偏好名，EnsureBookFont 失败时 BookFont→fontpack
        if (!st.settings.font_entries.empty()) {
            BookReaderPrefsSetFontFile(st.settings.font_entries.front().file.c_str());
        }
        need_layout = true;
    } else if (touch_cur) {
        need_layout = true;
    }

    st.settings.font_multi = false;
    st.settings.font_suppress_click_until_us = 0;
    st.settings.font_suppress_click_idx = -1;
    st.settings.font_suppress_next_click = false;
    st.settings.font_selected.assign(st.settings.font_entries.size(), 0);
    ClampFontListPage();
    EnsureFontListPageShowsSelection();
    // 空列表需重建空态；否则原地刷行
    if (st.settings.font_entries.empty() || !SettingsSheetWidgetsReady()) {
        RebuildSettingsSheet();
    } else {
        RequestSettingsSheetPaint();
    }
    if (need_layout) {
        ScheduleLayoutApply(true);
    }
}

void OnSheetFontLongPressed(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    if (Book_State().reader.opening.load()) {
        return;
    }
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    auto& st = Book_State();
    if (idx < 0 || idx >= static_cast<int>(st.settings.font_entries.size())) {
        return;
    }
    if (!st.settings.font_multi) {
        st.settings.font_suppress_click_idx = idx;
        st.settings.font_suppress_click_until_us = esp_timer_get_time() + kFontSuppressRowClickUs;
        st.settings.font_suppress_next_click = true;
        EnterFontMultiModeSelect(idx);
        return;
    }
    ToggleFontItemSelected(idx);
    RequestSettingsSheetPaint();
}

void OnSheetFontSelect(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    if (Book_State().reader.opening.load()) {
        return;
    }
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    auto& st = Book_State();
    if (idx < 0 || idx >= static_cast<int>(st.settings.font_entries.size())) {
        return;
    }
    // 长按进多选后松手常冒 CLICKED；设置卡刷屏可能超过短时间窗，故优先吃掉下一次同项点击
    if (st.settings.font_suppress_next_click && idx == st.settings.font_suppress_click_idx) {
        st.settings.font_suppress_next_click = false;
        return;
    }
    if (idx == st.settings.font_suppress_click_idx &&
        esp_timer_get_time() < st.settings.font_suppress_click_until_us) {
        return;
    }
    if (st.settings.font_multi) {
        ToggleFontItemSelected(idx);
        RequestSettingsSheetPaint();
        return;
    }
    const char* file = st.settings.font_entries[static_cast<size_t>(idx)].file.c_str();
    if (std::strcmp(BookReaderPrefsFontFile(), file) == 0) {
        return;
    }
    BookReaderPrefsSetFontFile(file);
    EnsureFontListPageShowsSelection();
    RequestSettingsSheetPaint();
    ScheduleLayoutApply(true);
}

