#include "task_screen.h"
#include "task_screen_priv.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include "api_endpoints.h"
#include "api_http.h"
#include "board.h"
#include "checklist_cache.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "power_policy.h"
#include "screen_common.h"
#include "vk_key_handler.h"
#include "vk_page_repeat.h"
#include "assets/lang_config.h"

namespace {

constexpr int kTabCount = 2;
constexpr int kHttpTimeoutMs = 15000;
constexpr int kMaxItems = 64;
constexpr int kWorkerStack = 10 * 1024;
constexpr int kBatchWorkerStack = 16 * 1024; // 批量多次 HTTP，栈放 SPIRAM
constexpr int64_t kSyncThrottleUs = 3000000; // 手动同步最短间隔 3s
/** 清单界面：fontpack UI 默认字（与书库列表 item 一致） */
const lv_font_t* TaskFont() {
    return fontpack_lv_font_ui();
}

/** 清单标题：fontpack 30@2（与书库标题一致） */
const lv_font_t* TaskTitleFont() {
    const lv_font_t* f = fontpack_lv_font_get(30, 2);
    return f != nullptr ? f : TaskFont();
}

lv_coord_t TaskFontLineH() {
    const lv_font_t* font = TaskFont();
    return (font != nullptr && font->line_height > 0) ? font->line_height : 30;
}

/** 标题 + 计划时间两行时的行高估算，用于翻页每页条数 */
lv_coord_t EstimatedTwoLineRowH() {
    const lv_coord_t line_h = TaskFontLineH();
    const lv_coord_t text_h = line_h * 2 + kRowLineGap;
    return text_h + kRowPad * 2 + kBorderW * 2;
}

struct TaskItem {
    char id[48] = {};
    char title[96] = {};
    char plan_date[16] = {};
    char plan_time[16] = {};
    char status[16] = {};
};

struct UiState {
    lv_obj_t* screen = nullptr;
    lv_obj_t* tab_btns[kTabCount] = {};
    lv_obj_t* tab_lbls[kTabCount] = {};
    lv_obj_t* section_title = nullptr;
    lv_obj_t* status_lbl = nullptr;  // 与标题同行，稍后显示完成中/已删除等
    lv_obj_t* list_host = nullptr;
    lv_obj_t* list_empty = nullptr;
    lv_obj_t* list_frame = nullptr;          // 列表+操作外框（页码在框外）
    lv_obj_t* footer = nullptr;             // 框内同步/批量操作区
    lv_obj_t* page_lbl = nullptr;
    lv_obj_t* sync_btn = nullptr;
    lv_obj_t* multi_bar = nullptr;           // 多选底栏（框内）
    lv_obj_t* multi_complete_sep = nullptr;  // 「完成」前圆点（已完成 Tab 一并隐藏）
    lv_obj_t* multi_complete_btn = nullptr;  // 批量完成（已完成 Tab 隐藏）
    lv_obj_t* dialog = nullptr;
    lv_obj_t* dialog_msg = nullptr;
    lv_obj_t* dialog_complete_btn = nullptr;
};

struct FetchResultMsg {
    bool ok = false;
    char err[80] = {};
    std::vector<TaskItem> items;
};

struct DeleteResultMsg {
    bool ok = false;
    char err[80] = {};
    char id[48] = {};
};

struct CompleteResultMsg {
    bool ok = false;
    char err[80] = {};
    char id[48] = {};
};

enum class BatchOp : uint8_t { kDelete = 0, kComplete };

struct BatchResultMsg {
    BatchOp op = BatchOp::kDelete;
    int ok_n = 0;
    int fail_n = 0;
    char err[80] = {};
    char ok_ids[kMaxItems][48] = {};
};

UiState s_ui;
bool s_screen_alive = false;
std::vector<TaskItem> s_items;
std::vector<uint8_t> s_selected; // 与 s_items 等长；1=多选选中
bool s_multi = false;
int64_t s_suppress_row_click_until_us = 0; // 仅吞长按进多选同一行的松手假 CLICKED
int s_suppress_row_click_idx = -1;
constexpr int64_t kSuppressRowClickUs = 400000; // 400ms：只挡该行松手，不挡其它行连点
int s_tab = 0;  // 0=待办 1=已完成
int s_list_page = 0;
int s_list_per_page = 5;
int s_pending_action_idx = -1;
TaskHandle_t s_fetch_task = nullptr;
TaskHandle_t s_delete_task = nullptr;
TaskHandle_t s_complete_task = nullptr;
TaskHandle_t s_batch_task = nullptr;
std::atomic<bool> s_busy{false};
std::atomic<bool> s_dialog_open{false};
std::atomic<bool> s_fetch_net_held{false}; // Fetch 硬占网至结果上屏后再 Release（启动保网计时）
char s_status_text[80] = {};       // 非空时显示在标题同行
char s_empty_override[80] = {};    // 空列表时覆盖「暂无待办」等，避免与 status 叠字
lv_timer_t* s_status_clear_timer = nullptr;
constexpr uint32_t kStatusClearMs = 2000;
int64_t s_last_user_sync_us = 0;       // 上次手动同步发起时刻；0=尚未点过

void DisableScroll(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

bool ItemIsDone(const TaskItem& item) {
    return std::strcmp(item.status, "done") == 0;
}

bool ItemInCurrentTab(const TaskItem& item) {
    return s_tab == 1 ? ItemIsDone(item) : !ItemIsDone(item);
}

bool ItemHasPlan(const TaskItem& item) {
    return item.plan_date[0] != '\0' || item.plan_time[0] != '\0';
}

/** 计划时间新→旧（高到底）；无计划时间的排后面 */
void SortItemsByPlanDesc(std::vector<TaskItem>& items) {
    std::sort(items.begin(), items.end(), [](const TaskItem& a, const TaskItem& b) {
        const bool ha = ItemHasPlan(a);
        const bool hb = ItemHasPlan(b);
        if (ha != hb) {
            return ha;
        }
        const int d = std::strcmp(a.plan_date, b.plan_date);
        if (d != 0) {
            return d > 0;
        }
        return std::strcmp(a.plan_time, b.plan_time) > 0;
    });
}

int FilteredCount() {
    int n = 0;
    for (const auto& item : s_items) {
        if (ItemInCurrentTab(item)) {
            ++n;
        }
    }
    return n;
}

void FormatPlanMeta(char* out, size_t out_sz, const TaskItem& item) {
    if (out == nullptr || out_sz == 0) {
        return;
    }
    char time_buf[8] = {};
    if (item.plan_time[0] != '\0') {
        std::snprintf(time_buf, sizeof(time_buf), "%.5s", item.plan_time);
    }
    if (item.plan_date[0] != '\0' && time_buf[0] != '\0') {
        std::snprintf(out, out_sz, "%s %s", item.plan_date, time_buf);
    } else if (item.plan_date[0] != '\0') {
        std::snprintf(out, out_sz, "%s", item.plan_date);
    } else if (time_buf[0] != '\0') {
        std::snprintf(out, out_sz, "%s", time_buf);
    } else {
        out[0] = '\0';
    }
}

void RefreshPageLbl();
void RefreshStatusLbl();
void RebuildListPage();
lv_obj_t* FindListRow(int index);
lv_obj_t* FindRowChildBySize(lv_obj_t* row, lv_coord_t w, lv_coord_t h);
void PatchRowCheckMark(int index);
void PatchVisibleRowCheckMarks();
void RequestRebuildListPage();
void RequestCheckMarksPaint();
void RefreshFooterMode();
void ExitMultiMode(bool rebuild);
void ExitMultiModeEx(bool rebuild, bool clear_status);
void SyncSelectedSize();
int SelectedCount();
bool ItemSelected(int idx);
void ToggleItemSelected(int idx);

void CancelStatusClearTimer() {
    if (s_status_clear_timer == nullptr) {
        return;
    }
    lv_timer_delete(s_status_clear_timer);
    s_status_clear_timer = nullptr;
}

void StatusClearTimerCb(lv_timer_t* /*t*/) {
    s_status_clear_timer = nullptr;
    if (!s_screen_alive) {
        return;
    }
    s_status_text[0] = '\0';
    RefreshStatusLbl();
}

void RefreshStatusLbl() {
    if (s_ui.status_lbl == nullptr) {
        return;
    }
    if (s_status_text[0] == '\0') {
        lv_label_set_text(s_ui.status_lbl, "");
        lv_obj_add_flag(s_ui.status_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(s_ui.status_lbl, s_status_text);
    lv_obj_clear_flag(s_ui.status_lbl, LV_OBJ_FLAG_HIDDEN);
}

/** @param auto_clear 成功/错误提示定时收起；忙碌态勿开 */
void SetStatusText(const char* text, bool auto_clear = false) {
    CancelStatusClearTimer();
    if (!s_screen_alive || s_ui.status_lbl == nullptr) {
        return;
    }
    const char* t = text != nullptr ? text : "";
    strlcpy(s_status_text, t, sizeof(s_status_text));
    RefreshStatusLbl();
    if (auto_clear && s_status_text[0] != '\0') {
        s_status_clear_timer = lv_timer_create(StatusClearTimerCb, kStatusClearMs, nullptr);
        if (s_status_clear_timer != nullptr) {
            lv_timer_set_repeat_count(s_status_clear_timer, 1);
        }
    }
}

void SetEmptyOverride(const char* msg) {
    if (msg != nullptr && msg[0] != '\0') {
        strlcpy(s_empty_override, msg, sizeof(s_empty_override));
    } else {
        s_empty_override[0] = '\0';
    }
}

void HideActionDialog() {
    s_pending_action_idx = -1;
    s_dialog_open.store(false, std::memory_order_release);
    if (s_ui.dialog != nullptr) {
        lv_obj_add_flag(s_ui.dialog, LV_OBJ_FLAG_HIDDEN);
    }
}

bool AnyWorkerBusy() {
    return s_fetch_task != nullptr || s_delete_task != nullptr ||
           s_complete_task != nullptr || s_batch_task != nullptr || s_busy.load();
}

void AcquireFetchNet() {
    if (!s_fetch_net_held.exchange(true)) {
        PowerPolicy::GetInstance().Acquire(PowerNeed::OtaDownload);
    }
}

void ReleaseFetchNet() {
    if (s_fetch_net_held.exchange(false)) {
        PowerPolicy::GetInstance().Release(PowerNeed::OtaDownload);
    }
}

void PublishChecklistCache() {
    checklist_cache_begin();
    for (const auto& item : s_items) {
        checklist_cache_append(item.id, item.title, item.status, item.plan_date, item.plan_time);
    }
    checklist_cache_end();
}

void ApplyFromCache() {
    s_items.clear();
    s_list_page = 0;
    s_multi = false;
    s_suppress_row_click_until_us = 0;
    s_suppress_row_click_idx = -1;
    s_selected.clear();
    if (!checklist_cache_ready()) {
        SetStatusText("");
        SetEmptyOverride(nullptr);
        RebuildListPage();
        return;
    }
    // 64×item ≈ 12KB：勿放 LVGL 栈（470 tick 12KB / 397 为 16KB，栈上必炸掉）
    auto* buf = static_cast<checklist_cache_item_t*>(heap_caps_malloc(
        sizeof(checklist_cache_item_t) * CHECKLIST_CACHE_MAX_ITEMS,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        buf = static_cast<checklist_cache_item_t*>(heap_caps_malloc(
            sizeof(checklist_cache_item_t) * CHECKLIST_CACHE_MAX_ITEMS, MALLOC_CAP_8BIT));
    }
    if (buf == nullptr) {
        ESP_LOGW(TAG, "ApplyFromCache alloc failed");
        SetStatusText("");
        SetEmptyOverride(nullptr);
        RebuildListPage();
        return;
    }
    const size_t n = checklist_cache_copy_all(buf, CHECKLIST_CACHE_MAX_ITEMS);
    s_items.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        TaskItem item;
        strlcpy(item.id, buf[i].id, sizeof(item.id));
        strlcpy(item.title, buf[i].title, sizeof(item.title));
        strlcpy(item.status, buf[i].status, sizeof(item.status));
        strlcpy(item.plan_date, buf[i].plan_date, sizeof(item.plan_date));
        strlcpy(item.plan_time, buf[i].plan_time, sizeof(item.plan_time));
        s_items.push_back(item);
    }
    heap_caps_free(buf);
    SortItemsByPlanDesc(s_items);
    SetEmptyOverride(nullptr);
    SetStatusText("");
    RebuildListPage();
}

int ListPageCount() {
    const int n = FilteredCount();
    if (n <= 0 || s_list_per_page <= 0) {
        return 1;
    }
    return (n + s_list_per_page - 1) / s_list_per_page;
}

void RefreshPageLbl() {
    if (s_ui.page_lbl == nullptr) {
        return;
    }
    const int filtered = FilteredCount();
    if (filtered <= 0) {
        lv_label_set_text(s_ui.page_lbl, "0 / 0");
        return;
    }
    char page_buf[32];
    std::snprintf(page_buf, sizeof(page_buf), "%d / %d", s_list_page + 1, ListPageCount());
    lv_label_set_text(s_ui.page_lbl, page_buf);
}

void OnRowClicked(lv_event_t* e);
void OnDialogBackdropClicked(lv_event_t* e);
void OnDialogDelete(lv_event_t* e);
void OnDialogComplete(lv_event_t* e);
void OnTabClicked(lv_event_t* e);
void OnRowClicked(lv_event_t* e);
void OnRowLongPressed(lv_event_t* e);
void OnMultiCancel(lv_event_t* e);
void OnMultiSelectAll(lv_event_t* e);
void OnMultiDelete(lv_event_t* e);
void OnMultiComplete(lv_event_t* e);
void EnsureActionDialog();
void ScheduleDelete(int idx);
void ScheduleComplete(int idx);
void ScheduleBatch(BatchOp op);
void RefreshTabUi();

void SyncSelectedSize() {
    if (s_selected.size() != s_items.size()) {
        s_selected.assign(s_items.size(), 0);
    }
}

int SelectedCount() {
    SyncSelectedSize();
    int n = 0;
    for (uint8_t v : s_selected) {
        if (v != 0) {
            ++n;
        }
    }
    return n;
}

bool ItemSelected(int idx) {
    SyncSelectedSize();
    return idx >= 0 && idx < static_cast<int>(s_selected.size()) && s_selected[static_cast<size_t>(idx)] != 0;
}

void ToggleItemSelected(int idx) {
    SyncSelectedSize();
    if (idx < 0 || idx >= static_cast<int>(s_selected.size())) {
        return;
    }
    s_selected[static_cast<size_t>(idx)] = s_selected[static_cast<size_t>(idx)] ? 0 : 1;
}

void RefreshFooterMode() {
    if (s_ui.footer == nullptr) {
        return;
    }
    if (s_ui.page_lbl != nullptr) {
        lv_obj_clear_flag(s_ui.page_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_multi) {
        if (s_ui.sync_btn != nullptr) {
            lv_obj_add_flag(s_ui.sync_btn, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_ui.multi_bar != nullptr) {
            lv_obj_clear_flag(s_ui.multi_bar, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_ui.multi_complete_btn != nullptr) {
            if (s_tab == 1) {
                lv_obj_add_flag(s_ui.multi_complete_btn, LV_OBJ_FLAG_HIDDEN);
                if (s_ui.multi_complete_sep != nullptr) {
                    lv_obj_add_flag(s_ui.multi_complete_sep, LV_OBJ_FLAG_HIDDEN);
                }
            } else {
                lv_obj_clear_flag(s_ui.multi_complete_btn, LV_OBJ_FLAG_HIDDEN);
                if (s_ui.multi_complete_sep != nullptr) {
                    lv_obj_clear_flag(s_ui.multi_complete_sep, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), Lang::Strings::TASK_SELECTED_FMT, SelectedCount());
        SetStatusText(buf);
    } else {
        if (s_ui.multi_bar != nullptr) {
            lv_obj_add_flag(s_ui.multi_bar, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_ui.sync_btn != nullptr) {
            lv_obj_clear_flag(s_ui.sync_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ExitMultiModeEx(bool rebuild, bool clear_status) {
    if (!s_multi && s_selected.empty()) {
        if (rebuild) {
            RequestRebuildListPage();
        }
        return;
    }
    s_multi = false;
    s_suppress_row_click_until_us = 0;
    s_suppress_row_click_idx = -1;
    s_selected.assign(s_items.size(), 0);
    if (clear_status) {
        SetStatusText("");
    }
    RefreshFooterMode();
    if (rebuild) {
        RequestRebuildListPage();
    }
}

void ExitMultiMode(bool rebuild) {
    ExitMultiModeEx(rebuild, true);
}

void EnterMultiModeSelect(int idx) {
    HideActionDialog();
    s_multi = true;
    SyncSelectedSize();
    s_selected.assign(s_items.size(), 0);
    // 长按进入即勾选当前项
    if (idx >= 0 && idx < static_cast<int>(s_selected.size())) {
        s_selected[static_cast<size_t>(idx)] = 1;
    }
    RefreshFooterMode();
    RequestRebuildListPage();
}

void RebuildListPage() {
    if (!s_screen_alive || s_ui.list_host == nullptr) {
        return;
    }
    lv_obj_clean(s_ui.list_host);
    s_ui.list_empty = nullptr;

    const int filtered = FilteredCount();
    if (filtered <= 0) {
        lv_obj_set_style_layout(s_ui.list_host, LV_LAYOUT_NONE, 0);
        s_ui.list_empty = lv_label_create(s_ui.list_host);
        const char* empty_txt = s_empty_override[0] != '\0'
                                    ? s_empty_override
                                    : (s_tab == 1 ? Lang::Strings::TASK_EMPTY_DONE : Lang::Strings::TASK_EMPTY_TODO);
        lv_label_set_text(s_ui.list_empty, empty_txt);
        lv_obj_set_style_text_font(s_ui.list_empty, TaskFont(), 0);
        lv_obj_set_style_text_color(s_ui.list_empty, lv_color_black(), 0);
        lv_obj_center(s_ui.list_empty);
        lv_obj_clear_flag(s_ui.list_empty, LV_OBJ_FLAG_CLICKABLE);
        RefreshPageLbl();
        return;
    }

    lv_obj_set_style_layout(s_ui.list_host, LV_LAYOUT_FLEX, 0);
    lv_obj_set_flex_flow(s_ui.list_host, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.list_host, kRowGap, 0);

    const int pages = ListPageCount();
    if (s_list_page < 0) {
        s_list_page = 0;
    }
    if (s_list_page >= pages) {
        s_list_page = pages - 1;
    }
    const int start = s_list_page * s_list_per_page;
    const int end = start + s_list_per_page;
    const lv_coord_t row_inner_w = LV_HOR_RES - kPad * 2 - kListFramePad * 2 -
                                   kListFrameBorderW * 2 - kRowPad * 2 - kBorderW * 2;
    const lv_coord_t text_w =
        s_multi ? (row_inner_w - kCheckSize - kCheckGap) : row_inner_w;

    int match_i = 0;
    for (int i = 0; i < static_cast<int>(s_items.size()); ++i) {
        const TaskItem& item = s_items[static_cast<size_t>(i)];
        if (!ItemInCurrentTab(item)) {
            continue;
        }
        const int slot = match_i++;
        if (slot < start || slot >= end) {
            continue;
        }

        void* idx_ud = reinterpret_cast<void*>(static_cast<intptr_t>(i));
        const bool selected = s_multi && ItemSelected(i);

        lv_obj_t* row = lv_obj_create(s_ui.list_host);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
        lv_obj_set_style_border_width(row, kBorderW, 0);
        lv_obj_set_style_radius(row, kRowRadius, 0);
        lv_obj_set_style_pad_all(row, kRowPad, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, kCheckGap, 0);
        DisableScroll(row);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, idx_ud);
        HapticAttachClick(row);
        lv_obj_add_event_cb(row, OnRowClicked, LV_EVENT_CLICKED, idx_ud);
        lv_obj_add_event_cb(row, OnRowLongPressed, LV_EVENT_LONG_PRESSED, idx_ud);

        lv_obj_t* text_col = lv_obj_create(row);
        lv_obj_remove_style_all(text_col);
        lv_obj_set_flex_grow(text_col, 1);
        lv_obj_set_height(text_col, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(text_col, LV_OPA_TRANSP, 0);
        lv_obj_set_flex_flow(text_col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(text_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(text_col, kRowLineGap, 0);
        DisableScroll(text_col);
        lv_obj_clear_flag(text_col, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* name = lv_label_create(text_col);
        lv_label_set_text(name, item.title);
        lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(name, text_w);
        lv_obj_set_style_text_font(name, TaskFont(), 0);
        lv_obj_set_style_text_color(name, lv_color_black(), 0);
        lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);

        char meta[72];
        FormatPlanMeta(meta, sizeof(meta), item);
        if (meta[0] != '\0') {
            lv_obj_t* hint = lv_label_create(text_col);
            lv_label_set_text(hint, meta);
            lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
            lv_obj_set_width(hint, text_w);
            lv_obj_set_style_text_font(hint, TaskFont(), 0);
            lv_obj_set_style_text_color(hint, lv_color_black(), 0);
            lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
        }

        if (s_multi) {
            // 行尾勾选：白底黑框；选中只画 √，勿实心填充
            lv_obj_t* check = lv_obj_create(row);
            lv_obj_remove_style_all(check);
            lv_obj_set_size(check, kCheckSize, kCheckSize);
            lv_obj_set_style_bg_color(check, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(check, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(check, lv_color_black(), 0);
            lv_obj_set_style_border_width(check, kBorderW, 0);
            lv_obj_set_style_radius(check, kCheckRadius, 0);
            DisableScroll(check);
            // 点框与点行相同：都走 row CLICKED 切换选中
            lv_obj_clear_flag(check, LV_OBJ_FLAG_CLICKABLE);
            if (selected) {
                lv_obj_t* mark = lv_label_create(check);
                lv_label_set_text(mark, "√");
                lv_obj_set_style_text_font(mark, TaskFont(), 0);
                lv_obj_set_style_text_color(mark, lv_color_black(), 0);
                lv_obj_center(mark);
                lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
            }
        }
    }

    RefreshPageLbl();
    RefreshFooterMode();
}

void ShowActionDialog(int idx) {
    if (idx < 0 || idx >= static_cast<int>(s_items.size())) {
        return;
    }
    EnsureActionDialog();
    if (s_ui.dialog == nullptr) {
        return;
    }
    s_pending_action_idx = idx;
    const TaskItem& item = s_items[static_cast<size_t>(idx)];
    if (s_ui.dialog_msg != nullptr) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "「%s」", item.title);
        lv_label_set_text(s_ui.dialog_msg, buf);
    }
    // 已完成任务不再显示「完成待办」
    if (s_ui.dialog_complete_btn != nullptr) {
        if (ItemIsDone(item)) {
            lv_obj_add_flag(s_ui.dialog_complete_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_ui.dialog_complete_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_clear_flag(s_ui.dialog, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.dialog);
    s_dialog_open.store(true, std::memory_order_release);
}

void OnRowLongPressed(lv_event_t* e) {
    if (s_busy.load() || s_dialog_open.load(std::memory_order_acquire)) {
        return;
    }
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (idx < 0 || idx >= static_cast<int>(s_items.size())) {
        return;
    }
    if (!s_multi) {
        s_suppress_row_click_idx = idx;
        s_suppress_row_click_until_us = esp_timer_get_time() + kSuppressRowClickUs;
        EnterMultiModeSelect(idx);
        return;
    }
    ToggleItemSelected(idx);
    RequestCheckMarksPaint();
}

void OnRowClicked(lv_event_t* e) {
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (idx == s_suppress_row_click_idx &&
        esp_timer_get_time() < s_suppress_row_click_until_us) {
        return;
    }
    if (s_busy.load()) {
        return;
    }
    if (s_multi) {
        ToggleItemSelected(idx);
        RequestCheckMarksPaint();
        return;
    }
    ShowActionDialog(idx);
}

void OnDialogDelete(lv_event_t* /*e*/) {
    const int idx = s_pending_action_idx;
    HideActionDialog();
    if (idx < 0 || idx >= static_cast<int>(s_items.size())) {
        return;
    }
    ScheduleDelete(idx);
}

void OnDialogComplete(lv_event_t* /*e*/) {
    const int idx = s_pending_action_idx;
    HideActionDialog();
    if (idx < 0 || idx >= static_cast<int>(s_items.size())) {
        return;
    }
    if (ItemIsDone(s_items[static_cast<size_t>(idx)])) {
        SetStatusText(Lang::Strings::TASK_DONE_OK, true);
        return;
    }
    ScheduleComplete(idx);
}

bool ParseItemsJson(const std::string& body, std::vector<TaskItem>& out, std::string& err) {
    out.clear();
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        err = Lang::Strings::TASK_PARSE_FAIL;
        return false;
    }
    cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 0) {
        cJSON* msg = cJSON_GetObjectItemCaseSensitive(root, "msg");
        if (cJSON_IsString(msg) && msg->valuestring != nullptr) {
            err = msg->valuestring;
        } else {
            err = Lang::Strings::TASK_API_ERROR;
        }
        cJSON_Delete(root);
        return false;
    }
    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsArray(data)) {
        err = Lang::Strings::TASK_DATA_FORMAT_ERR;
        cJSON_Delete(root);
        return false;
    }

    const cJSON* it = nullptr;
    cJSON_ArrayForEach(it, data) {
        if (!cJSON_IsObject(it) || out.size() >= static_cast<size_t>(kMaxItems)) {
            continue;
        }
        cJSON* id = cJSON_GetObjectItemCaseSensitive(it, "id");
        cJSON* title = cJSON_GetObjectItemCaseSensitive(it, "title");
        if (!cJSON_IsString(id) || id->valuestring == nullptr ||
            !cJSON_IsString(title) || title->valuestring == nullptr) {
            continue;
        }
        TaskItem item;
        strlcpy(item.id, id->valuestring, sizeof(item.id));
        strlcpy(item.title, title->valuestring, sizeof(item.title));

        cJSON* plan_date = cJSON_GetObjectItemCaseSensitive(it, "planDate");
        if (cJSON_IsString(plan_date) && plan_date->valuestring != nullptr) {
            strlcpy(item.plan_date, plan_date->valuestring, sizeof(item.plan_date));
        }
        cJSON* plan_time = cJSON_GetObjectItemCaseSensitive(it, "planTime");
        if (cJSON_IsString(plan_time) && plan_time->valuestring != nullptr) {
            strlcpy(item.plan_time, plan_time->valuestring, sizeof(item.plan_time));
        }
        cJSON* status = cJSON_GetObjectItemCaseSensitive(it, "status");
        if (cJSON_IsString(status) && status->valuestring != nullptr) {
            strlcpy(item.status, status->valuestring, sizeof(item.status));
        } else {
            strlcpy(item.status, "pending", sizeof(item.status));
        }
        out.push_back(item);
    }
    cJSON_Delete(root);
    return true;
}

bool HttpJson(const char* method, const std::string& url, std::string& body_out,
              std::string& err_out) {
    body_out.clear();
    // 短时联网：等 WiFi → 请求；返回后 Release，WiFi 板可再 pause
    PowerNeedHold hold_net(PowerNeed::OtaDownload);

    if (!Board::GetInstance().EnsureNetworkReady()) {
        err_out = Lang::Strings::TASK_NET_NOT_READY;
        return false;
    }
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        err_out = Lang::Strings::TASK_NO_NETWORK;
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        err_out = Lang::Strings::TASK_CONN_FAIL;
        return false;
    }
    http->SetTimeout(kHttpTimeoutMs);
    api::ApplyCommonHeaders(http);
    // 空 POST/PUT 必须 SetContent("")，否则 HttpClient 走 chunked 且不发结束块，服务端一直等 body
    if (method != nullptr &&
        (std::strcmp(method, "POST") == 0 || std::strcmp(method, "PUT") == 0)) {
        http->SetContent(std::string());
    }
    api::LogHttpRequest(TAG, method, url);
    if (!http->Open(method, url)) {
        err_out = Lang::Strings::TASK_REQUEST_FAIL;
        api::LogHttpResponse(TAG, -1, err_out);
        return false;
    }
    const int status = http->GetStatusCode();
    body_out = http->ReadAll();
    http->Close();
    api::LogHttpResponse(TAG, status, api::RedactClawUrlsForLog(body_out));
    if (status < 200 || status >= 300) {
        err_out = body_out.empty() ? ("HTTP " + std::to_string(status)) : Lang::Strings::TASK_REQUEST_FAIL;
        return false;
    }
    return true;
}

// 必须在 LVGL 任务里执行（经 lv_async_call），禁止 Application::Schedule 直接碰控件。
void AsyncApplyFetch(void* p) {
    auto* msg = static_cast<FetchResultMsg*>(p);
    s_busy.store(false);
    if (!msg->ok) {
        if (s_screen_alive) {
            const char* err = msg->err[0] != '\0' ? msg->err : Lang::Strings::TASK_LOAD_FAIL;
            if (s_items.empty()) {
                // 空列表：只在中间显示一条，勿叠「暂无待办」+「网络未就绪」
                SetStatusText("");
                SetEmptyOverride(err);
            } else {
                SetEmptyOverride(nullptr);
                SetStatusText(err, true);
            }
            RebuildListPage();
        }
        ReleaseFetchNet(); // 结果已上屏（或页已离）：再放网，启动保网计时
        delete msg;
        return;
    }
    if (s_screen_alive) {
        s_items = std::move(msg->items);
        SortItemsByPlanDesc(s_items);
        s_list_page = 0;
        PublishChecklistCache();
        SetEmptyOverride(nullptr);
        ExitMultiModeEx(false, false);
        SetStatusText(Lang::Strings::TASK_REFRESHED, true);
        RebuildListPage();
    } else {
        // 后台刷新：只写 cache
        SortItemsByPlanDesc(msg->items);
        checklist_cache_begin();
        for (const auto& item : msg->items) {
            checklist_cache_append(item.id, item.title, item.status, item.plan_date,
                                   item.plan_time);
        }
        checklist_cache_end();
    }
    ReleaseFetchNet();
    delete msg;
}

void AsyncApplyDelete(void* p) {
    auto* msg = static_cast<DeleteResultMsg*>(p);
    s_busy.store(false);
    if (!s_screen_alive) {
        delete msg;
        return;
    }
    if (!msg->ok) {
        SetStatusText(msg->err[0] != '\0' ? msg->err : Lang::Strings::TASK_DELETE_FAIL, true);
        delete msg;
        return;
    }
    for (auto it = s_items.begin(); it != s_items.end(); ++it) {
        if (std::strcmp(it->id, msg->id) == 0) {
            s_items.erase(it);
            break;
        }
    }
    PublishChecklistCache();
    SetEmptyOverride(nullptr);
    SetStatusText(Lang::Strings::TASK_DELETED, true);
    RebuildListPage();
    delete msg;
}

void AsyncApplyComplete(void* p) {
    auto* msg = static_cast<CompleteResultMsg*>(p);
    s_busy.store(false);
    if (!s_screen_alive) {
        delete msg;
        return;
    }
    if (!msg->ok) {
        SetStatusText(msg->err[0] != '\0' ? msg->err : Lang::Strings::TASK_COMPLETE_FAIL, true);
        delete msg;
        return;
    }
    for (auto& item : s_items) {
        if (std::strcmp(item.id, msg->id) == 0) {
            strlcpy(item.status, "done", sizeof(item.status));
            break;
        }
    }
    PublishChecklistCache();
    SetEmptyOverride(nullptr);
    SetStatusText(Lang::Strings::TASK_COMPLETED, true);
    RebuildListPage();
    delete msg;
}

void FetchTask(void* /*arg*/) {
    // 硬占网至 AsyncApplyFetch：结果上屏后再 Release，启动保网时长
    AcquireFetchNet();
    auto* msg = new FetchResultMsg();
    std::string body;
    std::string err;
    const bool http_ok = HttpJson("GET", api::ChecklistItemsAllUrl(), body, err);
    if (!http_ok) {
        msg->ok = false;
        strlcpy(msg->err, err.c_str(), sizeof(msg->err));
    } else if (!ParseItemsJson(body, msg->items, err)) {
        msg->ok = false;
        strlcpy(msg->err, err.c_str(), sizeof(msg->err));
    } else {
        msg->ok = true;
    }

    s_fetch_task = nullptr;
    if (lv_async_call(AsyncApplyFetch, msg) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "lv_async_call fetch failed");
        delete msg;
        s_busy.store(false);
        ReleaseFetchNet();
    }
    vTaskDelete(nullptr);
}

void DeleteTask(void* arg) {
    char id[48] = {};
    strlcpy(id, static_cast<const char*>(arg), sizeof(id));
    delete[] static_cast<char*>(arg);

    auto* msg = new DeleteResultMsg();
    strlcpy(msg->id, id, sizeof(msg->id));

    std::string body;
    std::string err;
    bool ok = HttpJson("DELETE", api::ChecklistItemUrl(id), body, err);
    if (ok && !body.empty()) {
        cJSON* root = cJSON_Parse(body.c_str());
        if (root != nullptr) {
            cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
            if (!cJSON_IsNumber(code) || code->valueint != 0) {
                ok = false;
                cJSON* m = cJSON_GetObjectItemCaseSensitive(root, "msg");
                if (cJSON_IsString(m) && m->valuestring != nullptr) {
                    err = m->valuestring;
                } else {
                    err = Lang::Strings::TASK_DELETE_FAIL;
                }
            }
            cJSON_Delete(root);
        }
    }
    if (!ok && err.empty()) {
        err = Lang::Strings::TASK_DELETE_FAIL;
    }
    msg->ok = ok;
    if (!ok) {
        strlcpy(msg->err, err.c_str(), sizeof(msg->err));
    }

    s_delete_task = nullptr;
    if (lv_async_call(AsyncApplyDelete, msg) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "lv_async_call delete failed");
        delete msg;
        s_busy.store(false);
    }
    vTaskDelete(nullptr);
}

void CompleteTask(void* arg) {
    char id[48] = {};
    strlcpy(id, static_cast<const char*>(arg), sizeof(id));
    delete[] static_cast<char*>(arg);

    auto* msg = new CompleteResultMsg();
    strlcpy(msg->id, id, sizeof(msg->id));

    std::string body;
    std::string err;
    bool ok = HttpJson("POST", api::ChecklistItemCompleteUrl(id), body, err);
    if (ok && !body.empty()) {
        cJSON* root = cJSON_Parse(body.c_str());
        if (root != nullptr) {
            cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
            if (!cJSON_IsNumber(code) || code->valueint != 0) {
                ok = false;
                cJSON* m = cJSON_GetObjectItemCaseSensitive(root, "msg");
                if (cJSON_IsString(m) && m->valuestring != nullptr) {
                    err = m->valuestring;
                } else {
                    err = Lang::Strings::TASK_COMPLETE_FAIL;
                }
            }
            cJSON_Delete(root);
        }
    }
    if (!ok && err.empty()) {
        err = Lang::Strings::TASK_COMPLETE_FAIL;
    }
    msg->ok = ok;
    if (!ok) {
        strlcpy(msg->err, err.c_str(), sizeof(msg->err));
    }

    s_complete_task = nullptr;
    if (lv_async_call(AsyncApplyComplete, msg) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "lv_async_call complete failed");
        delete msg;
        s_busy.store(false);
    }
    vTaskDelete(nullptr);
}

void ScheduleDelete(int idx) {
    if (!s_screen_alive || idx < 0 || idx >= static_cast<int>(s_items.size())) {
        return;
    }
    if (AnyWorkerBusy()) {
        SetStatusText(Lang::Strings::TASK_PLEASE_WAIT, true);
        return;
    }
    char* id_copy = new char[48];
    strlcpy(id_copy, s_items[static_cast<size_t>(idx)].id, 48);
    s_busy.store(true);
    SetStatusText(Lang::Strings::TASK_DELETING);
    if (xTaskCreatePinnedToCore(DeleteTask, "task_del", kWorkerStack, id_copy,
                                tskIDLE_PRIORITY + 2, &s_delete_task, 0) != pdPASS) {
        delete[] id_copy;
        s_delete_task = nullptr;
        s_busy.store(false);
        SetStatusText(Lang::Strings::TASK_DELETE_START_FAIL, true);
    }
}

void ScheduleComplete(int idx) {
    if (!s_screen_alive || idx < 0 || idx >= static_cast<int>(s_items.size())) {
        return;
    }
    if (AnyWorkerBusy()) {
        SetStatusText(Lang::Strings::TASK_PLEASE_WAIT, true);
        return;
    }
    char* id_copy = new char[48];
    strlcpy(id_copy, s_items[static_cast<size_t>(idx)].id, 48);
    s_busy.store(true);
    SetStatusText(Lang::Strings::TASK_COMPLETING);
    if (xTaskCreatePinnedToCore(CompleteTask, "task_done", kWorkerStack, id_copy,
                                tskIDLE_PRIORITY + 2, &s_complete_task, 0) != pdPASS) {
        delete[] id_copy;
        s_complete_task = nullptr;
        s_busy.store(false);
        SetStatusText(Lang::Strings::TASK_COMPLETE_START_FAIL, true);
    }
}

struct BatchWork {
    BatchOp op = BatchOp::kDelete;
    int id_n = 0;
    char ids[kMaxItems][48] = {};
};

void AsyncApplyBatch(void* p) {
    auto* msg = static_cast<BatchResultMsg*>(p);
    s_busy.store(false);
    if (!s_screen_alive) {
        delete msg;
        return;
    }
    if (msg->ok_n > 0) {
        if (msg->op == BatchOp::kDelete) {
            for (int i = 0; i < msg->ok_n; ++i) {
                for (auto it = s_items.begin(); it != s_items.end(); ++it) {
                    if (std::strcmp(it->id, msg->ok_ids[i]) == 0) {
                        s_items.erase(it);
                        break;
                    }
                }
            }
        } else {
            for (int i = 0; i < msg->ok_n; ++i) {
                for (auto& item : s_items) {
                    if (std::strcmp(item.id, msg->ok_ids[i]) == 0) {
                        strlcpy(item.status, "done", sizeof(item.status));
                        break;
                    }
                }
            }
        }
        PublishChecklistCache();
        SetEmptyOverride(nullptr);
    }

    char buf[64];
    if (msg->ok_n > 0 && msg->fail_n == 0) {
        std::snprintf(buf, sizeof(buf), msg->op == BatchOp::kDelete ? Lang::Strings::TASK_REMOVED_N_FMT : Lang::Strings::TASK_COMPLETED_N_FMT,
                      msg->ok_n);
        SetStatusText(buf, true);
    } else if (msg->ok_n > 0) {
        std::snprintf(buf, sizeof(buf), Lang::Strings::TASK_BATCH_RESULT_FMT, msg->ok_n, msg->fail_n);
        SetStatusText(buf, true);
    } else {
        SetStatusText(msg->err[0] != '\0' ? msg->err : Lang::Strings::TASK_BATCH_FAIL, true);
    }
    ExitMultiModeEx(true, false);
    delete msg;
}

bool BatchHttpOne(BatchOp op, const char* id, std::string& err) {
    if (id == nullptr || id[0] == '\0') {
        err = Lang::Strings::TASK_INVALID;
        return false;
    }
    std::string body;
    bool ok = false;
    if (op == BatchOp::kDelete) {
        ok = HttpJson("DELETE", api::ChecklistItemUrl(id), body, err);
    } else {
        ok = HttpJson("POST", api::ChecklistItemCompleteUrl(id), body, err);
    }
    if (ok && !body.empty()) {
        cJSON* root = cJSON_Parse(body.c_str());
        if (root != nullptr) {
            cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
            if (!cJSON_IsNumber(code) || code->valueint != 0) {
                ok = false;
                cJSON* m = cJSON_GetObjectItemCaseSensitive(root, "msg");
                if (cJSON_IsString(m) && m->valuestring != nullptr) {
                    err = m->valuestring;
                } else {
                    err = op == BatchOp::kDelete ? Lang::Strings::TASK_DELETE_FAIL : Lang::Strings::TASK_COMPLETE_FAIL;
                }
            }
            cJSON_Delete(root);
        }
    }
    return ok;
}

void BatchTask(void* arg) {
    auto* work = static_cast<BatchWork*>(arg);
    auto* msg = new BatchResultMsg();
    msg->op = work->op;

    if (work->id_n <= 0) {
        msg->fail_n = 1;
        strlcpy(msg->err, work->op == BatchOp::kComplete ? Lang::Strings::TASK_NO_COMPLETABLE : Lang::Strings::TASK_SELECT_FIRST,
                sizeof(msg->err));
    } else {
        // 整批保网（须在 vTaskDelete 前 Release，勿跨 delete 用 RAII）
        {
            PowerNeedHold hold_net(PowerNeed::OtaDownload);
            if (!Board::GetInstance().EnsureNetworkReady()) {
                msg->fail_n = work->id_n;
                strlcpy(msg->err, Lang::Strings::TASK_NET_NOT_READY, sizeof(msg->err));
            } else {
                for (int i = 0; i < work->id_n; ++i) {
                    std::string err;
                    bool ok = BatchHttpOne(work->op, work->ids[i], err);
                    // 连接超时等：重试 1 次
                    if (!ok) {
                        vTaskDelay(pdMS_TO_TICKS(400));
                        Board::GetInstance().EnsureNetworkReady();
                        err.clear();
                        ok = BatchHttpOne(work->op, work->ids[i], err);
                    }
                    if (ok) {
                        if (msg->ok_n < kMaxItems) {
                            strlcpy(msg->ok_ids[msg->ok_n], work->ids[i], sizeof(msg->ok_ids[0]));
                            ++msg->ok_n;
                        }
                    } else {
                        ++msg->fail_n;
                        if (msg->err[0] == '\0') {
                            strlcpy(msg->err, err.empty() ? Lang::Strings::TASK_BATCH_FAIL : err.c_str(),
                                    sizeof(msg->err));
                        }
                    }
                }
            }
        }
    }

    delete work;
    s_batch_task = nullptr;
    if (lv_async_call(AsyncApplyBatch, msg) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "lv_async_call batch failed");
        delete msg;
        s_busy.store(false);
    }
    vTaskDelete(nullptr);
}

void ScheduleBatch(BatchOp op) {
    if (!s_screen_alive || !s_multi) {
        return;
    }
    if (AnyWorkerBusy()) {
        SetStatusText(Lang::Strings::TASK_PLEASE_WAIT, true);
        return;
    }
    auto* work = new BatchWork();
    work->op = op;
    SyncSelectedSize();
    for (size_t i = 0; i < s_items.size() && i < s_selected.size(); ++i) {
        if (s_selected[i] == 0) {
            continue;
        }
        if (op == BatchOp::kComplete && ItemIsDone(s_items[i])) {
            continue;
        }
        if (work->id_n >= kMaxItems) {
            break;
        }
        strlcpy(work->ids[work->id_n], s_items[i].id, sizeof(work->ids[0]));
        ++work->id_n;
    }
    if (work->id_n <= 0) {
        delete work;
        SetStatusText(op == BatchOp::kComplete ? Lang::Strings::TASK_NO_COMPLETABLE : Lang::Strings::TASK_SELECT_FIRST, true);
        return;
    }
    s_busy.store(true);
    SetStatusText(op == BatchOp::kDelete ? Lang::Strings::TASK_BATCH_REMOVING : Lang::Strings::TASK_BATCH_COMPLETING);
    if (xTaskCreatePinnedToCoreWithCaps(BatchTask, "task_batch", kBatchWorkerStack, work,
                                        tskIDLE_PRIORITY + 2, &s_batch_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        delete work;
        s_batch_task = nullptr;
        s_busy.store(false);
        SetStatusText(Lang::Strings::TASK_BATCH_START_FAIL, true);
    }
}

void OnMultiCancel(lv_event_t* /*e*/) {
    if (s_busy.load()) {
        return;
    }
    ExitMultiMode(true);
}

void OnMultiSelectAll(lv_event_t* /*e*/) {
    if (s_busy.load() || !s_multi) {
        return;
    }
    SyncSelectedSize();
    const int filtered = FilteredCount();
    if (filtered <= 0) {
        return;
    }
    int selected_in_tab = 0;
    for (size_t i = 0; i < s_items.size() && i < s_selected.size(); ++i) {
        if (ItemInCurrentTab(s_items[i]) && s_selected[i] != 0) {
            ++selected_in_tab;
        }
    }
    const bool clear = (selected_in_tab >= filtered);
    for (size_t i = 0; i < s_items.size() && i < s_selected.size(); ++i) {
        if (ItemInCurrentTab(s_items[i])) {
            s_selected[i] = clear ? 0 : 1;
        }
    }
    RequestCheckMarksPaint();
}

void OnMultiDelete(lv_event_t* /*e*/) {
    ScheduleBatch(BatchOp::kDelete);
}

void OnMultiComplete(lv_event_t* /*e*/) {
    if (s_tab == 1) {
        return;
    }
    ScheduleBatch(BatchOp::kComplete);
}

void OnSyncClicked(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_dialog_open.load(std::memory_order_acquire)) {
        return;
    }
    if (AnyWorkerBusy()) {
        SetStatusText(Lang::Strings::TASK_PLEASE_WAIT, true);
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (s_last_user_sync_us != 0 && (now - s_last_user_sync_us) < kSyncThrottleUs) {
        SetStatusText(Lang::Strings::TASK_PLEASE_WAIT, true);
        return;
    }
    if (Board::GetInstance().IsWifiConfigMode()) {
        SetStatusText(Lang::Strings::TASK_NEED_WIFI_CFG, true);
        return;
    }
    s_last_user_sync_us = now;
    SetStatusText(Board::GetInstance().IsNetworkReady() ? Lang::Strings::TASK_REFRESHING : Lang::Strings::TASK_CONNECTING_NET);
    TaskScreen::RequestCacheRefresh();
    if (!s_busy.load()) {
        SetStatusText(Lang::Strings::TASK_REFRESH_START_FAIL, true);
    }
}

lv_obj_t* MakeDialogBtn(lv_obj_t* parent, const char* text, bool filled, lv_event_cb_t cb) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, kActionH);
    lv_obj_set_style_radius(btn, kRowRadius, 0);
    lv_obj_set_style_border_width(btn, kBorderW, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    if (filled) {
        lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    }
    DisableScroll(btn);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, TaskFont(), 0);
    lv_obj_set_style_text_color(lbl, filled ? lv_color_white() : lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

void OnDialogBackdropClicked(lv_event_t* e) {
    // 仅点遮罩本身关闭；点到卡片/按钮时 target ≠ current_target
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) {
        return;
    }
    HideActionDialog();
}

void EnsureActionDialog() {
    if (s_ui.dialog != nullptr || s_ui.screen == nullptr) {
        return;
    }

    s_ui.dialog = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.dialog);
    lv_obj_set_size(s_ui.dialog, LV_HOR_RES, LV_VER_RES);
    // 弹框外：网点灰底，点此区域关闭
    ScreenApplyDotBackdrop(s_ui.dialog);
    lv_obj_set_style_layout(s_ui.dialog, LV_LAYOUT_NONE, 0);
    DisableScroll(s_ui.dialog);
    lv_obj_add_flag(s_ui.dialog, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_ui.dialog, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_ui.dialog, OnDialogBackdropClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* card = lv_obj_create(s_ui.dialog);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_HOR_RES - kPad * 2);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, LV_VER_RES - kPad * 2, 0);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, kBorderW, 0);
    lv_obj_set_style_radius(card, kRowRadius, 0);
    lv_obj_set_style_pad_all(card, kDialogPad, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, kDialogRowGap, 0);
    DisableScroll(card);
    // 吸收点击，避免点卡片空白处落到遮罩上误关
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        card, [](lv_event_t* e) { lv_event_stop_bubbling(e); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_center(card);

    s_ui.dialog_msg = lv_label_create(card);
    lv_label_set_text(s_ui.dialog_msg, Lang::Strings::TASK_CHOOSE_ACTION);
    lv_obj_set_width(s_ui.dialog_msg, LV_HOR_RES - kPad * 2 - kDialogMsgInset);
    lv_obj_set_height(s_ui.dialog_msg, LV_SIZE_CONTENT);
    lv_label_set_long_mode(s_ui.dialog_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_ui.dialog_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_ui.dialog_msg, TaskFont(), 0);
    lv_obj_set_style_text_color(s_ui.dialog_msg, lv_color_black(), 0);
    lv_obj_clear_flag(s_ui.dialog_msg, LV_OBJ_FLAG_CLICKABLE);

    // 一行一个：完成（进行中才显示）/ 删除；无返回按钮，点遮罩关闭
    s_ui.dialog_complete_btn = MakeDialogBtn(card, Lang::Strings::TASK_COMPLETE_TODO, true, OnDialogComplete);
    MakeDialogBtn(card, Lang::Strings::TASK_DELETE_TODO, false, OnDialogDelete);
}

void StyleTabBtn(lv_obj_t* btn, lv_obj_t* lbl, bool selected) {
    // 选中段本身是完整胶囊（两侧圆弧），中间靠圆弧衔接而非竖线
    lv_obj_set_style_bg_color(btn, selected ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    if (lbl != nullptr) {
        lv_obj_set_style_text_color(lbl, selected ? lv_color_white() : lv_color_black(), 0);
    }
}

void RefreshTabUi() {
    for (int i = 0; i < kTabCount; ++i) {
        StyleTabBtn(s_ui.tab_btns[i], s_ui.tab_lbls[i], s_tab == i);
    }
    if (s_ui.section_title != nullptr) {
        lv_label_set_text(s_ui.section_title, s_tab == 1 ? Lang::Strings::TASK_TITLE_DONE : Lang::Strings::TASK_TITLE_TODO);
    }
}

void OnTabClicked(lv_event_t* e) {
    const int tab = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (tab < 0 || tab >= kTabCount || tab == s_tab) {
        return;
    }
    HideActionDialog();
    ExitMultiMode(false);
    s_tab = tab;
    s_list_page = 0;
    SetEmptyOverride(nullptr);
    RefreshTabUi();
    RebuildListPage();
}

lv_obj_t* MakeTabBtn(lv_obj_t* parent, const char* text, int tab) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, lv_pct(100));
    lv_obj_set_style_pad_all(btn, 0, 0);
    DisableScroll(btn);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, OnTabClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(tab)));

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, TaskFont(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    s_ui.tab_btns[tab] = btn;
    s_ui.tab_lbls[tab] = lbl;
    return btn;
}

bool OnVkKey(const char* key); // 前置

ScreenPaintCoalesce s_task_paint{};
ScreenPaintCoalesce s_task_check_paint{};

lv_obj_t* FindListRow(int index) {
    if (s_ui.list_host == nullptr || !lv_obj_is_valid(s_ui.list_host)) {
        return nullptr;
    }
    const uint32_t n = lv_obj_get_child_count(s_ui.list_host);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* row = lv_obj_get_child(s_ui.list_host, i);
        if (row != nullptr &&
            static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(row))) == index) {
            return row;
        }
    }
    return nullptr;
}

lv_obj_t* FindRowChildBySize(lv_obj_t* row, lv_coord_t w, lv_coord_t h) {
    if (row == nullptr) {
        return nullptr;
    }
    const uint32_t n = lv_obj_get_child_count(row);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* c = lv_obj_get_child(row, i);
        if (c != nullptr && lv_obj_get_width(c) == w && lv_obj_get_height(c) == h) {
            return c;
        }
    }
    return nullptr;
}

void PatchRowCheckMark(int index) {
    if (!s_multi) {
        return;
    }
    lv_obj_t* row = FindListRow(index);
    lv_obj_t* check = FindRowChildBySize(row, kCheckSize, kCheckSize);
    if (check == nullptr) {
        RequestRebuildListPage(); // 勾选列尚未建好（进多选 coalesce 未落地）
        return;
    }
    lv_obj_clean(check);
    if (!ItemSelected(index)) {
        return;
    }
    lv_obj_t* mark = lv_label_create(check);
    lv_label_set_text(mark, "√");
    lv_obj_set_style_text_font(mark, TaskFont(), 0);
    lv_obj_set_style_text_color(mark, lv_color_black(), 0);
    lv_obj_center(mark);
    lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
}

void PatchVisibleRowCheckMarks() {
    if (!s_multi || s_ui.list_host == nullptr || !lv_obj_is_valid(s_ui.list_host)) {
        return;
    }
    const uint32_t n = lv_obj_get_child_count(s_ui.list_host);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* row = lv_obj_get_child(s_ui.list_host, i);
        if (row == nullptr) {
            continue;
        }
        PatchRowCheckMark(static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(row))));
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), Lang::Strings::TASK_SELECTED_FMT, SelectedCount());
    SetStatusText(buf);
}

void RequestRebuildListPage() {
    if (s_task_paint.paint == nullptr) {
        s_task_paint.paint = RebuildListPage;
    }
    ScreenPaintCoalesceRequest(&s_task_paint);
}

void RequestCheckMarksPaint() {
    if (s_task_check_paint.paint == nullptr) {
        s_task_check_paint.paint = PatchVisibleRowCheckMarks;
    }
    // 点间隙手指会抬起：勿立刻上屏，停手后再一次刷全部 √
    ScreenPaintCoalesceRequestDebounced(&s_task_check_paint, 280000);
}

bool TaskPageRepeatStep(int page_delta) {
    if (page_delta == 0 || s_dialog_open.load(std::memory_order_acquire)) {
        return false;
    }
    const int last = std::max(0, ListPageCount() - 1);
    int next = s_list_page + page_delta;
    if (next < 0) {
        next = 0;
    } else if (next > last) {
        next = last;
    }
    if (next == s_list_page) {
        return false;
    }
    s_list_page = next;
    RequestRebuildListPage();
    return page_delta < 0 ? s_list_page > 0 : s_list_page < last;
}

bool OnVkKeyLongPress(const char* key) {
    return VkPageRepeatTryStart(key, TaskPageRepeatStep);
}

bool OnVkKeyPressUp(const char* key) {
    return VkPageRepeatOnPressUp(key);
}

bool OnVkKey(const char* key) {
    if (key == nullptr) {
        return false;
    }
    // 弹窗打开时：盖板键在 touch_feed，禁止同步改 LVGL（连续点返回会卡死）
    if (s_dialog_open.load(std::memory_order_acquire)) {
        if (std::strcmp(key, "vk_prev") == 0) {
            ScreenLvAsync([](void*) {
                if (s_screen_alive) {
                    HideActionDialog();
                }
            });
            return true;
        }
        if (std::strcmp(key, "vk_next") == 0) {
            return true;
        }
        return false;
    }
    // 多选：非首页翻上一页；首页 vk_prev 取消多选（勿出栈回首页）
    if (std::strcmp(key, "vk_prev") == 0) {
        if (s_list_page > 0) {
            --s_list_page;
            RequestRebuildListPage();
            return true;
        }
        if (s_multi) {
            if (s_busy.load()) {
                return true;
            }
            ScreenLvAsync([](void*) {
                if (s_screen_alive) {
                    ExitMultiMode(true);
                }
            });
            return true;
        }
        return false;
    }
    if (std::strcmp(key, "vk_next") == 0) {
        if (s_list_page + 1 < ListPageCount()) {
            ++s_list_page;
            RequestRebuildListPage();
        }
        return true;
    }
    return false;
}

void OnScreenUnloaded(lv_event_t* e) {
    lv_obj_t* scr = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (scr != s_ui.screen) {
        return;
    }
    CancelStatusClearTimer();
    s_screen_alive = false;
    ScreenPaintCoalesceReset(&s_task_paint);
    ScreenPaintCoalesceReset(&s_task_check_paint);
    s_ui = {};
    s_items.clear();
    s_selected.clear();
    s_multi = false;
    s_suppress_row_click_until_us = 0;
    s_suppress_row_click_idx = -1;
    s_tab = 0;
    s_list_page = 0;
    s_pending_action_idx = -1;
    s_status_text[0] = '\0';
    s_empty_override[0] = '\0';
    s_dialog_open.store(false, std::memory_order_release);
}

void OnScreenLoaded(lv_event_t* e) {
    lv_obj_t* scr = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (scr != s_ui.screen || !s_screen_alive) {
        return;
    }
    // 只读 cache；无缓存显示空态，不在本页主动 GET
    ApplyFromCache();
}

}  // namespace

lv_obj_t* TaskScreen::Create() {
    s_ui = {};
    s_screen_alive = true;
    s_items.clear();
    s_selected.clear();
    s_multi = false;
    s_suppress_row_click_until_us = 0;
    s_suppress_row_click_idx = -1;
    s_tab = 0;
    s_list_page = 0;
    s_pending_action_idx = -1;
    s_busy.store(false);
    s_dialog_open.store(false, std::memory_order_release);
    s_status_text[0] = '\0';
    s_empty_override[0] = '\0';
    s_last_user_sync_us = 0;
    CancelStatusClearTimer();

    ScreenSetIsHome(false);

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, TaskFont(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    DisableScroll(scr);
    lv_obj_add_event_cb(scr, OnScreenLoaded, LV_EVENT_SCREEN_LOADED, nullptr);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    const lv_coord_t body_h = LV_VER_RES - status.height;

    s_list_per_page =
        (body_h - kPad * 2 - kListPerPageOffset - kTabH - kRowGap - kHeaderH - kRowGap - kFooterOutsideH -
         kListFramePad * 2 - kListFrameBorderW * 2 - kActionRowH - kRowGap) /
        (EstimatedTwoLineRowH() + kRowGap);
    if (s_list_per_page < 3) {
        s_list_per_page = 3;
    }

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, body_h);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(body, kPad, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(body, kRowGap, 0);
    DisableScroll(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* tab_row = lv_obj_create(body);
    lv_obj_remove_style_all(tab_row);
    lv_obj_set_width(tab_row, kTabW);
    lv_obj_set_height(tab_row, kTabH);
    lv_obj_set_style_bg_color(tab_row, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(tab_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(tab_row, lv_color_black(), 0);
    lv_obj_set_style_border_width(tab_row, kTabBorderW, 0);
    lv_obj_set_style_radius(tab_row, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(tab_row, true, 0);
    lv_obj_set_flex_flow(tab_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tab_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(tab_row, kTabInset, 0);
    lv_obj_set_style_pad_column(tab_row, 0, 0);
    DisableScroll(tab_row);
    lv_obj_clear_flag(tab_row, LV_OBJ_FLAG_CLICKABLE);

    MakeTabBtn(tab_row, Lang::Strings::TASK_TAB_TODO, 0);
    MakeTabBtn(tab_row, Lang::Strings::TASK_TAB_DONE, 1);
    RefreshTabUi();

    lv_obj_t* header_row = lv_obj_create(body);
    lv_obj_remove_style_all(header_row);
    lv_obj_set_width(header_row, lv_pct(100));
    lv_obj_set_height(header_row, kHeaderH);
    lv_obj_set_style_bg_opa(header_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(header_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header_row, kHeaderGap, 0);
    DisableScroll(header_row);
    lv_obj_clear_flag(header_row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* star_wrap = lv_obj_create(header_row);
    lv_obj_remove_style_all(star_wrap);
    lv_obj_set_size(star_wrap, kHeaderIcon, kHeaderIcon);
    lv_obj_set_style_bg_color(star_wrap, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(star_wrap, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(star_wrap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(star_wrap, 0, 0);
    DisableScroll(star_wrap);
    lv_obj_clear_flag(star_wrap, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* star = lv_label_create(star_wrap);
    lv_label_set_text(star, "\xE2\x98\x85");  // ★
    lv_obj_set_style_text_color(star, lv_color_white(), 0);
    lv_obj_set_style_text_font(star, TaskFont(), 0);
    lv_obj_center(star);
    lv_obj_clear_flag(star, LV_OBJ_FLAG_CLICKABLE);

    s_ui.section_title = lv_label_create(header_row);
    lv_label_set_text(s_ui.section_title, Lang::Strings::TASK_TITLE_TODO);
    lv_obj_set_style_text_font(s_ui.section_title, TaskTitleFont(), 0);
    lv_obj_set_style_text_color(s_ui.section_title, lv_color_black(), 0);
    lv_obj_clear_flag(s_ui.section_title, LV_OBJ_FLAG_CLICKABLE);

    // 与「日程待办 / 已完成」同行稍后：完成中、已删除等
    s_ui.status_lbl = lv_label_create(header_row);
    lv_label_set_text(s_ui.status_lbl, "");
    lv_obj_set_style_pad_left(s_ui.status_lbl, kStatusGap, 0);
    lv_obj_set_style_text_font(s_ui.status_lbl, TaskFont(), 0);
    lv_obj_set_style_text_color(s_ui.status_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(s_ui.status_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_ui.status_lbl, LV_OBJ_FLAG_HIDDEN);

    // 外框：包住待办列表 + 同步/批量操作；页码在框外（同传输页）
    s_ui.list_frame = lv_obj_create(body);
    lv_obj_remove_style_all(s_ui.list_frame);
    lv_obj_set_width(s_ui.list_frame, LV_HOR_RES - kPad * 2);
    lv_obj_set_flex_grow(s_ui.list_frame, 1);
    lv_obj_set_style_bg_color(s_ui.list_frame, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_ui.list_frame, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ui.list_frame, kListFrameBorderW, 0);
    lv_obj_set_style_border_color(s_ui.list_frame, lv_color_black(), 0);
    lv_obj_set_style_radius(s_ui.list_frame, kListFrameRadius, 0);
    lv_obj_set_style_pad_all(s_ui.list_frame, kListFramePad, 0);
    lv_obj_set_flex_flow(s_ui.list_frame, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.list_frame, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_ui.list_frame, 0, 0);
    DisableScroll(s_ui.list_frame);
    lv_obj_clear_flag(s_ui.list_frame, LV_OBJ_FLAG_CLICKABLE);

    s_ui.list_host = lv_obj_create(s_ui.list_frame);
    lv_obj_remove_style_all(s_ui.list_host);
    lv_obj_set_width(s_ui.list_host, lv_pct(100));
    lv_obj_set_flex_grow(s_ui.list_host, 1);
    lv_obj_set_flex_flow(s_ui.list_host, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.list_host, kRowGap, 0);
    DisableScroll(s_ui.list_host);

    s_ui.footer = lv_obj_create(s_ui.list_frame);
    lv_obj_remove_style_all(s_ui.footer);
    lv_obj_set_width(s_ui.footer, lv_pct(100));
    lv_obj_set_height(s_ui.footer, kActionRowH);
    lv_obj_set_style_bg_opa(s_ui.footer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(s_ui.footer, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(s_ui.footer, kSyncDividerW, 0);
    lv_obj_set_style_border_color(s_ui.footer, lv_color_black(), 0);
    lv_obj_set_flex_flow(s_ui.footer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.footer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    DisableScroll(s_ui.footer);
    lv_obj_clear_flag(s_ui.footer, LV_OBJ_FLAG_CLICKABLE);

    // 分割线 + 居中「刷新」下划线（多选时隐藏，换 multi_bar）
    s_ui.sync_btn = lv_obj_create(s_ui.footer);
    lv_obj_remove_style_all(s_ui.sync_btn);
    lv_obj_set_width(s_ui.sync_btn, lv_pct(100));
    lv_obj_set_height(s_ui.sync_btn, kSyncBtnH);
    lv_obj_set_style_bg_opa(s_ui.sync_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_ui.sync_btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.sync_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    DisableScroll(s_ui.sync_btn);
    lv_obj_add_flag(s_ui.sync_btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(s_ui.sync_btn);
    lv_obj_add_event_cb(s_ui.sync_btn, OnSyncClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* sync_text = lv_obj_create(s_ui.sync_btn);
    lv_obj_remove_style_all(sync_text);
    lv_obj_set_size(sync_text, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_border_side(sync_text, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(sync_text, kSyncUnderlineH, 0);
    lv_obj_set_style_border_color(sync_text, lv_color_black(), 0);
    lv_obj_set_style_pad_bottom(sync_text, kSyncUnderlinePadBottom, 0);
    lv_obj_set_style_pad_hor(sync_text, kSyncUnderlinePadHor, 0);
    lv_obj_set_style_bg_opa(sync_text, LV_OPA_TRANSP, 0);
    DisableScroll(sync_text);
    lv_obj_clear_flag(sync_text, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* sync_lbl = lv_label_create(sync_text);
    lv_label_set_text(sync_lbl, Lang::Strings::TASK_REFRESH);
    lv_obj_set_style_text_font(sync_lbl, TaskFont(), 0);
    lv_obj_set_style_text_color(sync_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(sync_lbl, LV_OBJ_FLAG_CLICKABLE);

    s_ui.multi_bar = lv_obj_create(s_ui.footer);
    lv_obj_remove_style_all(s_ui.multi_bar);
    lv_obj_set_width(s_ui.multi_bar, lv_pct(100));
    lv_obj_set_height(s_ui.multi_bar, kSyncBtnH);
    lv_obj_set_style_bg_opa(s_ui.multi_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_ui.multi_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ui.multi_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_ui.multi_bar, kMultiBarGap, 0);
    DisableScroll(s_ui.multi_bar);
    lv_obj_clear_flag(s_ui.multi_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_ui.multi_bar, LV_OBJ_FLAG_HIDDEN);

    // 与「刷新」同款：下划线文字；多项间居中「·」
    auto make_multi_action = [](lv_obj_t* parent, const char* text, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_obj_create(parent);
        lv_obj_remove_style_all(btn);
        lv_obj_set_height(btn, kSyncBtnH);
        lv_obj_set_width(btn, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_hor(btn, kMultiActionPadHor, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        DisableScroll(btn);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(btn);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* text_wrap = lv_obj_create(btn);
        lv_obj_remove_style_all(text_wrap);
        lv_obj_set_size(text_wrap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_border_side(text_wrap, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(text_wrap, kSyncUnderlineH, 0);
        lv_obj_set_style_border_color(text_wrap, lv_color_black(), 0);
        lv_obj_set_style_pad_bottom(text_wrap, kSyncUnderlinePadBottom, 0);
        lv_obj_set_style_pad_hor(text_wrap, kSyncUnderlinePadHor, 0);
        lv_obj_set_style_bg_opa(text_wrap, LV_OPA_TRANSP, 0);
        DisableScroll(text_wrap);
        lv_obj_clear_flag(text_wrap, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* lbl = lv_label_create(text_wrap);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, TaskFont(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        return btn;
    };
    auto make_multi_dot = [](lv_obj_t* parent) {
        lv_obj_t* dot = lv_obj_create(parent);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, kMultiDotSize, kMultiDotSize);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        DisableScroll(dot);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        return dot;
    };
    make_multi_action(s_ui.multi_bar, Lang::Strings::COMMON_CANCEL, OnMultiCancel);
    make_multi_dot(s_ui.multi_bar);
    make_multi_action(s_ui.multi_bar, Lang::Strings::COMMON_SELECT_ALL, OnMultiSelectAll);
    make_multi_dot(s_ui.multi_bar);
    make_multi_action(s_ui.multi_bar, Lang::Strings::COMMON_REMOVE, OnMultiDelete);
    s_ui.multi_complete_sep = make_multi_dot(s_ui.multi_bar);
    s_ui.multi_complete_btn = make_multi_action(s_ui.multi_bar, Lang::Strings::TASK_COMPLETE, OnMultiComplete);

    s_ui.page_lbl = lv_label_create(body);
    lv_label_set_text(s_ui.page_lbl, "0 / 0");
    lv_obj_set_width(s_ui.page_lbl, LV_HOR_RES - kPad * 2);
    lv_obj_set_height(s_ui.page_lbl, kFooterH);
    lv_obj_set_style_text_align(s_ui.page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_ui.page_lbl, TaskFont(), 0);
    lv_obj_clear_flag(s_ui.page_lbl, LV_OBJ_FLAG_CLICKABLE);

    VkKey_AttachScreen(scr, kScreenId,
                       VkKeyScreenDesc{TaskScreen::Create, OnVkKey, nullptr, nullptr, nullptr,
                                       nullptr, OnVkKeyLongPress, OnVkKeyPressUp});
    return scr;
}

bool TaskScreen::IsActive() {
    return s_screen_alive && s_ui.screen != nullptr;
}

void TaskScreen::RequestCacheRefresh() {
    if (s_fetch_task != nullptr) {
        return;
    }
    s_busy.store(true);
    if (xTaskCreatePinnedToCore(FetchTask, "task_fetch", kWorkerStack, nullptr,
                                tskIDLE_PRIORITY + 2, &s_fetch_task, 0) != pdPASS) {
        s_fetch_task = nullptr;
        s_busy.store(false);
        ESP_LOGW(TAG, "cache refresh task start failed");
    }
}

void TaskScreen::EnsureCacheSynced() {
    if (!Board::GetInstance().IsNetworkReady()) {
        ESP_LOGW(TAG, "EnsureCacheSynced skip: network not ready");
        return;
    }
    std::vector<TaskItem> items;
    std::string body;
    std::string err;
    if (!HttpJson("GET", api::ChecklistItemsAllUrl(), body, err)) {
        ESP_LOGW(TAG, "EnsureCacheSynced GET failed: %s", err.c_str());
        return;
    }
    if (!ParseItemsJson(body, items, err)) {
        ESP_LOGW(TAG, "EnsureCacheSynced parse failed: %s", err.c_str());
        return;
    }
    SortItemsByPlanDesc(items);
    // 开机路径：只写 cache，不依赖清单页存活
    checklist_cache_begin();
    for (const auto& item : items) {
        checklist_cache_append(item.id, item.title, item.status, item.plan_date, item.plan_time);
    }
    checklist_cache_end();
    ESP_LOGI(TAG, "EnsureCacheSynced ok items=%u", static_cast<unsigned>(items.size()));
    // 开机同步时若已在清单页：投递 LVGL 刷新（勿在 Application 线程碰控件）
    if (IsActive()) {
        if (lv_async_call(
                [](void* /*p*/) {
                    if (s_screen_alive) {
                        ApplyFromCache();
                    }
                },
                nullptr) != LV_RESULT_OK) {
            ESP_LOGW(TAG, "EnsureCacheSynced: UI reload schedule failed");
        }
    }
}

void TaskScreen::ReloadFromCache() {
    if (!IsActive()) {
        return;
    }
    ApplyFromCache();
}

void TaskScreen::OnResumeFromStandby() {
    if (!IsActive()) {
        return;
    }
    ESP_LOGI(TAG, "resume from standby -> reload cache");
    ApplyFromCache();
}
