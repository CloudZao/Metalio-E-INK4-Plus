#pragma GCC optimize("O1")

#include "book_screen/settings/book_font_multi.h"
#include "book_screen/settings/book_ttf_panel.h"
#include "display/font/epdfont.h"
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
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <esp_heap_caps.h>

#include "assets/lang_config.h"
#include "power_policy.h"
#include "sd_paths.h"
#include "sd_access.h"
#include "sd_directory_page.h"
#include "epd_refresh.h"
#include "lv_adapter_display.h"

bool ReadFontEntryLess(const ReadFontEntry& a, const ReadFontEntry& b) {
    if (a.size_px != b.size_px) {
        return a.size_px < b.size_px;
    }
    return a.file < b.file;
}

static SdDirectoryPage s_font_directory;
static lv_timer_t* s_font_scan_timer = nullptr;

static bool FontDirectoryFilter(const char* path, bool directory) {
    const bool source = std::strncmp(path, SD_PATH_FONT_SRC "/", std::strlen(SD_PATH_FONT_SRC) + 1) == 0;
    const char* relative = path + std::strlen(source ? SD_PATH_FONT_SRC : SD_PATH_FONTS) + 1;
    if (directory || !SdIsValidFileName(relative)) {
        return false;
    }
    const char* extension = std::strrchr(relative, '.');
    return source ? IsTtfFileName(relative) : extension && !strcasecmp(extension, ".ef");
}

static void FontDirectoryMetadata(SdDirectoryPage::Entry& entry) {
    const char* name = std::strrchr(entry.path.c_str(), '/');
    if (IsTtfFileName(name ? name + 1 : entry.path.c_str())) {
        return;
    }
    FILE* fp = std::fopen(entry.path.c_str(), "rb");
    epdfont_header_t header{};
    const bool valid = fp && std::fread(&header, 1, sizeof(header), fp) == sizeof(header) &&
                       !std::memcmp(header.magic, EPDFONT_MAGIC, 8) && header.version == EPDFONT_VERSION;
    if (fp) {
        std::fclose(fp);
    }
    entry.metric = valid ? header.line_height : 0;
    if (!valid) {
        ESP_LOGW(TAG, "font page header unavailable");
    }
}

void CancelReadFontScan() {
    s_font_directory.Cancel();
    if (s_font_scan_timer) {
        lv_timer_del(s_font_scan_timer);
    }
    s_font_scan_timer = nullptr;
    Book_State().settings.font_scan_loading = false;
    Book_State().settings.font_scan_failed = false;
}

static void ReadFontScanTick(lv_timer_t*) {
    auto& st = Book_State();
    if (st.reader.standby_paused || !st.reader.read_scr ||
        st.reader.read_chrome != BookUiState::ReadChrome::kSettings || st.settings.reader_tab != 1 ||
        !st.settings.settings_sheet || sd_access::IsQuiescing()) {
        CancelReadFontScan();
        return;
    }
    if (st.ttf.ttf_mode != BookUiState::TtfMode::kNone) {
        return;
    }
    SdDirectoryPage::Result result;
    const bool received = s_font_directory.Poll(result);
    if (!received && st.settings.font_scan_loading && !st.settings.font_loading_shown &&
        esp_timer_get_time() - st.settings.font_requested_us >= 180000) {
        st.settings.font_loading_shown = true;
        RequestSettingsSheetPaint();
    }
    if (received) {
        if (result.page != st.settings.font_list_page) {
            return;
        }
        if (result.ok && result.entries.empty() && result.page > 0) {
            --st.settings.font_list_page;
            StartReadFontScan();
            return;
        }
        st.settings.font_entries.clear();
        for (const auto& item : result.entries) {
            ReadFontEntry entry;
            const char* root = item.path.compare(0, std::strlen(SD_PATH_FONT_SRC) + 1, SD_PATH_FONT_SRC "/") == 0
                                   ? SD_PATH_FONT_SRC
                                   : SD_PATH_FONTS;
            entry.file = item.path.substr(std::strlen(root) + 1);
            if (!SdIsValidFileName(entry.file.c_str())) {
                continue;
            }
            entry.size_px = item.metric;
            st.settings.font_entries.push_back(std::move(entry));
        }
        st.settings.font_scan_loading = false;
        st.settings.font_scan_failed = !result.ok;
        RequestSettingsSheetPaint();
        if (!result.ok) {
            lv_timer_del(s_font_scan_timer);
            s_font_scan_timer = nullptr;
            return;
        }
    }
    if (s_font_directory.Counted() && !st.settings.font_scan_loading) {
        RequestSettingsSheetPaint();
        lv_timer_del(s_font_scan_timer);
        s_font_scan_timer = nullptr;
    }
}

void StartReadFontScan() {
    auto& settings = Book_State().settings;
    settings.font_requested_us = esp_timer_get_time();
    settings.font_loading_shown = false;
    settings.font_scan_loading = true;
    settings.font_scan_failed = false;
    s_font_directory.Request({SD_PATH_FONT_SRC, SD_PATH_FONTS}, FontDirectoryFilter, settings.font_list_page,
                             kReadFontPageSize, false, FontDirectoryMetadata);
    if (!s_font_scan_timer) {
        s_font_scan_timer = lv_timer_create(ReadFontScanTick, 100, nullptr);
    }
    if (s_font_directory.Cached(settings.font_list_page)) {
        ReadFontScanTick(nullptr);
    }
    if (!s_font_scan_timer && settings.font_scan_loading) {
        settings.font_scan_loading = false;
        settings.font_scan_failed = true;
        ESP_LOGE(TAG, "font page timer creation failed");
    }
}

int CurrentFontIndex() {
    auto& st = Book_State();
    const char* cur = BookReaderPrefsFontFile();
    for (int i = 0; i < static_cast<int>(st.settings.font_entries.size()); ++i) {
        if (st.settings.font_entries[static_cast<size_t>(i)].file == cur) {
            return i;
        }
    }
    return -1;
}

int FontListPageCount() {
    return s_font_directory.Pages();
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

void FormatFontListName(const std::string& file, char* out, size_t out_len) {
    if (!out || !out_len) {
        return;
    }
    std::string stem = file;
    const size_t dot = stem.rfind('.');
    if (dot != std::string::npos) {
        stem.resize(dot);
    }
    if (stem.empty()) {
        stem = file;
    }
    const size_t bytes = SdUtf8PrefixLength(stem.c_str(), out_len - 1);
    std::memcpy(out, stem.data(), bytes);
    out[bytes] = '\0';
}

int FontSelectedCount() {
    return static_cast<int>(Book_State().settings.font_selected.size());
}

bool FontItemSelected(int idx) {
    const auto& settings = Book_State().settings;
    if (idx < 0 || idx >= static_cast<int>(settings.font_entries.size())) {
        return false;
    }
    const auto& file = settings.font_entries[idx].file;
    return std::find(settings.font_selected.begin(), settings.font_selected.end(), file) !=
           settings.font_selected.end();
}

void ToggleFontItemSelected(int idx) try {
    auto& settings = Book_State().settings;
    if (settings.font_scan_loading || idx < 0 || idx >= static_cast<int>(settings.font_entries.size())) {
        return;
    }
    const auto& file = settings.font_entries[idx].file;
    const auto found = std::find(settings.font_selected.begin(), settings.font_selected.end(), file);
    if (found == settings.font_selected.end()) {
        settings.font_selected.push_back(file);
    } else {
        settings.font_selected.erase(found);
    }
} catch (const std::bad_alloc&) {
    ESP_LOGW(TAG, "font selection allocation failed; selection unchanged");
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
    st.settings.font_selected.clear();
    if (paint) {
        RequestSettingsSheetPaint();
    } else {
        RefreshFontMultiFooter();
    }
}

void EnterFontMultiModeSelect(int idx) {
    auto& st = Book_State();
    st.settings.font_multi = true;
    st.settings.font_selected.clear();
    ToggleFontItemSelected(idx);
    RequestSettingsSheetPaint();
}

bool DeleteReadFontFile(const std::string& file, std::string& err_out) {
    err_out.clear();
    if (!SdIsValidFileName(file.c_str()) || file.size() < 4 ||
        (!IsTtfFileName(file.c_str()) && strcasecmp(file.c_str() + file.size() - 3, ".ef") != 0)) {
        err_out = Lang::Strings::BOOK_FONT_NAME_INVALID;
        return false;
    }
    sd_access::Transaction transaction;
    if (!transaction) {
        err_out = Lang::Strings::BOOK_DELETE_FAILED;
        return false;
    }
    char path[SD_FILE_PATH_CAPACITY];
    std::snprintf(path, sizeof(path), "%s/%s", IsTtfFileName(file.c_str()) ? SD_PATH_FONT_SRC : SD_PATH_FONTS,
                  file.c_str());
    if (unlink(path) != 0 && errno != ENOENT) {
        err_out = Lang::Strings::BOOK_DELETE_FAILED;
        ESP_LOGE(TAG, "font unlink failed errno=%d", errno);
        return false;
    }
    if (unlink((std::string(path) + ".tmp").c_str()) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "font temporary file cleanup failed errno=%d", errno);
    }
    ESP_LOGI(TAG, "font deleted kind=%s", IsTtfFileName(file.c_str()) ? "vector" : "bitmap");
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

    if (st.settings.font_scan_loading) {
        return;
    }
    const int n = static_cast<int>(st.settings.font_entries.size());
    bool clear = n > 0;
    for (int i = 0; i < n; ++i) {
        clear = clear && FontItemSelected(i);
    }
    for (int i = 0; i < n; ++i) {
        if (FontItemSelected(i) == clear) {
            ToggleFontItemSelected(i);
        }
    }
    RequestSettingsSheetPaint();
}

void OnFontMultiRemove(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = Book_State();
    if (!st.settings.font_multi || ReaderWorkersBusy()) {
        return;
    }

    if (FontSelectedCount() <= 0) {
        return;
    }

    const std::string cur = BookReaderPrefsFontFile();
    if (st.settings.font_scan_loading) {
        return;
    }
    const bool touch_cur = std::find(st.settings.font_selected.begin(), st.settings.font_selected.end(), cur) !=
                           st.settings.font_selected.end();
    PowerNeedHold awake(PowerNeed::StandbyInhibit);
    std::vector<std::string> selected;
    try {
        selected = st.settings.font_selected;
    } catch (const std::bad_alloc&) {
        ESP_LOGW(TAG, "font delete snapshot allocation failed; selection retained");
        return;
    }

    // 先卸当前 epdfont 并立刻把 session/正文绑到安全字体，再 unlink。
    // 只 Release 不重绑：正文 style 仍握已释放的 lv_font → InstrFetchProhibited。
    // 漫画中只保存字体选择，实际加载的字库可能与偏好不同，删除前统一解绑。
    if (touch_cur || ReaderPageImagesHint()) {
        const auto fallback = std::find_if(
            st.settings.font_entries.begin(), st.settings.font_entries.end(), [&](const ReadFontEntry& entry) {
                return !FontItemSelected(static_cast<int>(&entry - st.settings.font_entries.data()));
            });
        const char* file =
            fallback == st.settings.font_entries.end() ? kBookReaderDefaultFontFile : fallback->file.c_str();
        BookReaderPrefsSetFontFile(file, IsTtfFileName(file) ? 40 : 0);
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

    for (const auto& file : selected) {
        std::string err;
        if (!DeleteReadFontFile(file, err)) {
            ScreenShowToast(Lang::Strings::BOOK_DELETE_FAILED);
        }
    }

    const bool need_layout = touch_cur;

    st.settings.font_multi = false;
    st.settings.font_suppress_click_until_us = 0;
    st.settings.font_suppress_click_idx = -1;
    st.settings.font_suppress_next_click = false;
    st.settings.font_selected.clear();
    s_font_directory.Cancel();
    st.settings.font_list_page = 0;
    StartReadFontScan();
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
    if (Book_State().settings.font_scan_loading) {
        return;
    }
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
    if (Book_State().settings.font_scan_loading) {
        return;
    }
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
    if (idx == st.settings.font_suppress_click_idx && esp_timer_get_time() < st.settings.font_suppress_click_until_us) {
        return;
    }
    if (st.settings.font_multi) {
        ToggleFontItemSelected(idx);
        RequestSettingsSheetPaint();
        return;
    }
    const auto& selected = st.settings.font_entries[static_cast<size_t>(idx)];
    const char* file = selected.file.c_str();
    if (!PrepareBookFont(file, IsTtfFileName(file)
                                   ? std::max(20, BookReaderPrefsFontSize() > 0 ? BookReaderPrefsFontSize() : 40)
                                   : 0)) {
        return;
    }
    RequestSettingsSheetPaint();
    ScheduleLayoutApply(true);
}
