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
#include "sd_paths.h"
#include "sd_access.h"
#include "epd_refresh.h"
#include "lv_adapter_display.h"

bool ParseEfSizePx(const std::string& name, int* size_px) {
    if (size_px == nullptr || name.size() < 8) {
        return false;
    }
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || strcasecmp(name.c_str() + dot, ".ef") != 0) {
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

namespace {
struct FontScanResult {
    uint32_t generation = 0; // 本轮扫描代次
    std::vector<ReadFontEntry> entries; // 仅由扫描任务构建的目录快照
    bool ok = false; // 是否完整读取目录
};
std::atomic<uint32_t> s_font_scan_generation{0}; // 取消时递增，任务逐项检查
std::atomic<bool> s_font_scan_busy{false}; // 同时只运行一个扫描任务
std::mutex s_font_scan_mu; // 只保护完成结果的交接
std::unique_ptr<FontScanResult> s_font_scan_result; // UI 尚未接收的完整结果
lv_timer_t* s_font_scan_timer = nullptr; // UI 轮询，取消时删除

bool CollectReadFonts(FontScanResult& result) {
    DIR* dir = opendir(SD_PATH_FONTS);
    if (dir == nullptr) return false;
    std::unique_ptr<DIR, decltype(&closedir)> directory(dir, &closedir);
    size_t visited = 0;
    while (true) {
        if (result.generation != s_font_scan_generation.load() || sd_access::IsQuiescing()) return false;
        errno = 0;
        dirent* ent = readdir(dir);
        if (!ent) {
            if (errno != 0) return false;
            break;
        }
        if ((++visited & 15u) == 0) vTaskDelay(1);
        if (ent->d_name[0] == '.') {
            continue;
        }
        const size_t n = std::strlen(ent->d_name);
        if (n < 4 || !SdIsValidFileName(ent->d_name) || strcasecmp(ent->d_name + (n - 3), ".ef") != 0) {
            continue;
        }
        char name[SD_FILE_NAME_CAPACITY];
        std::memcpy(name, ent->d_name, n + 1);
        char full[SD_FILE_PATH_CAPACITY];
        std::snprintf(full, sizeof(full), "%s/%s", SD_PATH_FONTS, name);
        struct stat st_info {};
        if (stat(full, &st_info) != 0) return false;
        if (!S_ISREG(st_info.st_mode)) continue;
        ReadFontEntry fe;
        fe.file = name;
        if (!ParseEfSizePx(fe.file, &fe.size_px)) {
            fe.size_px = 0;
        }
        result.entries.push_back(std::move(fe));
    }
    std::sort(result.entries.begin(), result.entries.end(), ReadFontEntryLess);
    return true;
}

void ReadFontScanWorker(void* arg) {
    {
        std::unique_ptr<FontScanResult> result(static_cast<FontScanResult*>(arg));
        sd_access::Transaction transaction;
        try {
            if (transaction) result->ok = CollectReadFonts(*result);
        } catch (const std::bad_alloc&) {
            result->ok = false;
        }
        std::lock_guard<std::mutex> lock(s_font_scan_mu);
        if (result->generation == s_font_scan_generation.load()) s_font_scan_result = std::move(result);
    }
    s_font_scan_busy.store(false);
    vTaskDeleteWithCaps(nullptr);
}

void ReadFontScanTick(lv_timer_t*) {
    auto& st = Book_State();
    if (st.reader.standby_paused || !st.reader.read_scr ||
        st.reader.read_chrome != BookUiState::ReadChrome::kSettings || st.settings.reader_tab != 1 ||
        !st.settings.settings_sheet || !lv_obj_is_valid(st.settings.settings_sheet)) {
        CancelReadFontScan();
        return;
    }
    // 先完成加载提示的刷屏，交互或刷屏期间不替换容器。
    if (TouchUiFingerIsDown() || TouchUiHasPendingEdges() || EpdGetStats().busy ||
        st.settings.reader_sheet_scrolling) return;
    if (st.ttf.ttf_mode != BookUiState::TtfMode::kNone) return;
    std::unique_ptr<FontScanResult> result;
    {
        std::lock_guard<std::mutex> lock(s_font_scan_mu);
        result = std::move(s_font_scan_result);
    }
    if (result) {
        if (result->generation != s_font_scan_generation.load()) return;
        st.settings.font_scan_loading = false;
        st.settings.font_scan_failed = !result->ok;
        if (result->ok) st.settings.font_entries.swap(result->entries);
        st.settings.font_selected.clear();
        ClampFontListPage();
        lv_timer_del(s_font_scan_timer);
        s_font_scan_timer = nullptr;
        RequestSettingsSheetPaint();
        return;
    }
    if (s_font_scan_busy.load()) return;
    auto* work = new(std::nothrow) FontScanResult;
    if (work) {
        work->generation = s_font_scan_generation.load();
        s_font_scan_busy.store(true);
        if (xTaskCreatePinnedToCoreWithCaps(ReadFontScanWorker, "book_font_scan", 4096, work,
                tskIDLE_PRIORITY + 1, nullptr, 0, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == pdPASS) return;
        s_font_scan_busy.store(false);
        delete work;
    }
    st.settings.font_scan_loading = false;
    st.settings.font_scan_failed = true;
    lv_timer_del(s_font_scan_timer);
    s_font_scan_timer = nullptr;
    RequestSettingsSheetPaint();
}
} // namespace

void CancelReadFontScan() {
    {
        std::lock_guard<std::mutex> lock(s_font_scan_mu);
        ++s_font_scan_generation;
        s_font_scan_result.reset();
    }
    if (s_font_scan_timer) lv_timer_del(s_font_scan_timer);
    s_font_scan_timer = nullptr;
    Book_State().settings.font_scan_loading = false;
    Book_State().settings.font_scan_failed = false;
}

void StartReadFontScan() {
    CancelReadFontScan();
    auto& settings = Book_State().settings;
    settings.font_entries.clear();
    ExitFontMultiMode(false);
    settings.font_scan_loading = true;
    s_font_scan_timer = lv_timer_create(ReadFontScanTick, 100, nullptr);
    if (!s_font_scan_timer) {
        settings.font_scan_loading = false;
        settings.font_scan_failed = true;
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
    return st.settings.font_entries.empty() || BookReaderPrefsFontSize() > 0 ? -1 : 0;
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

// 列表仅去掉 .ef，显示省略由控件按可用宽度处理。
void FormatFontListName(const std::string& file, char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    std::string stem = file;
    if (stem.size() > 3 && strcasecmp(stem.c_str() + stem.size() - 3, ".ef") == 0) {
        stem.resize(stem.size() - 3);
    }
    if (stem.empty()) {
        stem = file;
    }
    const size_t bytes = SdUtf8PrefixLength(stem.c_str(), out_len - 1);
    std::memcpy(out, stem.data(), bytes);
    out[bytes] = '\0';
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
    if (!SdIsValidFileName(file.c_str()) || file.size() < 4 ||
        strcasecmp(file.c_str() + file.size() - 3, ".ef") != 0) {
        err_out = Lang::Strings::BOOK_FONT_NAME_INVALID;
        return false;
    }
    char path[SD_FILE_PATH_CAPACITY];
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
    if (!st.settings.font_multi || ReaderWorkersBusy()) {
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
    // 漫画中只保存字体选择，实际加载的字库可能与偏好不同，删除前统一解绑。
    if (touch_cur || ReaderPageImagesHint()) {
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
    if (BookReaderPrefsFontSize() == 0 && !still_has(BookReaderPrefsFontFile())) {
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

