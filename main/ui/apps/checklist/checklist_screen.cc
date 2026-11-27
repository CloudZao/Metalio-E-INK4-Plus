#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>
#include <esp_log.h>
#include <esp_timer.h>
#include "ui/apps/checklist/checklist_screen.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "boards/common/board/board.h"
#include "business/checklist/checklist_cache.h"
#include "business/checklist/checklist_service.h"
#include "ui/common/runtime/page_lifecycle.h"
#include "ui/apps/checklist/checklist_screen_priv.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/runtime/modal.h"
#include "ui/common/widgets/screen_text.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/runtime/input/vk_page_repeat.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/runtime/screen_common_nav.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"

static constexpr int kTabCount = 2;
static constexpr int64_t kSyncThrottleUs = 3000000;  // 手动同步最短间隔 3s

using ChecklistScreenItem = ChecklistItem;

struct ChecklistScreenUiState {
    lv_obj_t* screen = nullptr;
    lv_obj_t* tab_btns[kTabCount] = {};
    lv_obj_t* tab_lbls[kTabCount] = {};
    lv_obj_t* section_title = nullptr;
    lv_obj_t* status_lbl = nullptr;  // 与标题同行，稍后显示完成中/已删除等
    lv_obj_t* list_host = nullptr;
    lv_obj_t* list_empty = nullptr;
    lv_obj_t* list_frame = nullptr;  // 列表+操作外框（页码在框外）
    lv_obj_t* footer = nullptr;      // 框内同步/批量操作区
    lv_obj_t* page_lbl = nullptr;
    lv_obj_t* sync_btn = nullptr;
    lv_obj_t* multi_bar = nullptr;           // 多选底栏（框内）
    lv_obj_t* multi_complete_sep = nullptr;  // 「完成」前圆点（已完成 Tab 一并隐藏）
    lv_obj_t* multi_complete_btn = nullptr;  // 批量完成（已完成 Tab 隐藏）
    lv_obj_t* dialog = nullptr;
    lv_obj_t* dialog_msg = nullptr;
    lv_obj_t* dialog_complete_btn = nullptr;
};

static ChecklistScreenUiState s_ui;
static bool s_screen_alive = false;
static std::vector<ChecklistScreenItem> s_items;
static std::vector<uint8_t> s_selected;  // 与 s_items 等长；1=多选选中
static bool s_multi = false;
static int64_t s_suppress_row_click_until_us = 0;  // 仅吞长按进多选同一行的松手假 CLICKED
static int s_suppress_row_click_idx = -1;
static constexpr int64_t kSuppressRowClickUs = 400000;  // 400ms：只挡该行松手，不挡其它行连点
static int s_tab = 0;                                   // 0=待办 1=已完成
static int s_list_page = 0;
static int s_list_per_page = 5;
static int s_pending_action_idx = -1;
static ChecklistRequest s_request; // 当前页面提交的请求，离页仅解除结果接收
static uint32_t s_cache_revision = 0; // 当前显示快照的版本
static bool s_dialog_open = false;
static UiPageLifecycle s_page;
static uint32_t s_page_generation = 0;
static int64_t s_cache_retry_at = 0; // 快照分配失败后限频重试
static int64_t s_status_clear_at = 0; // 提示自动收起时刻，0表示不自动收起
static char s_status_text[80] = {};                // 非空时显示在标题同行
static char s_empty_override[80] = {};             // 空列表时覆盖「暂无待办」等，避免与 status 叠字
static constexpr uint32_t kStatusClearMs = 2000;
static int64_t s_last_user_sync_us = 0;  // 上次手动同步发起时刻；0=尚未点过

// 清单界面：fontpack UI 默认字（与书库列表 item 一致）
static const lv_font_t* ChecklistFont() {
    return UiFont();
}

// 清单标题复用公共标题角色。
static const lv_font_t* ChecklistTitleFont() {
    return UiFont(UiFontRole::TitleEmphasis);
}

static UiModal s_window_modal;

static lv_coord_t ChecklistFontLineH() {
    const lv_font_t* font = ChecklistFont();
    return (font != nullptr && font->line_height > 0) ? font->line_height : 30;
}

static int ChecklistSyncRowHeight() {
    return std::max<int>(
        kSyncBtnH, ScreenTextHeight(ChecklistFont()) + kSyncUnderlinePadBottom + kSyncUnderlineH + kSyncDividerW + UiSy(8));
}

// 标题 + 计划时间两行时的行高估算，用于翻页每页条数
static lv_coord_t EstimatedTwoLineRowH() {
    const lv_coord_t line_h = ChecklistFontLineH();
    const lv_coord_t text_h = line_h * 2 + kRowLineGap;
    return text_h + kRowPad * 2 + kBorderW * 2;
}

static void DisableScroll(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

static bool ItemIsDone(const ChecklistScreenItem& item) {
    return std::strcmp(item.status, "done") == 0;
}

static bool ItemInCurrentTab(const ChecklistScreenItem& item) {
    return s_tab == 1 ? ItemIsDone(item) : !ItemIsDone(item);
}

static int FilteredCount() {
    int n = 0;
    for (const auto& item : s_items) {
        if (ItemInCurrentTab(item)) {
            ++n;
        }
    }
    return n;
}

static void FormatPlanMeta(char* out, size_t out_sz, const ChecklistScreenItem& item) {
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

static void RefreshPageLbl();
static void RefreshStatusLbl();
static void RebuildListPage();
static lv_obj_t* FindListRow(int index);
static lv_obj_t* FindRowChildBySize(lv_obj_t* row, lv_coord_t w, lv_coord_t h);
static void PatchRowCheckMark(int index);
static void PatchVisibleRowCheckMarks();
static void RequestRebuildListPage();
static void RequestCheckMarksPaint();
static void RefreshFooterMode();
static void ExitMultiMode(bool rebuild);
static void ExitMultiModeEx(bool rebuild, bool clear_status);
static void SyncSelectedSize();
static int SelectedCount();
static bool ItemSelected(int idx);
static void ToggleItemSelected(int idx);

static void StopChecklistUi() {
    s_page.Unbind();
    s_request.reset();
    s_status_clear_at = 0;
}

static void RefreshStatusLbl() {
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

// 参数 auto_clear：成功/错误提示定时收起；忙碌态勿开
static void SetStatusText(const char* text, bool auto_clear = false) {
    s_status_clear_at = 0;
    if (!s_screen_alive || s_ui.status_lbl == nullptr) {
        return;
    }
    const char* t = text != nullptr ? text : "";
    strlcpy(s_status_text, t, sizeof(s_status_text));
    RefreshStatusLbl();
    if (auto_clear && s_status_text[0] != '\0') {
        s_status_clear_at = esp_timer_get_time() + static_cast<int64_t>(kStatusClearMs) * 1000;
    }
}

static void SetEmptyOverride(const char* msg) {
    if (msg != nullptr && msg[0] != '\0') {
        strlcpy(s_empty_override, msg, sizeof(s_empty_override));
    } else {
        s_empty_override[0] = '\0';
    }
}

static void HideActionDialog() {
    s_pending_action_idx = -1;
    s_dialog_open = false;
    if (s_ui.dialog != nullptr) {
        lv_obj_add_flag(s_ui.dialog, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool AnyWorkerBusy() {
    return s_request != nullptr || ChecklistIsBusy();
}

static bool LoadChecklistItems(bool reset_view) {
    if (esp_timer_get_time() < s_cache_retry_at) {
        return false;
    }
    if (!ChecklistCopyItems(s_items, s_cache_revision)) {
        s_cache_retry_at = esp_timer_get_time() + 5000000;
        SetStatusText(LangString_COMMON_MEMORY_RETRY, true);
        return false;
    }
    if (reset_view) {
        // 新快照的条目顺序可能变化，关闭仍绑定旧行号的操作弹框。
        HideActionDialog();
        s_list_page = 0;
        s_multi = false;
        s_suppress_row_click_until_us = 0;
        s_suppress_row_click_idx = -1;
        s_selected.clear();
    }
    SetEmptyOverride(nullptr);
    return true;
}

static void ApplyFromCache() {
    if (LoadChecklistItems(true)) {
        SetStatusText("");
        RebuildListPage();
    }
}

static int ListPageCount() {
    const int n = FilteredCount();
    if (n <= 0 || s_list_per_page <= 0) {
        return 1;
    }
    return (n + s_list_per_page - 1) / s_list_per_page;
}

static void RefreshPageLbl() {
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

static void OnRowClicked(lv_event_t* e);
static void OnDialogBackdropClicked(lv_event_t* e);
static void OnDialogDelete(lv_event_t* e);
static void OnDialogComplete(lv_event_t* e);
static void OnTabClicked(lv_event_t* e);
static void OnRowLongPressed(lv_event_t* e);
static void OnMultiCancel(lv_event_t* e);
static void OnMultiSelectAll(lv_event_t* e);
static void OnMultiDelete(lv_event_t* e);
static void OnMultiComplete(lv_event_t* e);
static void EnsureActionDialog();
static void ScheduleUpdate(int idx, ChecklistOperation action);
static void ScheduleBatch(ChecklistOperation op);
static void RefreshTabUi();

static void SyncSelectedSize() {
    if (s_selected.size() != s_items.size()) {
        s_selected.assign(s_items.size(), 0);
    }
}

static int SelectedCount() {
    SyncSelectedSize();
    int n = 0;
    for (uint8_t v : s_selected) {
        if (v != 0) {
            ++n;
        }
    }
    return n;
}

static bool ItemSelected(int idx) {
    SyncSelectedSize();
    return idx >= 0 && idx < static_cast<int>(s_selected.size()) && s_selected[static_cast<size_t>(idx)] != 0;
}

static void ToggleItemSelected(int idx) {
    SyncSelectedSize();
    if (idx < 0 || idx >= static_cast<int>(s_selected.size())) {
        return;
    }
    s_selected[static_cast<size_t>(idx)] = s_selected[static_cast<size_t>(idx)] ? 0 : 1;
}

static void RefreshFooterMode() {
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
        std::snprintf(buf, sizeof(buf), LangString_TASK_SELECTED_FMT, SelectedCount());
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

static void ExitMultiModeEx(bool rebuild, bool clear_status) {
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

static void ExitMultiMode(bool rebuild) {
    ExitMultiModeEx(rebuild, true);
}

static void EnterMultiModeSelect(int idx) {
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

static void RebuildListPage() {
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
                                    : (s_tab == 1 ? LangString_TASK_EMPTY_DONE : LangString_TASK_EMPTY_TODO);
        lv_label_set_text(s_ui.list_empty, empty_txt);
        lv_obj_set_style_text_font(s_ui.list_empty, ChecklistFont(), 0);
        lv_obj_set_style_text_color(s_ui.list_empty, UiColorForeground(), 0);
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
    const lv_coord_t row_inner_w =
        LV_HOR_RES - kPad * 2 - kListFramePad * 2 - kListFrameBorderW * 2 - kRowPad * 2 - kBorderW * 2;
    const lv_coord_t text_w = s_multi ? (row_inner_w - kCheckSize - kCheckGap) : row_inner_w;

    int match_i = 0;
    for (int i = 0; i < static_cast<int>(s_items.size()); ++i) {
        const ChecklistScreenItem& item = s_items[static_cast<size_t>(i)];
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
        lv_obj_set_style_bg_color(row, UiColorBackground(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, UiColorForeground(), 0);
        lv_obj_set_style_border_width(row, kBorderW, 0);
        lv_obj_set_style_radius(row, kRowRadius, 0);
        lv_obj_set_style_pad_all(row, kRowPad, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
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
        lv_obj_set_flex_align(text_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(text_col, kRowLineGap, 0);
        DisableScroll(text_col);
        lv_obj_clear_flag(text_col, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* name = lv_label_create(text_col);
        lv_label_set_text(name, item.title);
        lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(name, text_w);
        lv_obj_set_style_text_font(name, ChecklistFont(), 0);
        lv_obj_set_style_text_color(name, UiColorForeground(), 0);
        lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);

        char meta[72];
        FormatPlanMeta(meta, sizeof(meta), item);
        if (meta[0] != '\0') {
            lv_obj_t* hint = lv_label_create(text_col);
            lv_label_set_text(hint, meta);
            lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
            lv_obj_set_width(hint, text_w);
            lv_obj_set_style_text_font(hint, ChecklistFont(), 0);
            lv_obj_set_style_text_color(hint, UiColorForeground(), 0);
            lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
        }

        if (s_multi) {
            // 行尾勾选：白底黑框；选中只画 √，勿实心填充
            lv_obj_t* check = lv_obj_create(row);
            lv_obj_remove_style_all(check);
            lv_obj_set_size(check, kCheckSize, kCheckSize);
            lv_obj_set_style_bg_color(check, UiColorBackground(), 0);
            lv_obj_set_style_bg_opa(check, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(check, UiColorForeground(), 0);
            lv_obj_set_style_border_width(check, kBorderW, 0);
            lv_obj_set_style_radius(check, kCheckRadius, 0);
            DisableScroll(check);
            // 点框与点行相同：都走 row CLICKED 切换选中
            lv_obj_clear_flag(check, LV_OBJ_FLAG_CLICKABLE);
            if (selected) {
                lv_obj_t* mark = lv_label_create(check);
                lv_label_set_text(mark, "√");
                lv_obj_set_style_text_font(mark, ChecklistFont(), 0);
                lv_obj_set_style_text_color(mark, UiColorForeground(), 0);
                lv_obj_center(mark);
                lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
            }
        }
    }

    RefreshPageLbl();
    RefreshFooterMode();
}

static void ShowActionDialog(int idx) {
    if (idx < 0 || idx >= static_cast<int>(s_items.size())) {
        return;
    }
    EnsureActionDialog();
    if (s_ui.dialog == nullptr) {
        return;
    }
    s_pending_action_idx = idx;
    const ChecklistScreenItem& item = s_items[static_cast<size_t>(idx)];
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
    s_dialog_open = true;
}

static void OnRowLongPressed(lv_event_t* e) {
    if (AnyWorkerBusy() || s_dialog_open) {
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

static void OnRowClicked(lv_event_t* e) {
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (idx == s_suppress_row_click_idx && esp_timer_get_time() < s_suppress_row_click_until_us) {
        return;
    }
    if (AnyWorkerBusy()) {
        return;
    }
    if (s_multi) {
        ToggleItemSelected(idx);
        RequestCheckMarksPaint();
        return;
    }
    ShowActionDialog(idx);
}

static void OnDialogDelete(lv_event_t* /*e*/) {
    const int idx = s_pending_action_idx;
    HideActionDialog();
    if (idx < 0 || idx >= static_cast<int>(s_items.size())) {
        return;
    }
    const std::string id = s_items[static_cast<size_t>(idx)].id;
    ScreenConfirmDelete(s_ui.screen, [id] {
        const auto found = std::find_if(s_items.begin(), s_items.end(),
                                        [&](const auto& item) { return item.id == id; });
        if (found != s_items.end()) {
            ScheduleUpdate(static_cast<int>(found - s_items.begin()), ChecklistOperation::Delete);
        }
    });
}

static void OnDialogComplete(lv_event_t* /*e*/) {
    const int idx = s_pending_action_idx;
    HideActionDialog();
    if (idx < 0 || idx >= static_cast<int>(s_items.size())) {
        return;
    }
    if (ItemIsDone(s_items[static_cast<size_t>(idx)])) {
        SetStatusText(LangString_TASK_DONE_OK, true);
        return;
    }
    ScheduleUpdate(idx, ChecklistOperation::Complete);
}

static void RefreshChecklistUi() {
    if (!s_page.IsCurrent(s_page_generation)) {
        return;
    }
    if (s_request != nullptr && s_request->completed.load(std::memory_order_acquire)) {
        // 业务已提交共享缓存；页面只处理本次操作的显示结果。
        const auto result = std::move(s_request);
        const bool refresh = result->operation == ChecklistOperation::Refresh;
        const bool remove = result->operation == ChecklistOperation::Delete;
        bool displayed = true;
        if (result->succeeded > 0) {
            displayed = LoadChecklistItems(refresh);
        }
        if (displayed) {
            if (refresh) {
                if (result->failed == 0) {
                    ExitMultiModeEx(false, false);
                    SetStatusText(LangString_TASK_REFRESHED, true);
                } else if (s_items.empty()) {
                    SetStatusText("");
                    SetEmptyOverride(result->error[0] ? result->error : LangString_TASK_LOAD_FAIL);
                } else {
                    SetStatusText(result->error[0] ? result->error : LangString_TASK_LOAD_FAIL, true);
                }
            } else if (result->batch) {
                char text[64];
                if (result->succeeded > 0 && result->failed == 0) {
                    std::snprintf(text, sizeof(text), remove ? LangString_TASK_REMOVED_N_FMT :
                                                              LangString_TASK_COMPLETED_N_FMT, result->succeeded);
                    SetStatusText(text, true);
                } else if (result->succeeded > 0) {
                    std::snprintf(text, sizeof(text), LangString_TASK_BATCH_RESULT_FMT,
                                  result->succeeded, result->failed);
                    SetStatusText(text, true);
                } else {
                    SetStatusText(result->error[0] ? result->error : LangString_TASK_BATCH_FAIL, true);
                }
                ExitMultiModeEx(false, false);
            } else if (result->failed != 0) {
                const char* fallback = remove ? LangString_TASK_DELETE_FAIL : LangString_TASK_COMPLETE_FAIL;
                SetStatusText(result->error[0] ? result->error : fallback, true);
            } else {
                SetStatusText(remove ? LangString_TASK_DELETED : LangString_TASK_COMPLETED, true);
            }
        }
        if (result->batch && !displayed) {
            ExitMultiModeEx(false, false);
        }
        RebuildListPage();
    } else if (!ChecklistIsBusy() && s_request == nullptr &&
               s_cache_revision != checklist_cache_revision()) {
        ApplyFromCache();
    }
    if (s_status_clear_at != 0 && esp_timer_get_time() >= s_status_clear_at) {
        s_status_clear_at = 0;
        s_status_text[0] = '\0';
        RefreshStatusLbl();
    }
}

static bool SubmitChecklist(ChecklistOperation operation, std::vector<std::string> ids = {}, bool batch = false) {
    if (!s_page.IsCurrent(s_page_generation)) {
        ESP_LOGE(TAG, "operation=submit stage=page_lifecycle result=unavailable");
        return false;
    }
    s_request = ChecklistSubmit(operation, std::move(ids), batch);
    return s_request != nullptr;
}

static void ScheduleUpdate(int idx, ChecklistOperation operation) {
    if (!s_screen_alive || idx < 0 || idx >= static_cast<int>(s_items.size())) {
        return;
    }
    if (AnyWorkerBusy()) {
        SetStatusText(LangString_TASK_PLEASE_WAIT, true);
        return;
    }
    bool accepted = false;
    try {
        accepted = SubmitChecklist(operation, {s_items[static_cast<size_t>(idx)].id});
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "operation=submit stage=allocate result=no_memory");
    }
    const bool remove = operation == ChecklistOperation::Delete;
    SetStatusText(accepted ? (remove ? LangString_TASK_DELETING : LangString_TASK_COMPLETING) :
                            (remove ? LangString_TASK_DELETE_START_FAIL : LangString_TASK_COMPLETE_START_FAIL), !accepted);
}

static void ScheduleBatch(ChecklistOperation operation) {
    if (!s_screen_alive || !s_multi) {
        return;
    }
    if (AnyWorkerBusy()) {
        SetStatusText(LangString_TASK_PLEASE_WAIT, true);
        return;
    }
    const bool complete = operation == ChecklistOperation::Complete;
    bool accepted = false;
    try {
        std::vector<std::string> ids;
        SyncSelectedSize();
        for (size_t i = 0; i < s_items.size() && i < s_selected.size(); ++i) {
            if (s_selected[i] == 0 || (complete && ItemIsDone(s_items[i]))) {
                continue;
            }
            ids.emplace_back(s_items[i].id);
        }
        if (ids.empty()) {
            SetStatusText(complete ? LangString_TASK_NO_COMPLETABLE : LangString_TASK_SELECT_FIRST, true);
            return;
        }
        accepted = SubmitChecklist(operation, std::move(ids), true);
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "operation=submit_batch stage=allocate result=no_memory");
    }
    SetStatusText(accepted ? (complete ? LangString_TASK_BATCH_COMPLETING : LangString_TASK_BATCH_REMOVING) :
                            LangString_TASK_BATCH_START_FAIL, !accepted);
}

static void OnMultiCancel(lv_event_t* /*e*/) {
    if (AnyWorkerBusy()) {
        return;
    }
    ExitMultiMode(true);
}

static void OnMultiSelectAll(lv_event_t* /*e*/) {
    if (AnyWorkerBusy() || !s_multi) {
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

static void OnMultiDelete(lv_event_t* /*e*/) {
    if (AnyWorkerBusy() || !s_multi) {
        return;
    }
    std::vector<std::string> ids;
    SyncSelectedSize();
    for (size_t i = 0; i < s_items.size() && i < s_selected.size(); ++i) {
        if (s_selected[i] != 0) {
            ids.push_back(s_items[i].id);
        }
    }
    if (ids.empty()) {
        SetStatusText(LangString_TASK_SELECT_FIRST, true);
        return;
    }
    ScreenConfirmDelete(s_ui.screen, [ids = std::move(ids)]() mutable {
        if (!s_screen_alive || AnyWorkerBusy()) {
            return;
        }
        const bool accepted = SubmitChecklist(ChecklistOperation::Delete, std::move(ids), true);
        SetStatusText(accepted ? LangString_TASK_BATCH_REMOVING : LangString_TASK_BATCH_START_FAIL, !accepted);
    });
}

static void OnMultiComplete(lv_event_t* /*e*/) {
    if (s_tab == 1) {
        return;
    }
    ScheduleBatch(ChecklistOperation::Complete);
}

static void OnSyncClicked(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_dialog_open) {
        return;
    }
    if (AnyWorkerBusy()) {
        SetStatusText(LangString_TASK_PLEASE_WAIT, true);
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (s_last_user_sync_us != 0 && (now - s_last_user_sync_us) < kSyncThrottleUs) {
        SetStatusText(LangString_TASK_PLEASE_WAIT, true);
        return;
    }
    if (Board::GetInstance().IsWifiConfigMode()) {
        SetStatusText(LangString_TASK_NEED_WIFI_CFG, true);
        return;
    }
    s_last_user_sync_us = now;
    SetStatusText(Board::GetInstance().IsNetworkReady() ? LangString_TASK_REFRESHING : LangString_TASK_CONNECTING_NET);
    if (!SubmitChecklist(ChecklistOperation::Refresh)) {
        SetStatusText(LangString_TASK_REFRESH_START_FAIL, true);
    }
}

static lv_obj_t* MakeDialogBtn(lv_obj_t* parent, const char* text, bool filled, lv_event_cb_t cb) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, kActionH);
    lv_obj_set_style_radius(btn, kRowRadius, 0);
    lv_obj_set_style_border_width(btn, kBorderW, 0);
    lv_obj_set_style_border_color(btn, UiColorForeground(), 0);
    if (filled) {
        lv_obj_set_style_bg_color(btn, UiColorForeground(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_bg_color(btn, UiColorBackground(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    }
    DisableScroll(btn);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, ChecklistFont(), 0);
    lv_obj_set_style_text_color(lbl, filled ? UiColorBackground() : UiColorForeground(), 0);
    lv_obj_set_style_pad_ver(btn, UiSy(6), 0);
    ScreenLayoutTextButton(btn, lbl, kActionH);
    return btn;
}

static void OnDialogBackdropClicked(lv_event_t* e) {
    // 仅点遮罩本身关闭；点到卡片/按钮时 target ≠ current_target
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) {
        return;
    }
    HideActionDialog();
}

static void EnsureActionDialog() {
    if (s_ui.dialog != nullptr || s_ui.screen == nullptr) {
        return;
    }

    s_ui.dialog = lv_obj_create(s_ui.screen);
    s_window_modal.Bind(s_ui.dialog, [](void*) { HideActionDialog(); });
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
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, kDialogRowGap, 0);
    ScreenStyleDialogCard(card, kDialogPad);
    // 吸收点击，避免点卡片空白处落到遮罩上误关
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, [](lv_event_t* e) { lv_event_stop_bubbling(e); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_center(card);

    s_ui.dialog_msg = lv_label_create(card);
    lv_label_set_text(s_ui.dialog_msg, LangString_TASK_CHOOSE_ACTION);
    lv_obj_set_width(s_ui.dialog_msg, LV_HOR_RES - kPad * 2 - kDialogMsgInset);
    lv_obj_set_height(s_ui.dialog_msg, LV_SIZE_CONTENT);
    lv_label_set_long_mode(s_ui.dialog_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_ui.dialog_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_ui.dialog_msg, ChecklistFont(), 0);
    lv_obj_set_style_text_color(s_ui.dialog_msg, UiColorForeground(), 0);
    lv_obj_clear_flag(s_ui.dialog_msg, LV_OBJ_FLAG_CLICKABLE);

    // 一行一个：完成（进行中才显示）/ 删除；无返回按钮，点遮罩关闭
    s_ui.dialog_complete_btn = MakeDialogBtn(card, LangString_TASK_COMPLETE_TODO, true, OnDialogComplete);
    MakeDialogBtn(card, LangString_TASK_DELETE_TODO, false, OnDialogDelete);
}

static void StyleTabBtn(lv_obj_t* btn, lv_obj_t* lbl, bool selected) {
    // 选中段本身是完整胶囊（两侧圆弧），中间靠圆弧衔接而非竖线
    lv_obj_set_style_bg_color(btn, selected ? UiColorForeground() : UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    if (lbl != nullptr) {
        lv_obj_set_style_text_color(lbl, selected ? UiColorBackground() : UiColorForeground(), 0);
    }
}

static void RefreshTabUi() {
    for (int i = 0; i < kTabCount; ++i) {
        StyleTabBtn(s_ui.tab_btns[i], s_ui.tab_lbls[i], s_tab == i);
    }
    if (s_ui.section_title != nullptr) {
        lv_label_set_text(s_ui.section_title, s_tab == 1 ? LangString_TASK_TITLE_DONE : LangString_TASK_TITLE_TODO);
    }
}

static void OnTabClicked(lv_event_t* e) {
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

static lv_obj_t* MakeTabBtn(lv_obj_t* parent, const char* text, int tab) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, lv_pct(100));
    lv_obj_set_style_pad_all(btn, 0, 0);
    DisableScroll(btn);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, OnTabClicked, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(tab)));

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, ChecklistFont(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    s_ui.tab_btns[tab] = btn;
    s_ui.tab_lbls[tab] = lbl;
    return btn;
}

static bool OnVkKey(const char* key);  // 前置

static ScreenPaintCoalesce s_task_paint{};
static ScreenPaintCoalesce s_task_check_paint{};

static lv_obj_t* FindListRow(int index) {
    if (s_ui.list_host == nullptr || !lv_obj_is_valid(s_ui.list_host)) {
        return nullptr;
    }
    const uint32_t n = lv_obj_get_child_count(s_ui.list_host);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* row = lv_obj_get_child(s_ui.list_host, i);
        if (row != nullptr && static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(row))) == index) {
            return row;
        }
    }
    return nullptr;
}

static lv_obj_t* FindRowChildBySize(lv_obj_t* row, lv_coord_t w, lv_coord_t h) {
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

static void PatchRowCheckMark(int index) {
    if (!s_multi) {
        return;
    }
    lv_obj_t* row = FindListRow(index);
    lv_obj_t* check = FindRowChildBySize(row, kCheckSize, kCheckSize);
    if (check == nullptr) {
        RequestRebuildListPage();  // 勾选列尚未建好（进多选 coalesce 未落地）
        return;
    }
    lv_obj_clean(check);
    if (!ItemSelected(index)) {
        return;
    }
    lv_obj_t* mark = lv_label_create(check);
    lv_label_set_text(mark, "√");
    lv_obj_set_style_text_font(mark, ChecklistFont(), 0);
    lv_obj_set_style_text_color(mark, UiColorForeground(), 0);
    lv_obj_center(mark);
    lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
}

static void PatchVisibleRowCheckMarks() {
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
    std::snprintf(buf, sizeof(buf), LangString_TASK_SELECTED_FMT, SelectedCount());
    SetStatusText(buf);
}

static void RequestRebuildListPage() {
    if (s_task_paint.paint == nullptr) {
        s_task_paint.paint = RebuildListPage;
    }
    ScreenPaintCoalesceRequest(&s_task_paint);
}

static void RequestCheckMarksPaint() {
    if (s_task_check_paint.paint == nullptr) {
        s_task_check_paint.paint = PatchVisibleRowCheckMarks;
    }
    // 点间隙手指会抬起：勿立刻上屏，停手后再一次刷全部 √
    ScreenPaintCoalesceRequestDebounced(&s_task_check_paint, 280000);
}

static bool TaskPageRepeatStep(int page_delta) {
    if (page_delta == 0 || s_dialog_open) {
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

static bool OnPageKey(const char* key, bool long_press) {
    if (long_press) {
        VkPageRepeatTryStart(key, TaskPageRepeatStep);
    } else {
        TaskPageRepeatStep(std::strcmp(key, "vk_prev") == 0 ? -1 : 1);
    }
    return true;
}

static bool OnVkKeyLongPress(const char* key) {
    return VkPageRepeatTryStart(key, TaskPageRepeatStep);
}

static bool OnVkKeyPressUp(const char* key) {
    return VkPageRepeatOnPressUp(key);
}

static bool OnVkKey(const char* key) {
    if (key == nullptr) {
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
            if (AnyWorkerBusy()) {
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

static void OnScreenUnloaded(lv_event_t* e) {
    lv_obj_t* scr = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (scr != s_ui.screen) {
        return;
    }
    StopChecklistUi();
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
    s_dialog_open = false;
}

static void OnScreenLoaded(lv_event_t* e) {
    lv_obj_t* scr = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (scr != s_ui.screen || !s_screen_alive) {
        return;
    }
    // 只读 cache；无缓存显示空态，不在本页主动 GET
    ApplyFromCache();
}

lv_obj_t* ChecklistScreen::Create() {
    StopChecklistUi();
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
    s_dialog_open = false;
    s_status_text[0] = '\0';
    s_empty_override[0] = '\0';
    s_last_user_sync_us = 0;
    s_cache_retry_at = 0;

    ScreenSetIsHome(false);

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    lv_obj_set_style_bg_color(scr, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, ChecklistFont(), 0);
    lv_obj_set_style_text_color(scr, UiColorForeground(), 0);
    lv_obj_add_event_cb(scr, OnScreenLoaded, LV_EVENT_SCREEN_LOADED, nullptr);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    const lv_coord_t body_h = LV_VER_RES - status.height;

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, body_h);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(body, kPad, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(body, kRowGap, 0);
    DisableScroll(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* tab_row = lv_obj_create(body);
    lv_obj_remove_style_all(tab_row);
    lv_obj_set_width(tab_row, kTabW);
    lv_obj_set_height(tab_row, std::max<int>(kTabH, ScreenTextHeight(ChecklistFont()) + UiSy(16)));
    lv_obj_set_style_bg_color(tab_row, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(tab_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(tab_row, UiColorForeground(), 0);
    lv_obj_set_style_border_width(tab_row, kTabBorderW, 0);
    lv_obj_set_style_radius(tab_row, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(tab_row, true, 0);
    lv_obj_set_flex_flow(tab_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tab_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(tab_row, kTabInset, 0);
    lv_obj_set_style_pad_column(tab_row, 0, 0);
    DisableScroll(tab_row);
    lv_obj_clear_flag(tab_row, LV_OBJ_FLAG_CLICKABLE);

    MakeTabBtn(tab_row, LangString_TASK_TAB_TODO, 0);
    MakeTabBtn(tab_row, LangString_TASK_TAB_DONE, 1);
    RefreshTabUi();

    lv_obj_t* header_row = lv_obj_create(body);
    lv_obj_remove_style_all(header_row);
    lv_obj_set_width(header_row, lv_pct(100));
    lv_obj_set_height(header_row, std::max<int>(kHeaderH, ChecklistTitleFont()->line_height));
    lv_obj_set_style_bg_opa(header_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(header_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header_row, kHeaderGap, 0);
    DisableScroll(header_row);
    lv_obj_clear_flag(header_row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* star_wrap = lv_obj_create(header_row);
    lv_obj_remove_style_all(star_wrap);
    lv_obj_set_size(star_wrap, kHeaderIcon, kHeaderIcon);
    lv_obj_set_style_bg_color(star_wrap, UiColorForeground(), 0);
    lv_obj_set_style_bg_opa(star_wrap, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(star_wrap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(star_wrap, 0, 0);
    DisableScroll(star_wrap);
    lv_obj_clear_flag(star_wrap, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* star = lv_label_create(star_wrap);
    lv_label_set_text(star, "\xE2\x98\x85");  // ★
    lv_obj_set_style_text_color(star, UiColorBackground(), 0);
    lv_obj_set_style_text_font(star, ChecklistFont(), 0);
    lv_obj_center(star);
    lv_obj_clear_flag(star, LV_OBJ_FLAG_CLICKABLE);

    s_ui.section_title = lv_label_create(header_row);
    lv_label_set_text(s_ui.section_title, LangString_TASK_TITLE_TODO);
    lv_obj_set_style_text_font(s_ui.section_title, ChecklistTitleFont(), 0);
    lv_obj_set_style_text_color(s_ui.section_title, UiColorForeground(), 0);
    lv_obj_clear_flag(s_ui.section_title, LV_OBJ_FLAG_CLICKABLE);

    // 与「日程待办 / 已完成」同行稍后：完成中、已删除等
    s_ui.status_lbl = lv_label_create(header_row);
    lv_label_set_text(s_ui.status_lbl, "");
    lv_obj_set_style_pad_left(s_ui.status_lbl, kStatusGap, 0);
    lv_obj_set_style_text_font(s_ui.status_lbl, ChecklistFont(), 0);
    lv_obj_set_style_text_color(s_ui.status_lbl, UiColorForeground(), 0);
    lv_obj_clear_flag(s_ui.status_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_ui.status_lbl, LV_OBJ_FLAG_HIDDEN);

    // 外框：包住待办列表 + 同步/批量操作；页码在框外（同传输页）
    s_ui.list_frame = lv_obj_create(body);
    lv_obj_remove_style_all(s_ui.list_frame);
    lv_obj_set_width(s_ui.list_frame, LV_HOR_RES - kPad * 2);
    lv_obj_set_flex_grow(s_ui.list_frame, 1);
    lv_obj_set_style_bg_color(s_ui.list_frame, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(s_ui.list_frame, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ui.list_frame, kListFrameBorderW, 0);
    lv_obj_set_style_border_color(s_ui.list_frame, UiColorForeground(), 0);
    lv_obj_set_style_radius(s_ui.list_frame, kListFrameRadius, 0);
    lv_obj_set_style_pad_all(s_ui.list_frame, kListFramePad, 0);
    lv_obj_set_flex_flow(s_ui.list_frame, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.list_frame, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_ui.list_frame, 0, 0);
    DisableScroll(s_ui.list_frame);
    lv_obj_clear_flag(s_ui.list_frame, LV_OBJ_FLAG_CLICKABLE);

    s_ui.list_host = lv_obj_create(s_ui.list_frame);
    lv_obj_remove_style_all(s_ui.list_host);
    lv_obj_set_width(s_ui.list_host, lv_pct(100));
    lv_obj_set_height(s_ui.list_host, 0);
    lv_obj_set_flex_grow(s_ui.list_host, 1);
    lv_obj_set_flex_flow(s_ui.list_host, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.list_host, kRowGap, 0);
    DisableScroll(s_ui.list_host);

    s_ui.footer = lv_obj_create(s_ui.list_frame);
    lv_obj_remove_style_all(s_ui.footer);
    lv_obj_set_width(s_ui.footer, lv_pct(100));
    lv_obj_set_height(s_ui.footer, ChecklistSyncRowHeight());
    lv_obj_set_style_bg_opa(s_ui.footer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(s_ui.footer, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(s_ui.footer, kSyncDividerW, 0);
    lv_obj_set_style_border_color(s_ui.footer, UiColorForeground(), 0);
    lv_obj_set_flex_flow(s_ui.footer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.footer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    DisableScroll(s_ui.footer);
    lv_obj_clear_flag(s_ui.footer, LV_OBJ_FLAG_CLICKABLE);

    // 分割线 + 居中「刷新」下划线（多选时隐藏，换 multi_bar）
    s_ui.sync_btn = lv_obj_create(s_ui.footer);
    lv_obj_remove_style_all(s_ui.sync_btn);
    lv_obj_set_width(s_ui.sync_btn, lv_pct(100));
    lv_obj_set_height(s_ui.sync_btn, lv_pct(100));
    lv_obj_set_style_bg_opa(s_ui.sync_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_ui.sync_btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.sync_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    DisableScroll(s_ui.sync_btn);
    lv_obj_add_flag(s_ui.sync_btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(s_ui.sync_btn);
    lv_obj_add_event_cb(s_ui.sync_btn, OnSyncClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* sync_text = lv_obj_create(s_ui.sync_btn);
    lv_obj_remove_style_all(sync_text);
    lv_obj_set_size(sync_text, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_border_side(sync_text, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(sync_text, kSyncUnderlineH, 0);
    lv_obj_set_style_border_color(sync_text, UiColorForeground(), 0);
    lv_obj_set_style_pad_bottom(sync_text, kSyncUnderlinePadBottom, 0);
    lv_obj_set_style_pad_hor(sync_text, kSyncUnderlinePadHor, 0);
    lv_obj_set_style_bg_opa(sync_text, LV_OPA_TRANSP, 0);
    DisableScroll(sync_text);
    lv_obj_clear_flag(sync_text, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* sync_lbl = lv_label_create(sync_text);
    lv_label_set_text(sync_lbl, LangString_TASK_REFRESH);
    lv_obj_set_style_text_font(sync_lbl, ChecklistFont(), 0);
    lv_obj_set_style_text_color(sync_lbl, UiColorForeground(), 0);
    lv_obj_clear_flag(sync_lbl, LV_OBJ_FLAG_CLICKABLE);

    s_ui.multi_bar = lv_obj_create(s_ui.footer);
    lv_obj_remove_style_all(s_ui.multi_bar);
    lv_obj_set_width(s_ui.multi_bar, lv_pct(100));
    lv_obj_set_height(s_ui.multi_bar, lv_pct(100));
    lv_obj_set_style_bg_opa(s_ui.multi_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_ui.multi_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ui.multi_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_ui.multi_bar, kMultiBarGap, 0);
    DisableScroll(s_ui.multi_bar);
    lv_obj_clear_flag(s_ui.multi_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_ui.multi_bar, LV_OBJ_FLAG_HIDDEN);

    // 与「刷新」同款：下划线文字；多项间居中「·」
    auto make_multi_action = [](lv_obj_t* parent, const char* text, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_obj_create(parent);
        lv_obj_remove_style_all(btn);
        lv_obj_set_height(btn, lv_pct(100));
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
        lv_obj_set_style_border_color(text_wrap, UiColorForeground(), 0);
        lv_obj_set_style_pad_bottom(text_wrap, kSyncUnderlinePadBottom, 0);
        lv_obj_set_style_pad_hor(text_wrap, kSyncUnderlinePadHor, 0);
        lv_obj_set_style_bg_opa(text_wrap, LV_OPA_TRANSP, 0);
        DisableScroll(text_wrap);
        lv_obj_clear_flag(text_wrap, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* lbl = lv_label_create(text_wrap);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, ChecklistFont(), 0);
        lv_obj_set_style_text_color(lbl, UiColorForeground(), 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        return btn;
    };
    auto make_multi_dot = [](lv_obj_t* parent) {
        lv_obj_t* dot = lv_obj_create(parent);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, kMultiDotSize, kMultiDotSize);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, UiColorForeground(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        DisableScroll(dot);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        return dot;
    };
    make_multi_action(s_ui.multi_bar, LangString_COMMON_CANCEL, OnMultiCancel);
    make_multi_dot(s_ui.multi_bar);
    make_multi_action(s_ui.multi_bar, LangString_COMMON_SELECT_ALL, OnMultiSelectAll);
    make_multi_dot(s_ui.multi_bar);
    make_multi_action(s_ui.multi_bar, LangString_COMMON_REMOVE, OnMultiDelete);
    s_ui.multi_complete_sep = make_multi_dot(s_ui.multi_bar);
    s_ui.multi_complete_btn = make_multi_action(s_ui.multi_bar, LangString_TASK_COMPLETE, OnMultiComplete);

    s_ui.page_lbl = lv_label_create(body);
    lv_label_set_text(s_ui.page_lbl, "0 / 0");
    lv_obj_set_width(s_ui.page_lbl, LV_HOR_RES - kPad * 2);
    lv_obj_set_height(s_ui.page_lbl, kFooterH);
    lv_obj_set_style_text_align(s_ui.page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_ui.page_lbl, ChecklistFont(), 0);
    lv_obj_clear_flag(s_ui.page_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_update_layout(scr);
    s_list_per_page =
        std::max<int>(1, (lv_obj_get_content_height(s_ui.list_host) + kRowGap) / (EstimatedTwoLineRowH() + kRowGap));
    VkKey_AttachScreen(scr, kScreenId,
                       VkKeyScreenDesc{ChecklistScreen::Create, OnVkKey, nullptr, nullptr, nullptr, nullptr,
                                       OnVkKeyLongPress, OnVkKeyPressUp, nullptr, nullptr, OnPageKey});
    s_page.Bind(scr, {RefreshChecklistUi, [](UiPagePowerEvent event) {
        if (event == UiPagePowerEvent::PauseViews || event == UiPagePowerEvent::ResumeViews) {
            ChecklistScreen::SetStandby(event == UiPagePowerEvent::PauseViews);
        }
    }, StopChecklistUi, 100});
    s_page_generation = s_page.Generation();
    return scr;
}

void ChecklistScreen::SetStandby(bool standby) {
    if (!s_screen_alive || s_ui.screen == nullptr) {
        return;
    }
    if (standby) {
        s_request.reset();
    } else {
        ApplyFromCache();
    }
}
