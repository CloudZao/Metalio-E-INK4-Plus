#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>
#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>
#include "assets/lang_config.h"
#include "business/cloud/push_resources_library.h"
#include "business/reader/reader.h"
#include "business/reader/text_encoding.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/widgets/screen_text.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/drawing/rendering/epd_content_policy.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"
#include "assets/lang_screen.h"
#include "ui/common/drawing/adapter/epd_refresh.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/widgets/screen_options.h"
#include "ui/common/runtime/modal.h"
#include "business/cloud/cloud_resource_service.h"
#include "business/yunxin_binding/yunxin_binding.h"
#include "infra/image/image_decode.h"
#include "wifi_station.h"
#include "assets/lang_network.h"
#include "boards/common/board/board.h"
#include "boards/common/board/dual_network_board.h"
#include "business/transfer/ap_transfer.h"
#include "business/transfer/transfer_session.h"
#include "ui/common/widgets/binding_panel.h"
#include "ui/common/drawing/ui_profile.h"
#include "ui/common/widgets/ui_qrcode.h"
#include "ui/apps/settings/network/network_screen.h"
#include "ui/apps/cloud/usb/cloud_usb_panel.h"
#include "sysmgmt/system_control.h"
#include "sysmgmt/tasks/task_executor.h"
#include "ui/common/runtime/page_lifecycle.h"
#include "ui/apps/cloud/cloud_screen.h"
#include "ui/apps/cloud/cloud_preview.h"
#include "ui/common/drawing/display.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/runtime/input/vk_page_repeat.h"
#include "ui/common/runtime/screen_common_nav.h"
#include "infra/storage/sd_paths.h"
#include "business/reader/reader_types.h"
#include "infra/network/http/download_gate.h"
#include "infra/network/http/http_download_file.h"
#include "ui/common/drawing/ui_scale.h"

static constexpr const char* TAG = "CloudScreen";
static constexpr const char* kScreenId = "cloud";

// 源值对齐 397 cloud_screen；经 UiSx/UiSy 同比例到 470
static constexpr lv_coord_t kPad = UiSx(12);
static constexpr lv_coord_t kFooterH = UiSy(36);
static constexpr lv_coord_t kCloudRowH = UiSy(80);
static constexpr lv_coord_t kCloudRowLineGap = UiSy(6);
static constexpr lv_coord_t kRowGap = UiSy(8);
static constexpr lv_coord_t kThumbW = UiSx(48);
static constexpr lv_coord_t kThumbH = UiSy(64);
static constexpr lv_coord_t kThumbGap = UiSx(10);
static constexpr int kThumbDecodeW = UiSx(48);
static constexpr int kThumbDecodeH = UiSy(64);
static constexpr lv_coord_t kRowPad = UiSx(10);
static constexpr lv_coord_t kRowBorderW = UiSx(2);
static constexpr lv_coord_t kRowRadius = UiSx(8);
static constexpr int64_t kSyncThrottleUs = 3000000;  // 手动刷新最短间隔 3s
static constexpr lv_coord_t kSyncBtnH = UiSy(44);
static constexpr lv_coord_t kSyncDividerW = 2;               // 底栏顶部分割线
static constexpr lv_coord_t kSyncUnderlineH = 2;             // 「刷新」/多选动作下划线
static constexpr lv_coord_t kSyncUnderlinePadHor = UiSx(5);  // 下划线两端各外延；共比字宽加长 10px
static constexpr lv_coord_t kSyncUnderlinePadBottom = UiSy(2);
static constexpr lv_coord_t kMultiDotSize = UiSx(6);  // 分隔圆点直径（约 · 的 2 倍）
static constexpr lv_coord_t kMultiBarGap = UiSx(10);
static constexpr lv_coord_t kMultiActionPadHor = UiSx(4);
static constexpr lv_coord_t kListFrameBorderW = UiSx(2);  // 列表+刷新细外框（不含页码）
static constexpr lv_coord_t kListFrameRadius = UiSx(10);
static constexpr lv_coord_t kListFramePad = UiSx(10);
static constexpr lv_coord_t kTabH = UiSy(48);
static constexpr lv_coord_t kTabBorderW = UiSx(3);
static constexpr lv_coord_t kTabInset = UiSx(3);
static constexpr lv_coord_t kHeaderH = UiSy(36);
static constexpr lv_coord_t kHeaderIcon = UiSx(32);
static constexpr lv_coord_t kHeaderGap = UiSx(8);
static constexpr lv_coord_t kStatusGap = UiSx(12);  // 标题与拉取提示间距（同每日清单）
static constexpr int kTabCount = 4;
static constexpr int kFilterTabCount = 4;  // APP 推送框内：全部 / 壁纸 / 书籍 / 字体
static constexpr lv_coord_t kFilterH = UiSy(40);
static constexpr lv_coord_t kPanelGap = UiSy(12);
static constexpr lv_coord_t kPageLblMargin = UiSx(16);
static constexpr lv_coord_t kPageLblBottom = UiSy(4);
static constexpr lv_coord_t kFooterOutsideH = kFooterH;
static constexpr uint32_t kStatusClearMs = 1200;
static constexpr lv_coord_t kCheckSize = UiSx(28);  // 多选行尾勾选（同清单）
static constexpr lv_coord_t kCheckGap = UiSx(10);
static constexpr lv_coord_t kDialogCardMargin = UiSx(80);
static constexpr lv_coord_t kDialogCardPad = UiSx(18);
static constexpr lv_coord_t kDialogCardPadSmall = UiSx(14);
static constexpr lv_coord_t kDialogRowGap = UiSy(14);
static constexpr lv_coord_t kDialogRowGapSmall = UiSy(20);
static constexpr lv_coord_t kDlBarH = UiSy(28);
static constexpr lv_coord_t kDlBarRadius = UiSx(6);
static constexpr lv_coord_t kDlBarIndRadius = UiSx(4);
static constexpr lv_coord_t kCancelBtnW = UiSx(160);
static constexpr lv_coord_t kCancelBtnH = UiSy(44);
static constexpr lv_coord_t kActionBtnRowH = UiSy(48);
static constexpr lv_coord_t kActionBtnW = UiSx(120);
static constexpr lv_coord_t kActionBtnH = UiSy(44);
static constexpr int64_t kSuppressRowClickUs = 400000;       // 只挡长按行松手假点击，不挡其它行连点
static constexpr int64_t kDlProgUiIntervalUs = 3000 * 1000;  // 进度页约 3s 刷一次

enum CloudXferMode : int {
    kCloudModeApp = 0,   // 云端待收，框内再按类型筛选
    kCloudModeAp = 1,    // 开放热点文件页
    kCloudModeUsb = 2,   // 模拟 U 盘
    kCloudModeWifi = 3,  // 局域网文件传输
};

struct CloudPageState {
    CloudImageRequestPtr covers_request;
    uint32_t covers_generation = 0;
    CloudResourceRequestPtr request; // 当前页面的结果接收，不持有执行状态
    uint32_t request_generation = 0;
    size_t shown_download_index = SIZE_MAX;
    int64_t download_painted_us = 0;
    struct Chrome {
        lv_obj_t* screen = nullptr;
        lv_obj_t* main_body = nullptr;  // 列表区；预览时隐藏（页码贴底，不在此列）
        lv_obj_t* list_body = nullptr;
        lv_obj_t* footer = nullptr;  // 框内刷新/多选底栏
        lv_obj_t* sync_btn = nullptr;
        lv_obj_t* sync_lbl = nullptr;
        lv_obj_t* multi_bar = nullptr;         // 取消 / 全选 / 删除 / 保存
        lv_obj_t* page_lbl = nullptr;          // 贴底，对齐壁纸页码
        lv_obj_t* radio_status_lbl = nullptr;  // 贴底：AP打开中 / WiFi连接中 / 失败原因
        lv_obj_t* tab_btns[kTabCount] = {};
        lv_obj_t* tab_lbls[kTabCount] = {};
        lv_obj_t* filter_row = nullptr;  // APP 推送框内类型筛选
        lv_obj_t* filter_btns[kFilterTabCount] = {};
        lv_obj_t* filter_lbls[kFilterTabCount] = {};
        lv_obj_t* header_row = nullptr;
        lv_obj_t* list_frame = nullptr;
        lv_obj_t* binding_panel = nullptr;  // App 推送内容区的扫码绑定面板
        lv_obj_t* section_title = nullptr;
        lv_obj_t* status_lbl = nullptr;
        lv_obj_t* wifi_panel = nullptr;  // WIFI 列表、密码与连接信息共用内容区
        lv_obj_t* ap_panel = nullptr;
        lv_obj_t* ap_ssid_lbl = nullptr;
        lv_obj_t* ap_url_lbl = nullptr;
        lv_obj_t* ap_status_lbl = nullptr;
        lv_obj_t* ap_qr_wifi = nullptr;
        lv_obj_t* ap_qr_url = nullptr;
        lv_obj_t* usb_panel = nullptr;
    } chrome;

    struct Data {
        std::vector<CloudPushResource> items;
        std::vector<int> filtered;                // items 下标，按 Tab 筛选
        std::vector<uint8_t> selected;            // 与 items 等长；1=多选选中
        std::vector<std::unique_ptr<RasterImage>> thumbs;
        std::vector<std::shared_ptr<HttpTempFile>> cover_cache;  // 与 items 等长的磁盘封面缓存
        uint32_t thumb_epoch = 0;
        int mode = kCloudModeAp;  // CloudXferMode
        int filter_tab = 0;       // APP 推送：0全部 1壁纸 2书籍 3字体
        int page = 0;
        int page_size = 6;
        bool loading = false;
        bool waiting_net = false;
        bool fetch_done = false;
        bool covers_loading = false;  // 本批封面缓存与缩略图就绪后渲染列表行
        bool multi = false;
        bool batch_busy = false;
        bool delete_busy = false;  // 多选 DELETE push-resources 进行中
        int64_t suppress_row_click_until_us = 0;
        int suppress_row_click_idx = -1;  // 与 until 配对：只吞该行松手 CLICKED
        std::string error;
        char status_text[80] = {};
    } data;

    struct Xfer {
        bool download_busy = false;
        uint32_t op_generation = 1;
        int pending_index = -1;
        int action_index = -1;
        // 下载独占进度页（网点底）：单文件/批量共用
        lv_obj_t* dl_page = nullptr;
        lv_obj_t* dl_bar = nullptr;
        lv_obj_t* dl_pct_lbl = nullptr;
        lv_obj_t* dl_count_lbl = nullptr;
        lv_obj_t* dl_title_lbl = nullptr;
        bool dl_page_open = false;
        int dl_job_total = 0;
        int dl_job_done = 0;
        int dl_shown_percent = -1;
    } xfer;

    struct Dialogs {
        lv_obj_t* storage_error_mask = nullptr;  // 内存卡故障重启弹窗
        lv_obj_t* dialog_mask = nullptr;
        lv_obj_t* action_mask = nullptr;
    } dialogs;
};

static CloudPageState s_cloud;
static CloudPreview s_preview;
static lv_timer_t* s_status_clear_timer = nullptr;
static int64_t s_last_user_sync_us = 0;
static ScreenPaintCoalesce s_cloud_paint{};
static ScreenPaintCoalesce s_cloud_check_paint{};

static void ScheduleUserCloudSync();
static void OnTabClicked(lv_event_t* e);
static lv_obj_t* MakeTabBtn(lv_obj_t* parent, const char* text, int tab);
static void OnSyncClicked(lv_event_t* e);
static void BuildCloudMainChrome(lv_obj_t* scr, lv_coord_t status_height);
static void EnterCloudMode(int mode);
static void CloudRadioAbandon();
static void ParkApForStandby();
static void ReloadApAfterStandby();
static void PollCloudTransferSession();
static void CloseDialog();
static void CloseActionDialog();
static void ShowSaveConflictDialog(int cloud_index);
static void ScheduleDeleteItems(std::vector<CloudPushResource> items);
static void Cloud_OnDeleteClicked(lv_event_t* e);
static void ShowStorageErrorDialog();
static void ApplyCloudDeleteResult(const CloudResourceRequest& result);
static void ScheduleCloudFetch();
static void ApplyCloudFetchResult(CloudResourceRequest& result);
static void Cloud_OnRowLongPressed(lv_event_t* e);
static void Cloud_OnRowClicked(lv_event_t* e);
static void OnMultiCancel(lv_event_t* e);
static void OnMultiSelectAll(lv_event_t* e);
static void OnMultiSave(lv_event_t* e);
static void OnMultiDelete(lv_event_t* e);
static lv_obj_t* FindListRow(int index);
static lv_obj_t* FindRowChildBySize(lv_obj_t* row, lv_coord_t w, lv_coord_t h);
static void PatchRowCheckMark(int index);
static void PatchVisibleRowCheckMarks();
static lv_obj_t* CreateResourceRow(lv_obj_t* parent, const CloudPushResource& item, int index);
static void RenderListPageInternal(bool schedule_thumbs);
static void RenderListPage();
static void RequestCloudRender();
static void RequestCheckMarksPaint();
static void ScheduleCoverThumbFill();
static void InvalidateSession();
static void ApplyStopTransferUi();
static void RequestStopTransfer(const char* why);
static bool Cloud_OnVkKey(const char* key);
static bool Cloud_OnVkKeyLongPress(const char* key);
static bool Cloud_OnVkKeyPressUp(const char* key);
static void Cloud_OnScreenDeleted(lv_event_t* e);
static void Cloud_RebootAfterStorageError();
static void CancelCloudImageViews();
static void PollCloudImages();
static int CloudRowHeight();
static lv_coord_t Cloud_ContentWidth();
static void Cloud_DisableScroll(lv_obj_t* obj);
static void Cloud_ShowMessage(lv_obj_t* parent, const char* msg);
static void CancelStatusClearTimer();
static void ApplyStatusTipUi();
static void SetStatusTip(const char* text, bool auto_clear);
static void RebuildFiltered();
static void RefreshSectionTitle();
static void RefreshFilterUi();
static void StyleTabBtn(lv_obj_t* btn, lv_obj_t* lbl, bool on);
static void RefreshTabUi();
static void SyncSelectedSize();
static int SelectedCount();
static bool ItemSelected(int idx);
static void ToggleItemSelected(int idx);
static void RefreshFooterMode();
static void ExitMultiModeEx(bool rebuild, bool clear_status);
static void ExitMultiMode(bool rebuild);
static void EnterMultiModeSelect(int idx);
static bool ScreenAlive();
static bool OpStillValid(uint32_t op_gen);
static bool SyncBusy();
static int PageCount();
static void ClampPage();
static void SetPageFooterText(const char* text);
static void EnsureDlProgressPageBuilt();
static void RefreshDlProgressPage(int file_percent);
static void ShowDlProgressPage();
static void HideDlProgressPage();
static void AbortDownloadJobFromUi();
static bool CloudDlTasksActive();
static void AbortCloudSideHttpForDownload();
static void ResumeCloudSideHttpAfterDl();
static void StartResourceDownload(int index, CloudSaveChoice choice = CloudSaveChoice::kAsk);
static void StartSelectedResourceDownloads();
static void RefreshCloudDownload();
static void ApplyCloudDownloadResult(const CloudResourceRequest& result);
static int FindItemIndexByTaskId(const char* key);
static void EraseItemAt(int index);
static void ShowWallpaperPreviewMode(bool preview);
static void CloseWallpaperPreview();
static void OpenWallpaperPreview(int index);
static void StartWallpaperDownload(int index);

static UiModal s_download_modal;
static UiModal s_resource_modal;
static UiModal s_storage_modal;

static void StatusClearTimerCb(lv_timer_t* /*t*/) {
    s_status_clear_timer = nullptr;
    auto& st = s_cloud;
    st.data.status_text[0] = '\0';
    if (st.chrome.status_lbl != nullptr) {
        lv_label_set_text(st.chrome.status_lbl, "");
        lv_obj_add_flag(st.chrome.status_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool ItemMatchesTab(const CloudPushResource& item, int tab) {
    if (tab <= 0) {
        return true;
    }
    if (tab == 1) {
        return item.type == PushResourceType::kBadge;
    }
    if (tab == 2) {
        return item.type == PushResourceType::kBook;
    }
    if (tab == 3) {
        return item.type == PushResourceType::kFont;
    }
    return true;
}

static void StyleFilterBtn(lv_obj_t* lbl, bool on) {
    if (lbl == nullptr) {
        return;
    }
    lv_obj_set_style_border_side(lbl, on ? LV_BORDER_SIDE_BOTTOM : LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_border_width(lbl, on ? kTabBorderW : 0, 0);
    lv_obj_set_style_border_color(lbl, UiColorForeground(), 0);
    lv_obj_set_style_pad_bottom(lbl, on ? kSyncUnderlinePadBottom : 0, 0);
    const lv_font_t* font = UiFont(on ? UiFontRole::Emphasis : UiFontRole::Body);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, UiColorForeground(), 0);
}

static int CloudRowHeight() {
    const int text_height = 2 * UiFont()->line_height + kCloudRowLineGap + 4;
    return std::max<int>(kCloudRowH, std::max<int>(text_height, kThumbH) + 12 + 2 * kRowBorderW);
}

static lv_coord_t Cloud_ContentWidth() {
    return LV_HOR_RES - kPad * 2;
}

static void Cloud_DisableScroll(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(obj, LV_DIR_NONE);
}

// 按像素逐字排满最多两行（末行可省略）。 绕开 LVGL 在 LONG_WRAP 时「非行首英文整词不拆」导致的首行留白。
static void Cloud_ShowMessage(lv_obj_t* parent, const char* msg) {
    if (parent == nullptr) {
        return;
    }
    lv_obj_clean(parent);
    lv_obj_set_style_layout(parent, LV_LAYOUT_NONE, 0);
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, msg != nullptr ? msg : "");
    lv_obj_set_style_text_font(label, UiFont(), 0);
    lv_obj_set_style_text_color(label, UiColorForeground(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, Cloud_ContentWidth());
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_center(label);
    Cloud_DisableScroll(label);
}

static void CancelStatusClearTimer() {
    if (s_status_clear_timer == nullptr) {
        return;
    }
    lv_timer_delete(s_status_clear_timer);
    s_status_clear_timer = nullptr;
}

static void ApplyStatusTipUi() {
    auto& st = s_cloud;
    if (st.chrome.status_lbl == nullptr) {
        return;
    }
    if (st.data.status_text[0] == '\0') {
        lv_label_set_text(st.chrome.status_lbl, "");
        lv_obj_add_flag(st.chrome.status_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(st.chrome.status_lbl, st.data.status_text);
    lv_obj_clear_flag(st.chrome.status_lbl, LV_OBJ_FLAG_HIDDEN);
}

static void SetStatusTip(const char* text, bool auto_clear) {
    CancelStatusClearTimer();
    auto& st = s_cloud;
    if (text == nullptr || text[0] == '\0') {
        st.data.status_text[0] = '\0';
    } else {
        std::snprintf(st.data.status_text, sizeof(st.data.status_text), "%s", text);
    }
    ApplyStatusTipUi();
    if (auto_clear && st.data.status_text[0] != '\0') {
        s_status_clear_timer = lv_timer_create(StatusClearTimerCb, kStatusClearMs, nullptr);
        if (s_status_clear_timer != nullptr) {
            lv_timer_set_repeat_count(s_status_clear_timer, 1);
        }
    }
}

static void RebuildFiltered() {
    auto& st = s_cloud;
    st.data.filtered.clear();
    st.data.filtered.reserve(st.data.items.size());
    for (int i = 0; i < static_cast<int>(st.data.items.size()); ++i) {
        if (ItemMatchesTab(st.data.items[static_cast<size_t>(i)], st.data.filter_tab)) {
            st.data.filtered.push_back(i);
        }
    }
}

static void RefreshSectionTitle() {
    auto& st = s_cloud;
    if (st.chrome.section_title == nullptr) {
        return;
    }
    lv_label_set_text(st.chrome.section_title, LangString_CLOUD_PENDING);
}

static void RefreshFilterUi() {
    auto& st = s_cloud;
    for (int i = 0; i < kFilterTabCount; ++i) {
        StyleFilterBtn(st.chrome.filter_lbls[i], st.data.filter_tab == i);
    }
}

static void StyleTabBtn(lv_obj_t* btn, lv_obj_t* lbl, bool on) {
    if (btn == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(btn, on ? UiColorForeground() : UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(btn, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    if (lbl != nullptr) {
        lv_obj_set_style_text_color(lbl, on ? UiColorBackground() : UiColorForeground(), 0);
    }
}

static void RefreshTabUi() {
    auto& st = s_cloud;
    for (int i = 0; i < kTabCount; ++i) {
        StyleTabBtn(st.chrome.tab_btns[i], st.chrome.tab_lbls[i], st.data.mode == i);
    }
    RefreshSectionTitle();
}

static void SyncSelectedSize() {
    auto& st = s_cloud;
    if (st.data.selected.size() != st.data.items.size()) {
        st.data.selected.assign(st.data.items.size(), 0);
    }
}

static int SelectedCount() {
    SyncSelectedSize();
    auto& st = s_cloud;
    int n = 0;
    for (uint8_t v : st.data.selected) {
        if (v != 0) {
            ++n;
        }
    }
    return n;
}

static bool ItemSelected(int idx) {
    SyncSelectedSize();
    auto& st = s_cloud;
    return idx >= 0 && idx < static_cast<int>(st.data.selected.size()) &&
           st.data.selected[static_cast<size_t>(idx)] != 0;
}

static void ToggleItemSelected(int idx) {
    SyncSelectedSize();
    auto& st = s_cloud;
    if (idx < 0 || idx >= static_cast<int>(st.data.selected.size())) {
        return;
    }
    st.data.selected[static_cast<size_t>(idx)] = st.data.selected[static_cast<size_t>(idx)] ? 0 : 1;
}

static void RefreshFooterMode() {
    auto& st = s_cloud;
    if (st.data.multi) {
        if (st.chrome.sync_btn != nullptr) {
            lv_obj_add_flag(st.chrome.sync_btn, LV_OBJ_FLAG_HIDDEN);
        }
        if (st.chrome.multi_bar != nullptr) {
            lv_obj_clear_flag(st.chrome.multi_bar, LV_OBJ_FLAG_HIDDEN);
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), LangString_CLOUD_SELECTED_FMT, SelectedCount());
        SetStatusTip(buf, false);
    } else {
        if (st.chrome.multi_bar != nullptr) {
            lv_obj_add_flag(st.chrome.multi_bar, LV_OBJ_FLAG_HIDDEN);
        }
        if (st.chrome.sync_btn != nullptr) {
            lv_obj_clear_flag(st.chrome.sync_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void ExitMultiModeEx(bool rebuild, bool clear_status) {
    auto& st = s_cloud;
    if (!st.data.multi && st.data.selected.empty()) {
        if (rebuild) {
            RequestCloudRender();
        }
        return;
    }
    st.data.multi = false;
    st.data.suppress_row_click_until_us = 0;
    st.data.suppress_row_click_idx = -1;
    st.data.selected.assign(st.data.items.size(), 0);
    if (clear_status) {
        SetStatusTip("", false);
    }
    RefreshFooterMode();
    if (rebuild) {
        RequestCloudRender();
    }
}

static void ExitMultiMode(bool rebuild) {
    ExitMultiModeEx(rebuild, true);
}

static void EnterMultiModeSelect(int idx) {
    auto& st = s_cloud;
    CloseActionDialog();
    CloseDialog();
    st.data.multi = true;
    SyncSelectedSize();
    st.data.selected.assign(st.data.items.size(), 0);
    if (idx >= 0 && idx < static_cast<int>(st.data.selected.size())) {
        st.data.selected[static_cast<size_t>(idx)] = 1;
    }
    RefreshFooterMode();
    RequestCloudRender();
}

static bool ScreenAlive() {
    // 仅认指针：Delete/离页会置空。工作线程禁止 lv_obj_is_valid（无锁会 LoadProhibited）
    return s_cloud.chrome.screen != nullptr;
}

static bool OpStillValid(uint32_t op_gen) {
    return op_gen != 0 && ScreenAlive() && s_cloud.xfer.op_generation == op_gen;
}

static bool SyncBusy() {
    auto& st = s_cloud;
    return st.data.waiting_net || st.data.loading;
}

static int PageCount() {
    auto& st = s_cloud;
    if (st.data.filtered.empty() || st.data.page_size <= 0) {
        return 1;
    }
    return static_cast<int>((st.data.filtered.size() + static_cast<size_t>(st.data.page_size) - 1) /
                            static_cast<size_t>(st.data.page_size));
}

static void ClampPage() {
    auto& st = s_cloud;
    const int pages = PageCount();
    if (st.data.page < 0) {
        st.data.page = 0;
    }
    if (st.data.page >= pages) {
        st.data.page = pages - 1;
    }
}

static void SetPageFooterText(const char* text) {
    auto& st = s_cloud;
    if (st.chrome.page_lbl != nullptr) {
        lv_label_set_text(st.chrome.page_lbl, text != nullptr ? text : "");
    }
}

static void Cloud_OnRowLongPressed(lv_event_t* e) {
    auto& st = s_cloud;
    if (st.xfer.dl_page_open || st.xfer.download_busy || st.data.batch_busy || st.data.delete_busy || SyncBusy() ||
        st.dialogs.dialog_mask != nullptr || st.dialogs.action_mask != nullptr || s_preview.IsOpen()) {
        return;
    }
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (index < 0 || index >= static_cast<int>(st.data.items.size())) {
        return;
    }
    if (!st.data.multi) {
        st.data.suppress_row_click_idx = index;
        st.data.suppress_row_click_until_us = esp_timer_get_time() + kSuppressRowClickUs;
        EnterMultiModeSelect(index);
        return;
    }
    ToggleItemSelected(index);
    RequestCheckMarksPaint();
}

static void Cloud_OnRowClicked(lv_event_t* e) {
    auto& st = s_cloud;
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (index == st.data.suppress_row_click_idx && esp_timer_get_time() < st.data.suppress_row_click_until_us) {
        return;
    }
    if (st.xfer.dl_page_open || SyncBusy() || st.dialogs.dialog_mask != nullptr || st.dialogs.action_mask != nullptr) {
        return;
    }
    if (index < 0 || index >= static_cast<int>(st.data.items.size())) {
        return;
    }
    if (st.data.multi) {
        ToggleItemSelected(index);
        RequestCheckMarksPaint();
        return;
    }
    OpenWallpaperPreview(index);
}

static void OnMultiCancel(lv_event_t* /*e*/) {
    auto& st = s_cloud;
    if (st.data.batch_busy || st.xfer.download_busy || st.data.delete_busy) {
        return;
    }
    ExitMultiMode(true);
}

static void OnMultiSelectAll(lv_event_t* /*e*/) {
    auto& st = s_cloud;
    if (!st.data.multi || st.data.batch_busy || st.xfer.download_busy || st.data.delete_busy) {
        return;
    }
    RebuildFiltered();
    SyncSelectedSize();
    if (st.data.filtered.empty()) {
        return;
    }
    int selected_n = 0;
    for (int idx : st.data.filtered) {
        if (ItemSelected(idx)) {
            ++selected_n;
        }
    }
    const bool clear = (selected_n >= static_cast<int>(st.data.filtered.size()));
    for (int idx : st.data.filtered) {
        if (idx >= 0 && idx < static_cast<int>(st.data.selected.size())) {
            st.data.selected[static_cast<size_t>(idx)] = clear ? 0 : 1;
        }
    }
    RequestCheckMarksPaint();
}

static void OnMultiSave(lv_event_t* /*e*/) {
    auto& state = s_cloud;
    if (state.data.multi && !CloudDlTasksActive() && !SyncBusy()) {
        StartSelectedResourceDownloads();
    }
}

static void OnMultiDelete(lv_event_t* /*e*/) {
    auto& st = s_cloud;
    if (!st.data.multi || st.data.batch_busy || st.xfer.download_busy || st.data.delete_busy || SyncBusy()) {
        return;
    }
    SyncSelectedSize();
    std::vector<CloudPushResource> items;
    for (size_t i = 0; i < st.data.items.size() && i < st.data.selected.size(); ++i) {
        if (st.data.selected[i] != 0) {
            items.push_back(st.data.items[i]);
        }
    }
    if (!items.empty()) {
        ScreenConfirmDelete(st.chrome.screen, [items = std::move(items)]() mutable {
            ScheduleDeleteItems(std::move(items));
        });
    }
}

static lv_obj_t* FindListRow(int index) {
    auto& st = s_cloud;
    if (st.chrome.list_body == nullptr || !lv_obj_is_valid(st.chrome.list_body)) {
        return nullptr;
    }
    const uint32_t n = lv_obj_get_child_count(st.chrome.list_body);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* row = lv_obj_get_child(st.chrome.list_body, i);
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
    auto& st = s_cloud;
    if (!st.data.multi) {
        return;
    }
    lv_obj_t* row = FindListRow(index);
    lv_obj_t* check = FindRowChildBySize(row, kCheckSize, kCheckSize);
    if (check == nullptr) {
        RequestCloudRender();  // 勾选列尚未建好（进多选 coalesce 未落地）
        return;
    }
    lv_obj_clean(check);
    if (!ItemSelected(index)) {
        return;
    }
    lv_obj_t* mark = lv_label_create(check);
    lv_label_set_text(mark, "√");
    lv_obj_set_style_text_font(mark, UiFont(), 0);
    lv_obj_set_style_text_color(mark, UiColorForeground(), 0);
    lv_obj_center(mark);
    lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
}

static void PatchVisibleRowCheckMarks() {
    auto& st = s_cloud;
    if (!st.data.multi || st.chrome.list_body == nullptr || !lv_obj_is_valid(st.chrome.list_body)) {
        return;
    }
    const uint32_t n = lv_obj_get_child_count(st.chrome.list_body);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* row = lv_obj_get_child(st.chrome.list_body, i);
        if (row == nullptr) {
            continue;
        }
        PatchRowCheckMark(static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(row))));
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), LangString_CLOUD_SELECTED_FMT, SelectedCount());
    SetStatusTip(buf, false);
}

static lv_obj_t* CreateResourceRow(lv_obj_t* parent, const CloudPushResource& item, int index) {
    auto& st = s_cloud;
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    // 以外框内容宽为准：需扣 list_frame 的 pad + border，否则右边框竖线被裁切
    lv_coord_t row_w = parent != nullptr ? lv_obj_get_content_width(parent) : 0;
    if (row_w <= 0) {
        row_w = Cloud_ContentWidth() - kListFramePad * 2 - kListFrameBorderW * 2;
    }
    lv_obj_set_size(row, row_w, CloudRowHeight());
    lv_obj_set_style_bg_color(row, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, kRowBorderW, 0);
    lv_obj_set_style_border_color(row, UiColorForeground(), 0);
    lv_obj_set_style_radius(row, kRowRadius, 0);
    lv_obj_set_style_pad_hor(row, kRowPad, 0);
    lv_obj_set_style_pad_ver(row, 6, 0);
    lv_obj_set_style_layout(row, LV_LAYOUT_NONE, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    Cloud_DisableScroll(row);
    lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    HapticAttachClick(row);
    lv_obj_add_event_cb(row, Cloud_OnRowClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    lv_obj_add_event_cb(row, Cloud_OnRowLongPressed, LV_EVENT_LONG_PRESSED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(index)));

    const bool show_thumb = item.type == PushResourceType::kBook || item.type == PushResourceType::kBadge ||
                            item.type == PushResourceType::kFont;
    lv_coord_t text_x = 0;
    if (show_thumb) {
        lv_obj_t* thumb_box = lv_obj_create(row);
        lv_obj_remove_style_all(thumb_box);
        lv_obj_set_size(thumb_box, kThumbW, kThumbH);
        lv_obj_set_style_border_width(thumb_box, 2, 0);
        lv_obj_set_style_border_color(thumb_box, UiColorForeground(), 0);
        lv_obj_set_style_bg_color(thumb_box, UiColorBackground(), 0);
        lv_obj_set_style_bg_opa(thumb_box, LV_OPA_COVER, 0);
        lv_obj_align(thumb_box, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_clear_flag(thumb_box, LV_OBJ_FLAG_CLICKABLE);
        Cloud_DisableScroll(thumb_box);

        if (index >= 0 && index < static_cast<int>(st.data.thumbs.size()) &&
            st.data.thumbs[static_cast<size_t>(index)] != nullptr &&
            !st.data.thumbs[static_cast<size_t>(index)]->empty()) {
            lv_obj_t* thumb = lv_image_create(thumb_box);
            lv_image_set_src(thumb, &st.data.thumbs[static_cast<size_t>(index)]->dsc);
            // 列表封面使用黑白模式，灰阶仅用于详情图，避免影响整页对比度。
            EpdSetContentRole(thumb, EpdContentRole::Monochrome);
            lv_obj_set_size(thumb, st.data.thumbs[static_cast<size_t>(index)]->width,
                            st.data.thumbs[static_cast<size_t>(index)]->height);
            lv_obj_center(thumb);
            lv_obj_clear_flag(thumb, LV_OBJ_FLAG_CLICKABLE);
        }
        text_x = kThumbW + kThumbGap;
    }

    const lv_font_t* item_font = UiFont();
    const lv_coord_t line_h = (item_font != nullptr && item_font->line_height > 0) ? item_font->line_height : 30;
    const bool show_check = st.data.multi;
    const lv_coord_t trail_reserve = show_check ? (kCheckSize + kCheckGap) : 0;
    const lv_coord_t text_w = row_w - 2 * (kRowPad + kRowBorderW) - text_x - trail_reserve;
    const std::string line1 = ScreenEllipsizeText(item.name, item_font, text_w);

    lv_obj_t* title = lv_label_create(row);
    lv_obj_set_style_text_font(title, item_font, 0);
    lv_obj_set_style_text_color(title, UiColorForeground(), 0);
    lv_obj_set_size(title, text_w, line_h);
    lv_label_set_long_mode(title, LV_LABEL_LONG_CLIP);
    lv_label_set_text(title, line1.c_str());
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, text_x, show_thumb ? 4 : 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    char line2[80];
    std::snprintf(line2, sizeof(line2), "%s · %s", item.TypeLabel(), FormatCloudFileSize(item.file_size).c_str());
    lv_obj_t* meta = lv_label_create(row);
    lv_obj_set_style_text_font(meta, item_font, 0);
    lv_obj_set_style_text_color(meta, UiColorForeground(), 0);
    lv_obj_set_size(meta, text_w, line_h);
    lv_label_set_long_mode(meta, LV_LABEL_LONG_CLIP);
    lv_label_set_text(meta, line2);
    lv_obj_align(meta, LV_ALIGN_TOP_LEFT, text_x, (show_thumb ? 4 : 0) + line_h + kCloudRowLineGap);
    lv_obj_clear_flag(meta, LV_OBJ_FLAG_CLICKABLE);

    if (show_check) {
        // 行尾勾选：白底黑框；选中只画 √，勿实心填充（同每日清单）
        lv_obj_t* check = lv_obj_create(row);
        lv_obj_remove_style_all(check);
        lv_obj_set_size(check, kCheckSize, kCheckSize);
        lv_obj_set_style_bg_color(check, UiColorBackground(), 0);
        lv_obj_set_style_bg_opa(check, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(check, UiColorForeground(), 0);
        lv_obj_set_style_border_width(check, kRowBorderW, 0);
        lv_obj_set_style_radius(check, 4, 0);
        lv_obj_align(check, LV_ALIGN_RIGHT_MID, 0, 0);
        Cloud_DisableScroll(check);
        lv_obj_clear_flag(check, LV_OBJ_FLAG_CLICKABLE);
        if (ItemSelected(index)) {
            lv_obj_t* mark = lv_label_create(check);
            lv_label_set_text(mark, "√");
            lv_obj_set_style_text_font(mark, item_font, 0);
            lv_obj_set_style_text_color(mark, UiColorForeground(), 0);
            lv_obj_center(mark);
            lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    return row;
}

static void RenderListPageInternal(bool schedule_thumbs) {
    auto& st = s_cloud;
    if (st.chrome.list_body == nullptr) {
        return;
    }
    lv_obj_set_style_layout(st.chrome.list_body, LV_LAYOUT_FLEX, 0);
    lv_obj_set_flex_flow(st.chrome.list_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(st.chrome.list_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(st.chrome.list_body, UiSy(4), 0);
    lv_obj_clean(st.chrome.list_body);
    Cloud_DisableScroll(st.chrome.list_body);

    // 拉取中若已有列表：保留行，提示在标题旁；仅空列表时占位
    if (st.data.waiting_net && st.data.items.empty()) {
        Cloud_ShowMessage(st.chrome.list_body, LangString_CLOUD_CONNECT_NET);
        SetPageFooterText("");
        return;
    }
    if (st.data.loading && st.data.items.empty()) {
        Cloud_ShowMessage(st.chrome.list_body, LangString_CLOUD_FETCHING_LIST);
        SetPageFooterText("");
        return;
    }
    if (st.data.covers_loading && !st.data.items.empty()) {
        Cloud_ShowMessage(st.chrome.list_body, LangString_CLOUD_LOAD_COVER);
        SetPageFooterText("");
        return;
    }
    if (!st.data.error.empty() && st.data.items.empty()) {
        Cloud_ShowMessage(st.chrome.list_body, st.data.error.c_str());
        SetPageFooterText("1 / 1");
        return;
    }

    RebuildFiltered();
    if (st.data.filtered.empty()) {
        const char* empty_msg = LangString_CLOUD_EMPTY_ALL;
        if (st.data.filter_tab == 1) {
            empty_msg = LangString_CLOUD_EMPTY_WALLPAPER;
        } else if (st.data.filter_tab == 2) {
            empty_msg = LangString_CLOUD_EMPTY_BOOK;
        } else if (st.data.filter_tab == 3) {
            empty_msg = LangString_CLOUD_EMPTY_FONT;
        }
        Cloud_ShowMessage(st.chrome.list_body, empty_msg);
        SetPageFooterText("1 / 1");
        return;
    }

    ClampPage();
    const int start = st.data.page * st.data.page_size;
    const int end = std::min(start + st.data.page_size, static_cast<int>(st.data.filtered.size()));
    for (int fi = start; fi < end; ++fi) {
        const int i = st.data.filtered[static_cast<size_t>(fi)];
        CreateResourceRow(st.chrome.list_body, st.data.items[static_cast<size_t>(i)], i);
    }

    {
        char foot[48];
        std::snprintf(foot, sizeof(foot), "%d / %d", st.data.page + 1, PageCount());
        SetPageFooterText(foot);
    }

    if (schedule_thumbs && !st.data.loading && !st.data.waiting_net && !st.data.covers_loading &&
        !st.xfer.download_busy && !st.data.batch_busy) {
        ScheduleCoverThumbFill();
    }
    RefreshFooterMode();
}

static void RenderListPage() {
    RenderListPageInternal(true);
}

static void RequestCloudRender() {
    if (s_cloud_paint.paint == nullptr) {
        s_cloud_paint.paint = RenderListPage;
    }
    ScreenPaintCoalesceRequest(&s_cloud_paint);
}

static void RequestCheckMarksPaint() {
    if (s_cloud_check_paint.paint == nullptr) {
        s_cloud_check_paint.paint = PatchVisibleRowCheckMarks;
    }
    // 点间隙手指会抬起：勿立刻上屏，停手后再一次刷全部 √
    ScreenPaintCoalesceRequestDebounced(&s_cloud_check_paint, 280000);
}

static void OnDlProgressCancelClicked(lv_event_t* /*e*/) {
    AbortDownloadJobFromUi();
}

static void EnsureDlProgressPageBuilt() {
    auto& st = s_cloud;
    if (st.xfer.dl_page != nullptr || st.chrome.screen == nullptr) {
        return;
    }

    lv_obj_t* page = lv_obj_create(st.chrome.screen);
    st.xfer.dl_page = page;
    s_download_modal.Bind(page, [](void*) { AbortDownloadJobFromUi(); });
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(page, 0, 0);
    ScreenApplyDotBackdrop(page);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(page);

    lv_obj_t* card = lv_obj_create(page);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_HOR_RES - kDialogCardMargin, LV_SIZE_CONTENT);
    lv_obj_center(card);
    ScreenStyleDialogCard(card, kDialogCardPad);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, kDialogRowGap, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, [](lv_event_t* e) { lv_event_stop_bubbling(e); }, LV_EVENT_CLICKED, nullptr);

    st.xfer.dl_title_lbl = lv_label_create(card);
    lv_label_set_text(st.xfer.dl_title_lbl, LangString_CLOUD_SAVING);
    lv_obj_set_width(st.xfer.dl_title_lbl, lv_pct(100));
    lv_obj_set_style_text_align(st.xfer.dl_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(st.xfer.dl_title_lbl, UiFont(UiFontRole::Title), 0);
    lv_obj_set_style_text_color(st.xfer.dl_title_lbl, UiColorForeground(), 0);
    lv_obj_clear_flag(st.xfer.dl_title_lbl, LV_OBJ_FLAG_CLICKABLE);

    st.xfer.dl_count_lbl = lv_label_create(card);
    lv_label_set_text(st.xfer.dl_count_lbl, LangString_CLOUD_SAVED_0_1);
    lv_obj_set_width(st.xfer.dl_count_lbl, lv_pct(100));
    lv_obj_set_style_text_align(st.xfer.dl_count_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(st.xfer.dl_count_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(st.xfer.dl_count_lbl, UiColorForeground(), 0);
    lv_obj_clear_flag(st.xfer.dl_count_lbl, LV_OBJ_FLAG_CLICKABLE);

    st.xfer.dl_bar = lv_bar_create(card);
    lv_obj_set_size(st.xfer.dl_bar, lv_pct(100), kDlBarH);
    lv_bar_set_range(st.xfer.dl_bar, 0, 100);
    lv_bar_set_value(st.xfer.dl_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(st.xfer.dl_bar, UiColorBackground(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(st.xfer.dl_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(st.xfer.dl_bar, UiColorForeground(), LV_PART_MAIN);
    lv_obj_set_style_border_width(st.xfer.dl_bar, kRowBorderW, LV_PART_MAIN);
    lv_obj_set_style_radius(st.xfer.dl_bar, kDlBarRadius, LV_PART_MAIN);
    lv_obj_set_style_bg_color(st.xfer.dl_bar, UiColorForeground(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(st.xfer.dl_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(st.xfer.dl_bar, kDlBarIndRadius, LV_PART_INDICATOR);
    lv_obj_clear_flag(st.xfer.dl_bar, LV_OBJ_FLAG_CLICKABLE);

    st.xfer.dl_pct_lbl = lv_label_create(card);
    lv_label_set_text(st.xfer.dl_pct_lbl, "0%");
    lv_obj_set_style_text_font(st.xfer.dl_pct_lbl, UiFont(UiFontRole::Title), 0);
    lv_obj_set_style_text_color(st.xfer.dl_pct_lbl, UiColorForeground(), 0);
    lv_obj_clear_flag(st.xfer.dl_pct_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* cancel_btn = lv_obj_create(card);
    lv_obj_remove_style_all(cancel_btn);
    lv_obj_set_size(cancel_btn, kCancelBtnW, kCancelBtnH);
    lv_obj_set_style_bg_color(cancel_btn, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(cancel_btn, UiColorForeground(), 0);
    lv_obj_set_style_border_width(cancel_btn, kRowBorderW, 0);
    lv_obj_set_style_radius(cancel_btn, kRowRadius, 0);
    lv_obj_add_flag(cancel_btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(cancel_btn);
    lv_obj_add_event_cb(cancel_btn, OnDlProgressCancelClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, LangString_COMMON_CANCEL);
    lv_obj_set_style_text_font(cancel_lbl, UiFont(UiFontRole::Title), 0);
    lv_obj_center(cancel_lbl);
    lv_obj_clear_flag(cancel_lbl, LV_OBJ_FLAG_CLICKABLE);
}

static void RefreshDlProgressPage(int file_percent) {
    auto& st = s_cloud;
    if (!st.xfer.dl_page_open || st.xfer.dl_page == nullptr) {
        return;
    }
    if (file_percent < 0) {
        file_percent = 0;
    }
    if (file_percent > 100) {
        file_percent = 100;
    }
    if (st.xfer.dl_count_lbl != nullptr) {
        char buf[48];
        const int total = st.xfer.dl_job_total > 0 ? st.xfer.dl_job_total : 1;
        const int done = st.xfer.dl_job_done < 0 ? 0 : st.xfer.dl_job_done;
        std::snprintf(buf, sizeof(buf), LangString_CLOUD_SAVED_FMT, done, total);
        lv_label_set_text(st.xfer.dl_count_lbl, buf);
    }
    if (file_percent == st.xfer.dl_shown_percent) {
        return;
    }
    st.xfer.dl_shown_percent = file_percent;
    if (st.xfer.dl_bar != nullptr) {
        lv_bar_set_value(st.xfer.dl_bar, file_percent, LV_ANIM_OFF);
    }
    if (st.xfer.dl_pct_lbl != nullptr) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d%%", file_percent);
        lv_label_set_text(st.xfer.dl_pct_lbl, buf);
    }
}

static void ShowDlProgressPage() {
    auto& st = s_cloud;
    EnsureDlProgressPageBuilt();
    if (st.xfer.dl_page == nullptr) {
        return;
    }
    CloseDialog();
    CloseActionDialog();
    if (s_preview.IsOpen()) {
        ShowWallpaperPreviewMode(false);
    }
    st.xfer.dl_page_open = true;
    st.xfer.dl_shown_percent = -1;
    if (st.chrome.main_body != nullptr) {
        lv_obj_add_flag(st.chrome.main_body, LV_OBJ_FLAG_HIDDEN);
    }
    if (st.chrome.page_lbl != nullptr) {
        lv_obj_add_flag(st.chrome.page_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(st.xfer.dl_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(st.xfer.dl_page);
    // 遮罩盖住列表后下层热区仍按坐标早震；关掉热区早震，取消靠 PRESSED
    HapticSetZoneEarlyPulseEnabled(false);
    RefreshDlProgressPage(0);
}

static void HideDlProgressPage() {
    auto& st = s_cloud;
    const bool was_open = st.xfer.dl_page_open;
    st.xfer.dl_page_open = false;
    st.xfer.dl_shown_percent = -1;
    HapticSetZoneEarlyPulseEnabled(true);
    if (st.xfer.dl_page != nullptr) {
        lv_obj_add_flag(st.xfer.dl_page, LV_OBJ_FLAG_HIDDEN);
    }
    if (st.chrome.main_body != nullptr && !s_preview.IsOpen()) {
        lv_obj_clear_flag(st.chrome.main_body, LV_OBJ_FLAG_HIDDEN);
    }
    if (st.chrome.page_lbl != nullptr && !s_preview.IsOpen()) {
        lv_obj_clear_flag(st.chrome.page_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    if (was_open && ScreenAlive()) {
        EpdCancelPageTurn();
        EpdRequestScreenChange();
        lv_obj_invalidate(st.chrome.screen);
    }
}

static void OnActionCancelClicked(lv_event_t* /*e*/) {
    CloseActionDialog();
}

static void ApplyCloudDeleteResult(const CloudResourceRequest& result) {
    auto& state = s_cloud;
    state.data.delete_busy = false;
    if (state.chrome.list_body) {
        lv_obj_clean(state.chrome.list_body);
    }
    for (size_t i = 0; i < result.inputs.size(); ++i) {
        if (result.outcomes[i] == CloudResourceOutcome::Deleted) {
            EraseItemAt(FindItemIndexByTaskId(result.inputs[i].resource.task_id.c_str()));
        }
    }
    const int succeeded = result.succeeded.load(std::memory_order_acquire);
    char text[96];
    if (result.gate.IsCancelled() && result.failed == 0) {
        SetStatusTip(LangString_CLOUD_CANCELLED, true);
    } else if (succeeded > 0 && result.failed == 0) {
        std::snprintf(text, sizeof(text), LangString_TASK_REMOVED_N_FMT, succeeded);
        SetStatusTip(text, true);
    } else if (succeeded > 0) {
        std::snprintf(text, sizeof(text), LangString_TASK_BATCH_RESULT_FMT, succeeded, result.failed);
        SetStatusTip(text, true);
    } else {
        SetStatusTip(result.error[0] ? result.error : LangString_CLOUD_DELETE_FAIL, true);
    }
    ExitMultiModeEx(false, false);
    RenderListPage();
    ResumeCloudSideHttpAfterDl();
}

static void ShowResourceDialog(int cloud_index, bool conflict) {
    auto& st = s_cloud;
    if (!ScreenAlive() || st.dialogs.action_mask != nullptr || st.dialogs.dialog_mask != nullptr ||
        (!conflict && st.xfer.download_busy) || st.data.delete_busy || SyncBusy()) {
        return;
    }
    if (cloud_index < 0 || cloud_index >= static_cast<int>(st.data.items.size())) {
        return;
    }

    const CloudPushResource& item = st.data.items[static_cast<size_t>(cloud_index)];
    const lv_coord_t card_w = LV_HOR_RES - kDialogCardMargin;
    const lv_coord_t text_w = card_w - 2 * (kDialogCardPadSmall + kRowBorderW);
    // 单行省略书名/文件名，避免 WRAP 顶掉底部按钮
    const std::string short_name = ScreenEllipsizeText(item.name, UiFont(UiFontRole::Title), text_w);
    char title_buf[192];
    std::snprintf(title_buf, sizeof(title_buf), "「%s」\n%s", short_name.c_str(), item.TypeLabel());
    if (conflict) {
        st.xfer.pending_index = cloud_index;
        const std::string path = item.LocalPath();
        const std::string filename =
            ScreenEllipsizeText(path.substr(path.find_last_of('/') + 1), UiFont(UiFontRole::Title), text_w);
        std::snprintf(title_buf, sizeof(title_buf), LangString_CLOUD_CONFLICT_FMT, filename.c_str());
    } else {
        st.xfer.action_index = cloud_index;
    }

    lv_obj_t* mask = lv_obj_create(st.chrome.screen);
    s_resource_modal.Bind(mask, [](void*) {
        if (s_cloud.dialogs.dialog_mask) {
            AbortDownloadJobFromUi();
        } else {
            CloseActionDialog();
        }
    });
    if (conflict) {
        st.dialogs.dialog_mask = mask;
    } else {
        st.dialogs.action_mask = mask;
    }
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(mask, 0, 0);
    ScreenApplyDotBackdrop(mask);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(mask);
    lv_obj_add_event_cb(mask, conflict ? OnDlProgressCancelClicked : OnActionCancelClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, card_w, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(card, UiSy(144), 0);
    lv_obj_center(card);
    ScreenStyleDialogCard(card, kDialogCardPadSmall);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, kDialogRowGapSmall, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, [](lv_event_t* e) { lv_event_stop_bubbling(e); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* lbl = lv_label_create(card);
    lv_label_set_text(lbl, title_buf);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl, UiFont(UiFontRole::Title), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* btn_row = lv_obj_create(card);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, lv_pct(100));
    lv_obj_set_height(btn_row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(btn_row, conflict ? 0 : kActionBtnRowH, 0);
    lv_obj_set_flex_flow(btn_row, conflict ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_row(btn_row, kDialogRowGap, 0);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_CLICKABLE);

    auto make_btn = [&](const char* text, lv_event_cb_t cb) {
        auto* btn = ScreenCreateSelectableOption(btn_row, text, cb, 0);
        lv_obj_set_width(btn, conflict ? text_w : kActionBtnW);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_min_height(btn, kActionBtnH, 0);
        lv_obj_set_style_text_font(lv_obj_get_child(btn, 0), UiFont(UiFontRole::Title), 0);
    };
    if (conflict) {
        make_btn(LangString_CLOUD_KEEP_BOTH, [](lv_event_t*) {
            const int index = s_cloud.xfer.pending_index;
            CloseDialog();
            StartResourceDownload(index, CloudSaveChoice::kKeepBoth);
        });
        make_btn(LangString_CLOUD_REPLACE_FILE, [](lv_event_t*) {
            const int index = s_cloud.xfer.pending_index;
            CloseDialog();
            StartResourceDownload(index, CloudSaveChoice::kReplace);
        });
        make_btn(LangString_CLOUD_SKIP_FILE, [](lv_event_t*) {
            const int index = s_cloud.xfer.pending_index;
            CloseDialog();
            StartResourceDownload(index, CloudSaveChoice::kSkip);
        });
        make_btn(LangString_COMMON_CANCEL, OnDlProgressCancelClicked);
        HapticSetZoneEarlyPulseEnabled(false);
    } else {
        make_btn(LangString_COMMON_DELETE, Cloud_OnDeleteClicked);
        make_btn(LangString_COMMON_CANCEL, OnActionCancelClicked);
    }
    lv_obj_move_foreground(mask);
}

static void CloseDialog() {
    auto& st = s_cloud;
    if (st.dialogs.dialog_mask != nullptr && ScreenAlive()) {
        lv_obj_delete(st.dialogs.dialog_mask);
    }
    st.dialogs.dialog_mask = nullptr;
    st.xfer.pending_index = -1;
    HapticSetZoneEarlyPulseEnabled(!st.xfer.dl_page_open);
}

static void CloseActionDialog() {
    auto& st = s_cloud;
    if (st.dialogs.action_mask != nullptr && ScreenAlive()) {
        lv_obj_delete(st.dialogs.action_mask);
    }
    st.dialogs.action_mask = nullptr;
    st.xfer.action_index = -1;
}

static void ShowSaveConflictDialog(int cloud_index) {
    ShowResourceDialog(cloud_index, true);
}

static void ScheduleDeleteItems(std::vector<CloudPushResource> items) {
    auto& state = s_cloud;
    if (items.empty()) {
        SetStatusTip(LangString_CLOUD_SELECT_FIRST, true);
        return;
    }
    if (state.request || CloudResourcesBusy()) {
        SetStatusTip(LangString_CLOUD_PLEASE_WAIT, true);
        return;
    }
    state.request = CloudDeleteResources(std::move(items));
    if (!state.request) {
        SetStatusTip(LangString_CLOUD_DELETE_FAIL, true);
        return;
    }
    state.request_generation = state.xfer.op_generation;
    state.data.delete_busy = true;
    state.data.multi = false;
    RefreshFooterMode();
    SetStatusTip(LangString_CLOUD_PLEASE_WAIT, false);
    AbortCloudSideHttpForDownload();
}

static void Cloud_OnDeleteClicked(lv_event_t* /*e*/) {
    auto& st = s_cloud;
    const int index = st.xfer.action_index;
    CloseActionDialog();
    if (index < 0 || index >= static_cast<int>(st.data.items.size())) {
        return;
    }
    std::vector<CloudPushResource> items;
    items.push_back(st.data.items[static_cast<size_t>(index)]);
    ScreenConfirmDelete(st.chrome.screen, [items = std::move(items)]() mutable {
        ScheduleDeleteItems(std::move(items));
    });
}

static void ShowStorageErrorDialog() {
    auto& st = s_cloud;
    if (!ScreenAlive() || st.dialogs.storage_error_mask != nullptr) {
        return;
    }
    lv_obj_t* mask = lv_obj_create(st.chrome.screen);
    st.dialogs.storage_error_mask = mask;
    s_storage_modal.Bind(mask, nullptr, nullptr, UiModalPolicy::Locked);
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, LV_HOR_RES, LV_VER_RES);
    ScreenApplyDotBackdrop(mask);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(mask);

    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_HOR_RES - kDialogCardMargin, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_pad_row(card, kDialogRowGapSmall, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ScreenStyleDialogCard(card, kDialogCardPad);

    lv_obj_t* text = lv_label_create(card);
    lv_label_set_text(text, LangString_STORAGE_RESTART_REQUIRED);
    lv_obj_set_width(text, lv_pct(100));
    lv_obj_set_style_text_font(text, UiFont(UiFontRole::Title), 0);
    lv_obj_set_style_text_color(text, UiColorForeground(), 0);
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* button = lv_obj_create(card);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, kCancelBtnW, kCancelBtnH);
    lv_obj_set_style_bg_color(button, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(button, UiColorForeground(), 0);
    lv_obj_set_style_border_width(button, kRowBorderW, 0);
    lv_obj_set_style_radius(button, kRowRadius, 0);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(button);
    lv_obj_add_event_cb(
        button,
        [](lv_event_t* e) {
            lv_obj_remove_flag(static_cast<lv_obj_t*>(lv_event_get_target(e)), LV_OBJ_FLAG_CLICKABLE);
            Cloud_RebootAfterStorageError();
        },
        LV_EVENT_CLICKED, nullptr);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, LangString_COMMON_RESTART);
    lv_obj_set_style_text_font(label, UiFont(UiFontRole::Title), 0);
    lv_obj_set_style_text_color(label, UiColorForeground(), 0);
    lv_obj_center(label);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
}

static void ScheduleCloudFetch() {
    auto& state = s_cloud;
    if (!ScreenAlive() || state.data.mode != kCloudModeApp || YunxinBinding::NeedsBinding() ||
        state.request || state.xfer.download_busy) {
        return;
    }
    if (CloudResourcesBusy()) {
        state.data.waiting_net = true;
        return;
    }
    CancelCloudImageViews();
    state.request = CloudFetchResources();
    state.data.waiting_net = false;
    state.data.loading = state.request != nullptr;
    if (!state.request) {
        state.data.error = LangString_CLOUD_TASK_FAIL;
        SetStatusTip("", false);
        RenderListPage();
        return;
    }
    state.request_generation = state.xfer.op_generation;
    state.data.error.clear();
    state.data.fetch_done = false;
    SetStatusTip(LangString_CLOUD_REFRESHING, false);
    if (state.data.items.empty()) {
        RenderListPage();
    } else {
        ApplyStatusTipUi();
    }
}

static void ApplyCloudFetchResult(CloudResourceRequest& result) {
    auto& state = s_cloud;
    state.data.loading = false;
    state.data.waiting_net = false;
    if (result.gate.IsCancelled()) {
        SetStatusTip("", false);
        return;
    }
    // 先拆除借用图片描述符的控件，再替换列表和缩略图。
    if (state.chrome.list_body) {
        lv_obj_clean(state.chrome.list_body);
    }
    state.data.thumbs.clear();
    state.data.cover_cache.clear();
    state.data.filtered.clear();
    state.data.selected.clear();
    ++state.data.thumb_epoch;
    if (result.failed == 0) {
        state.data.items = std::move(result.items);
        state.data.thumbs.resize(state.data.items.size());
        state.data.cover_cache.resize(state.data.items.size());
        state.data.error.clear();
        state.data.page = 0;
        RebuildFiltered();
        state.data.covers_loading = !state.data.items.empty();
        SetStatusTip(state.data.items.empty() ? "" : LangString_CLOUD_LOAD_COVER, false);
    } else {
        state.data.items.clear();
        state.data.covers_loading = false;
        state.data.error = result.error[0] ? result.error : LangString_CLOUD_SYNC_FAIL;
        SetStatusTip("", false);
    }
    state.data.fetch_done = true;
    ExitMultiModeEx(false, false);
    SyncSelectedSize();
    RenderListPage();
    if (state.data.covers_loading) {
        ScheduleCoverThumbFill();
    }
}

static void CancelCloudImageViews() {
    auto& state = s_cloud;
    CloudCancelImages(state.covers_request);
    s_preview.CancelPending();
    state.covers_request.reset();
    state.data.covers_loading = false;
}

static void ScheduleCoverThumbFill() try {
    auto& state = s_cloud;
    if (!ScreenAlive() || state.data.mode != kCloudModeApp || state.data.items.empty() || state.covers_request ||
        CloudDlTasksActive() || SyncBusy()) {
        return;
    }
    state.data.thumbs.resize(state.data.items.size());
    state.data.cover_cache.resize(state.data.items.size());
    std::vector<CloudImageItem> items;
    for (size_t i = 0; i < state.data.items.size(); ++i) {
        if (state.data.thumbs[i] || !state.data.items[i].HasCoverThumb()) {
            continue;
        }
        CloudImageItem item;
        item.index = static_cast<int>(i);
        item.url = state.data.items[i].CoverThumbUrl();
        item.cache = state.data.cover_cache[i];
        item.width = kThumbDecodeW;
        item.height = kThumbDecodeH;
        item.binary = true;
        items.push_back(std::move(item));
    }
    if (items.empty()) {
        if (state.data.covers_loading) {
            state.data.covers_loading = false;
            SetStatusTip(LangString_CLOUD_REFRESHED, true);
            RenderListPage();
        }
        return;
    }
    state.covers_request = CloudLoadImages(std::move(items));
    state.covers_generation = state.data.thumb_epoch;
    if (!state.covers_request) {
        state.data.covers_loading = false;
        SetStatusTip(LangString_CLOUD_COVER_FAIL, true);
        RenderListPage();
    }
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=covers stage=submit result=no_memory");
    s_cloud.data.covers_loading = false;
    SetStatusTip(LangString_CLOUD_COVER_FAIL, true);
    RenderListPage();
}

static void PollCloudImages() {
    auto& state = s_cloud;
    if (state.covers_request && state.covers_request->completed.load(std::memory_order_acquire)) {
        const auto request = std::move(state.covers_request);
        if (state.covers_generation == state.data.thumb_epoch && !request->gate.IsCancelled()) {
            for (auto& item : request->items) {
                if (item.index < 0 || item.index >= static_cast<int>(state.data.thumbs.size())) {
                    continue;
                }
                state.data.cover_cache[item.index] = std::move(item.cache);
                if (item.image && !item.image->empty()) {
                    state.data.thumbs[item.index] = std::move(item.image);
                }
            }
            state.data.covers_loading = false;
            SetStatusTip(LangString_CLOUD_REFRESHED, true);
            RenderListPage();
        }
    }
    s_preview.Poll();
}

static void CloudRevealAfterWhite() {
    EpdCancelPageTurn();
    EpdRequestScreenChange();
    if (s_cloud.chrome.screen != nullptr) {
        lv_obj_invalidate(s_cloud.chrome.screen);
    }
}

static void ShowWallpaperPreviewMode(bool preview) {
    auto& st = s_cloud;
    if (!preview) {
        s_preview.Close();
    }
    if (st.chrome.main_body != nullptr) {
        if (preview) {
            lv_obj_add_flag(st.chrome.main_body, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(st.chrome.main_body, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (st.chrome.page_lbl != nullptr) {
        if (preview) {
            lv_obj_add_flag(st.chrome.page_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(st.chrome.page_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
    // 离开详情时请求清白过渡，避免灰阶图像残留。
    if (!preview) {
        CloudRevealAfterWhite();
    }
}

static void CloseWallpaperPreview() {
    ShowWallpaperPreviewMode(false);
    RenderListPage();
}

static void OpenWallpaperPreview(int index) {
    auto& state = s_cloud;
    if (index < 0 || index >= static_cast<int>(state.data.items.size()) ||
        state.dialogs.dialog_mask || state.dialogs.action_mask) {
        return;
    }
    auto cache = index < static_cast<int>(state.data.cover_cache.size()) ? state.data.cover_cache[index] : nullptr;
    if (s_preview.Open(index, state.data.items[index], std::move(cache), CloudDlTasksActive())) {
        ShowWallpaperPreviewMode(true);
        CloudRevealAfterWhite();
    } else {
        ShowWallpaperPreviewMode(false);
        RenderListPage();
    }
}

static void StartWallpaperDownload(int index) {
    auto& state = s_cloud;
    if (SyncBusy() || CloudDlTasksActive()) {
        SetStatusTip(LangString_CLOUD_PLEASE_WAIT, true);
        return;
    }
    if (s_preview.IsOpen()) {
        ShowWallpaperPreviewMode(false);
    }
    StartResourceDownload(index);
}

static void AbortDownloadJobFromUi() {
    auto& state = s_cloud;
    if (!state.request || state.request->operation != CloudResourceOperation::Download) {
        return;
    }
    CloudCancelResourceRequest(state.request);
    CloseDialog();
    if (state.xfer.dl_title_lbl) {
        lv_label_set_text(state.xfer.dl_title_lbl, LangString_CLOUD_CANCELLING);
    }
}

static bool CloudDlTasksActive() {
    const auto& state = s_cloud;
    return state.xfer.download_busy || state.data.batch_busy || state.data.delete_busy ||
           state.xfer.pending_index >= 0 || CloudResourcesBusy();
}

static void AbortCloudSideHttpForDownload() {
    CancelCloudImageViews();
    ++s_cloud.data.thumb_epoch;
    s_preview.PauseForTransfer();
}

static void ResumeCloudSideHttpAfterDl() {
    if (!ScreenAlive() || CloudDlTasksActive()) {
        return;
    }
    ScheduleCoverThumbFill();
    s_preview.Resume();
}

static void SubmitDownloads(std::vector<CloudDownloadItem> items, CloudSaveChoice choice) {
    auto& state = s_cloud;
    state.request = CloudDownloadResources(std::move(items), choice);
    if (!state.request) {
        SetStatusTip(LangString_CLOUD_TASK_FAIL, true);
        return;
    }
    state.request_generation = state.xfer.op_generation;
    state.shown_download_index = SIZE_MAX;
    state.download_painted_us = 0;
    state.data.multi = false;
    state.data.batch_busy = state.request->inputs.size() > 1;
    state.xfer.download_busy = true;
    state.xfer.dl_job_total = static_cast<int>(state.request->inputs.size());
    state.xfer.dl_job_done = 0;
    SetStatusTip("", false);
    RefreshFooterMode();
    AbortCloudSideHttpForDownload();
    ShowDlProgressPage();
}

static void StartResourceDownload(int index, CloudSaveChoice choice) try {
    auto& state = s_cloud;
    if (!ScreenAlive()) {
        return;
    }
    if (state.request && state.request->operation == CloudResourceOperation::Download &&
        state.request->stage.load(std::memory_order_acquire) == CloudResourceStage::Conflict) {
        if (!CloudResolveSaveConflict(state.request, choice)) {
            ScreenShowToast(LangString_CLOUD_TASK_FAIL);
        }
        return;
    }
    if (state.request || CloudResourcesBusy() || index < 0 || index >= static_cast<int>(state.data.items.size())) {
        SetStatusTip(LangString_CLOUD_PLEASE_WAIT, true);
        return;
    }
    CloudDownloadItem item;
    item.resource = state.data.items[index];
    if (index < static_cast<int>(state.data.cover_cache.size())) {
        item.cover = state.data.cover_cache[index];
    }
    std::vector<CloudDownloadItem> items;
    items.push_back(std::move(item));
    SubmitDownloads(std::move(items), choice);
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=download stage=submit result=no_memory");
    SetStatusTip(LangString_COMMON_MEMORY_RETRY, true);
}

static void StartSelectedResourceDownloads() try {
    auto& state = s_cloud;
    if (state.request || CloudResourcesBusy()) {
        return;
    }
    SyncSelectedSize();
    std::vector<CloudDownloadItem> items;
    for (size_t i = 0; i < state.data.items.size() && i < state.data.selected.size(); ++i) {
        if (!state.data.selected[i]) {
            continue;
        }
        items.push_back({state.data.items[i], i < state.data.cover_cache.size() ? state.data.cover_cache[i] : nullptr});
    }
    if (items.empty()) {
        SetStatusTip(LangString_CLOUD_SELECT_FIRST, true);
        return;
    }
    SubmitDownloads(std::move(items), CloudSaveChoice::kAsk);
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=download_batch stage=submit result=no_memory");
    SetStatusTip(LangString_COMMON_MEMORY_RETRY, true);
}

static void RefreshCloudDownload() {
    auto& state = s_cloud;
    const auto& request = state.request;
    if (!request || request->operation != CloudResourceOperation::Download) {
        return;
    }
    const auto stage = request->stage.load(std::memory_order_acquire);
    const size_t current = request->current.load(std::memory_order_acquire);
    if (current < request->inputs.size() && state.shown_download_index != current) {
        state.shown_download_index = current;
        const auto& resource = request->inputs[current].resource;
        if (state.xfer.dl_title_lbl) {
            const auto shown = ScreenEllipsizeText(resource.name, UiFont(UiFontRole::Large), LV_HOR_RES - 80);
            lv_label_set_text_fmt(state.xfer.dl_title_lbl, LangString_CLOUD_SAVING_NAME_FMT, shown.c_str());
        }
    }
    state.xfer.dl_job_done = request->succeeded.load(std::memory_order_acquire);
    state.xfer.dl_job_total = static_cast<int>(request->inputs.size()) - request->skipped.load(std::memory_order_acquire);
    const int percent = request->percent.load(std::memory_order_acquire);
    const int64_t now = esp_timer_get_time();
    if (state.download_painted_us == 0 || now - state.download_painted_us >= kDlProgUiIntervalUs ||
        (percent >= 99 && percent != state.xfer.dl_shown_percent)) {
        state.download_painted_us = now;
        RefreshDlProgressPage(percent);
    }
    if (stage == CloudResourceStage::Conflict && !state.dialogs.dialog_mask && current < request->inputs.size()) {
        const int index = FindItemIndexByTaskId(request->inputs[current].resource.task_id.c_str());
        if (index >= 0) {
            ShowSaveConflictDialog(index);
        }
    }
}

static int FindItemIndexByTaskId(const char* key) {
    if (key == nullptr || key[0] == '\0') {
        return -1;
    }
    auto& st = s_cloud;
    for (int i = 0; i < static_cast<int>(st.data.items.size()); ++i) {
        if (st.data.items[static_cast<size_t>(i)].task_id == key) {
            return i;
        }
    }
    return -1;
}

static void EraseItemAt(int idx) {
    auto& st = s_cloud;
    if (idx < 0 || idx >= static_cast<int>(st.data.items.size())) {
        return;
    }
    // 释放 RasterImage 前先清掉仍挂着 dsc 的缩略图，避免刷屏 LoadProhibited
    if (lv_obj_t* row = FindListRow(idx)) {
        if (lv_obj_t* thumb_box = FindRowChildBySize(row, kThumbW, kThumbH)) {
            lv_obj_clean(thumb_box);
        }
    }
    st.data.items.erase(st.data.items.begin() + idx);
    if (idx < static_cast<int>(st.data.thumbs.size())) {
        st.data.thumbs.erase(st.data.thumbs.begin() + idx);
    }
    if (idx < static_cast<int>(st.data.cover_cache.size())) {
        st.data.cover_cache.erase(st.data.cover_cache.begin() + idx);
    }
    if (idx < static_cast<int>(st.data.selected.size())) {
        st.data.selected.erase(st.data.selected.begin() + idx);
    }
    s_preview.OnItemErased(idx);
    RebuildFiltered();
    ClampPage();
}

static void ApplyCloudDownloadResult(const CloudResourceRequest& result) {
    auto& state = s_cloud;
    if (state.chrome.list_body) {
        lv_obj_clean(state.chrome.list_body);
    }
    bool unacknowledged = false;
    for (size_t i = 0; i < result.inputs.size(); ++i) {
        if (result.outcomes[i] == CloudResourceOutcome::Acknowledged) {
            EraseItemAt(FindItemIndexByTaskId(result.inputs[i].resource.task_id.c_str()));
        } else if (result.outcomes[i] == CloudResourceOutcome::Saved) {
            unacknowledged = true;
        }
    }
    state.xfer.download_busy = false;
    state.data.batch_busy = false;
    CloseDialog();
    HideDlProgressPage();
    ExitMultiModeEx(false, false);
    if (result.failed) {
        SetStatusTip(result.error[0] ? result.error : LangString_CLOUD_SAVE_FAIL, true);
    } else if (result.gate.IsCancelled()) {
        SetStatusTip(LangString_CLOUD_CANCELLED, true);
    } else if (unacknowledged) {
        SetStatusTip(LangString_CLOUD_SAVED_SYNC_FAIL, true);
    } else {
        SetStatusTip("", false);
    }
    RenderListPage();
    ResumeCloudSideHttpAfterDl();
}

static int CloudSyncRowHeight() {
    return std::max<int>(
        kSyncBtnH, ScreenTextHeight(UiFont()) + kSyncUnderlinePadBottom + kSyncUnderlineH + kSyncDividerW + UiSy(8));
}

static bool ShowBindingPanel();

static void ScheduleUserCloudSync() {
    if (ShowBindingPanel()) {
        return;
    }
    auto& st = s_cloud;
    if (s_preview.IsOpen()) {
        return;
    }
    if (st.xfer.download_busy || SyncBusy()) {
        return;
    }
    if (Board::GetInstance().IsWifiConfigMode()) {
        st.data.error = LangString_CLOUD_NEED_WIFI_CFG;
        RenderListPage();
        return;
    }
    st.data.fetch_done = false;
    ScheduleCloudFetch();
}

static void ShowObj(lv_obj_t* obj, bool on) {
    if (obj == nullptr) {
        return;
    }
    if (on) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ShowAppBody(bool on) {
    auto& st = s_cloud;
    ShowObj(st.chrome.header_row, on);
    ShowObj(st.chrome.filter_row, on);
    ShowObj(st.chrome.list_body, on);
    ShowObj(st.chrome.footer, on);
    ShowObj(st.chrome.page_lbl, on);
}

static bool ShowBindingPanel() {
    auto& st = s_cloud;
    if (st.data.mode != kCloudModeApp || !YunxinBinding::NeedsBinding()) {
        return false;
    }
    ShowAppBody(false);
    ShowObj(st.chrome.radio_status_lbl, false);
    if (!st.chrome.binding_panel) {
        st.chrome.binding_panel = YunxinBindingCreatePanel(st.chrome.list_frame, []() {
            auto& st = s_cloud;
            st.chrome.binding_panel = nullptr;
            if (!ScreenAlive() || st.data.mode != kCloudModeApp) {
                return;
            }
            ShowAppBody(true);
            ScheduleUserCloudSync();
        });
        if (!st.chrome.binding_panel) {
            if (!YunxinBinding::NeedsBinding()) {
                return false;
            }
            ScreenShowToast(LangString_COMMON_MEMORY_RETRY);
        }
    }
    return true;
}

static void FitApPanel() {
    auto& chrome = s_cloud.chrome;
    lv_obj_update_layout(chrome.ap_panel);
    const int height = lv_obj_get_content_height(chrome.ap_panel);
    const int width = lv_obj_get_content_width(chrome.ap_panel);
    int fixed = 0;
    for (auto* qr : {chrome.ap_qr_wifi, chrome.ap_qr_url}) {
        auto* frame = lv_obj_get_parent(qr);
        auto* step = lv_obj_get_parent(frame);
        int visible = 0;
        fixed += lv_obj_get_style_pad_top(step, LV_PART_MAIN) + lv_obj_get_style_pad_bottom(step, LV_PART_MAIN) +
                 lv_obj_get_style_border_width(step, LV_PART_MAIN);
        for (uint32_t i = 0; i < lv_obj_get_child_count(step); ++i) {
            auto* child = lv_obj_get_child(step, i);
            if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }
            ++visible;
            if (child != frame) {
                fixed += lv_obj_get_height(child);
            }
        }
        fixed += std::max(0, visible - 1) * lv_obj_get_style_pad_row(step, LV_PART_MAIN);
    }
    // 短屏按文字实测高度分配二维码空间；极端长文本保留纵向滚动。
    const int side =
        std::max(112, std::min<int>({240, width - 2 * kUiQrFrameInset, (height - fixed) / 2 - 2 * kUiQrFrameInset}));
    UiResizeQrCode(chrome.ap_qr_wifi, side);
    UiResizeQrCode(chrome.ap_qr_url, side);
}

static void PaintApPanel(const ApTransferInfo& info) {
    auto& st = s_cloud;
    if (st.chrome.ap_ssid_lbl != nullptr) {
        char line[64];
        std::snprintf(line, sizeof(line), LangString_CLOUD_AP_SSID_FMT, info.ssid);
        lv_label_set_text(st.chrome.ap_ssid_lbl, line);
    }
    if (st.chrome.ap_url_lbl != nullptr) {
        lv_label_set_text(st.chrome.ap_url_lbl, info.url);
    }
    if (st.chrome.ap_status_lbl != nullptr) {
        ShowObj(st.chrome.ap_status_lbl, false);
    }
    FitApPanel();
    char wifi_qr[64];
    std::snprintf(wifi_qr, sizeof(wifi_qr), "WIFI:T:nopass;S:%s;;", info.ssid);
    ApQrPaint(st.chrome.ap_qr_wifi, wifi_qr);
    ApQrPaint(st.chrome.ap_qr_url, info.url);
}

static void LeaveUsbMode() {
    CloudUsbPanelReset();
    auto& st = s_cloud;
    if (st.chrome.usb_panel != nullptr) {
        lv_obj_clean(st.chrome.usb_panel);
    }
}

static constexpr int kRadioClosed = static_cast<int>(TransferSessionMode::Closed);
static constexpr int kRadioStandby = static_cast<int>(TransferSessionMode::Standby);
static_assert(kCloudModeApp == static_cast<int>(TransferSessionMode::Cloud) &&
              kCloudModeAp == static_cast<int>(TransferSessionMode::Hotspot) &&
              kCloudModeUsb == static_cast<int>(TransferSessionMode::Usb) &&
              kCloudModeWifi == static_cast<int>(TransferSessionMode::Wifi));
static uint32_t s_radio_generation = 0;
static uint32_t s_radio_applied = 0;

static void SetRadioStatus(const char* text) {
    if (text == LangString_WIFI_DISABLED_RETRY) {
        ScreenShowToast(text);
    }
    auto& st = s_cloud;
    if (st.chrome.radio_status_lbl == nullptr) {
        return;
    }
    const bool on = text != nullptr && text[0] != '\0';
    if (on) {
        lv_label_set_text(st.chrome.radio_status_lbl, text);
        lv_obj_align(st.chrome.radio_status_lbl, LV_ALIGN_CENTER, 0, 0);
        lv_obj_move_foreground(st.chrome.radio_status_lbl);
    }
    ShowObj(st.chrome.radio_status_lbl, on);
}

static void PollCloudTransferSession() {
    const auto local = TransferSessionRead();
    if (!local.ready || local.generation != s_radio_generation || local.generation == s_radio_applied ||
        !ScreenAlive()) {
        return;
    }
    s_radio_applied = local.generation;
    const int mode = static_cast<int>(local.mode);
    auto& st = s_cloud;
    if (st.data.mode != mode) {
        return;
    }
    if (mode == kCloudModeAp) {
        if (!local.ok) {
            ShowObj(st.chrome.ap_panel, false);
            const char* fail = local.info.fail != nullptr ? local.info.fail : LangString_CLOUD_AP_FAIL;
            SetRadioStatus(fail);
            return;
        }
        ShowObj(st.chrome.ap_panel, true);
        SetRadioStatus(nullptr);
        PaintApPanel(local.info);
        return;
    }
    if (mode == kCloudModeWifi) {
        SetRadioStatus(nullptr);
        ShowObj(st.chrome.wifi_panel, true);
        NetworkScreen::BuildTransfer(st.chrome.wifi_panel);
        return;
    }
    if (mode != kCloudModeApp) {
        return;
    }
    if (!local.ok) {
        ShowAppBody(false);
        const char* fail = local.info.fail != nullptr ? local.info.fail : LangString_CLOUD_NET_NOT_READY;
        SetRadioStatus(fail);
        return;
    }
    SetRadioStatus(nullptr);
    if (ShowBindingPanel()) {
        return;
    }
    ShowAppBody(true);
    if (!st.data.fetch_done && !st.data.loading && !st.data.waiting_net) {
        ScheduleUserCloudSync();
    } else {
        RenderListPage();
        ScheduleCoverThumbFill();
    }
}

static void KickRadio(int mode) {
    if ((mode == kCloudModeAp || mode == kCloudModeWifi) && !WifiStation::GetInstance().IsUserEnabled()) {
        SetRadioStatus(LangString_WIFI_DISABLED_RETRY);
        return;
    }
    s_radio_generation = TransferSessionRequest(static_cast<TransferSessionMode>(mode));
    if (s_radio_generation == 0) {
        SetRadioStatus(LangString_CLOUD_TASK_FAIL);
    }
}

static void CloudRadioAbandon() {
    KickRadio(kRadioClosed);
}

static void ParkApForStandby() {
    auto& st = s_cloud;
    if (st.chrome.screen == nullptr || st.data.mode != kCloudModeAp) {
        return;
    }
    // 待机先阻止旧启动继续接收请求，关闭和任务回收仍由射频任务完成。
    KickRadio(kRadioStandby);
}

static void ReloadApAfterStandby() {
    auto& st = s_cloud;
    if (st.chrome.screen == nullptr || st.data.mode != kCloudModeAp) {
        ApTransferAllowStart();
        return;
    }
    ApTransferAllowStart();
    EnterCloudMode(kCloudModeAp);
}

static void EnterCloudMode(int mode) {
    auto& st = s_cloud;
    const int prev = st.data.mode;
    if (prev != mode && prev == kCloudModeApp) {
        RequestStopTransfer("mode_change");
    }
    if (prev == kCloudModeWifi) {
        NetworkScreen::CloseTransfer();
    }
    if (prev == kCloudModeUsb && mode != kCloudModeUsb) {
        LeaveUsbMode();
    }
    if (st.chrome.binding_panel) {
        lv_obj_delete(st.chrome.binding_panel);
        st.chrome.binding_panel = nullptr;
    }
    st.data.mode = mode;
    ShowAppBody(false);
    ShowObj(st.chrome.ap_panel, false);
    ShowObj(st.chrome.wifi_panel, false);
    ShowObj(st.chrome.usb_panel, mode == kCloudModeUsb);
    RefreshTabUi();
    auto* dual = dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
    if ((mode == kCloudModeAp || mode == kCloudModeWifi) && dual != nullptr &&
        dual->GetNetworkType() == NetworkType::CELLULAR) {
        CloudRadioAbandon();
        SetRadioStatus(LangString_CLOUD_CELLULAR_NEED_WIFI);
        return;
    }
    if (mode == kCloudModeAp || mode == kCloudModeWifi) {
        SetRadioStatus(mode == kCloudModeAp ? LangString_CLOUD_AP_OPENING : LangString_CLOUD_WIFI_PREPARING);
        KickRadio(mode);
        return;
    }
    if (mode == kCloudModeUsb) {
        SetRadioStatus(nullptr);
        if (st.chrome.usb_panel != nullptr && prev != kCloudModeUsb) {
            lv_obj_clean(st.chrome.usb_panel);
            CloudUsbPanelBuild(st.chrome.usb_panel);
        }
        KickRadio(mode);
        return;
    }
    SetRadioStatus(LangString_CLOUD_CONNECTING_NET);
    ShowBindingPanel();
    KickRadio(mode);
}

static void OnTabClicked(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto& st = s_cloud;
    // Tab 仅本地筛选；下载进度页打开时不可切
    if (st.xfer.dl_page_open || s_preview.IsOpen() || st.dialogs.dialog_mask != nullptr ||
        st.dialogs.action_mask != nullptr) {
        return;
    }
    const int tab = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (tab < 0 || tab >= kTabCount || tab == st.data.mode) {
        return;
    }
    EnterCloudMode(tab);
}

static void OnFilterClicked(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto& st = s_cloud;
    if (st.data.mode != kCloudModeApp || st.xfer.dl_page_open || s_preview.IsOpen() || st.dialogs.dialog_mask != nullptr ||
        st.dialogs.action_mask != nullptr) {
        return;
    }
    const int tab = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (tab < 0 || tab >= kFilterTabCount || tab == st.data.filter_tab) {
        return;
    }
    st.data.filter_tab = tab;
    st.data.page = 0;
    RefreshFilterUi();
    RefreshSectionTitle();
    RenderListPage();
}

static lv_obj_t* MakeTabBtn(lv_obj_t* parent, const char* text, int tab) {
    auto& st = s_cloud;
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, lv_pct(100));
    lv_obj_set_style_pad_all(btn, 0, 0);
    Cloud_DisableScroll(btn);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, OnTabClicked, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(tab)));
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, UiFont(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    st.chrome.tab_btns[tab] = btn;
    st.chrome.tab_lbls[tab] = lbl;
    return btn;
}

static void OnSyncClicked(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto& st = s_cloud;
    if (s_preview.IsOpen() || st.data.multi) {
        return;
    }
    if (st.xfer.download_busy || st.data.batch_busy || st.data.delete_busy || SyncBusy() ||
        CloudResourcesBusy()) {
        SetStatusTip(LangString_CLOUD_PLEASE_WAIT, true);
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (s_last_user_sync_us != 0 && (now - s_last_user_sync_us) < kSyncThrottleUs) {
        SetStatusTip(LangString_CLOUD_PLEASE_WAIT, true);
        return;
    }
    s_last_user_sync_us = now;
    ScheduleUserCloudSync();
}

static void BuildCloudMainChrome(lv_obj_t* scr, lv_coord_t status_height) {
    auto& st = s_cloud;
    // 为贴底页码留出 kFooterOutsideH，列表区可多挤一行
    const lv_coord_t body_h = LV_VER_RES - status_height - kFooterOutsideH;

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, body_h);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status_height);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(body, kPad, 0);
    lv_obj_set_style_pad_bottom(body, UiSy(4), 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(body, kRowGap, 0);
    Cloud_DisableScroll(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);
    st.chrome.main_body = body;

    lv_obj_t* tab_row = lv_obj_create(body);
    lv_obj_remove_style_all(tab_row);
    lv_obj_set_width(tab_row, Cloud_ContentWidth());
    lv_obj_set_height(tab_row, std::max<int>(kTabH, ScreenTextHeight(UiFont(UiFontRole::Title)) + UiSy(16)));
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
    Cloud_DisableScroll(tab_row);
    lv_obj_clear_flag(tab_row, LV_OBJ_FLAG_CLICKABLE);
    MakeTabBtn(tab_row, LangString_CLOUD_TAB_APP, kCloudModeApp);
    MakeTabBtn(tab_row, LangString_CLOUD_TAB_AP, kCloudModeAp);
    MakeTabBtn(tab_row, LangString_CLOUD_TAB_WIFI, kCloudModeWifi);
    MakeTabBtn(tab_row, LangString_CLOUD_TAB_USB, kCloudModeUsb);
    RefreshTabUi();

    lv_obj_t* header_row = lv_obj_create(body);
    st.chrome.header_row = header_row;
    lv_obj_remove_style_all(header_row);
    lv_obj_set_width(header_row, Cloud_ContentWidth());
    lv_obj_set_height(header_row, std::max<int>(kHeaderH, UiFont(UiFontRole::TitleEmphasis)->line_height));
    lv_obj_set_style_bg_opa(header_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(header_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header_row, kHeaderGap, 0);
    Cloud_DisableScroll(header_row);
    lv_obj_clear_flag(header_row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* star_wrap = lv_obj_create(header_row);
    lv_obj_remove_style_all(star_wrap);
    lv_obj_set_size(star_wrap, kHeaderIcon, kHeaderIcon);
    lv_obj_set_style_bg_color(star_wrap, UiColorForeground(), 0);
    lv_obj_set_style_bg_opa(star_wrap, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(star_wrap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(star_wrap, 0, 0);
    Cloud_DisableScroll(star_wrap);
    lv_obj_clear_flag(star_wrap, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* star = lv_label_create(star_wrap);
    lv_label_set_text(star, "\xE2\x98\x85");  // ★
    lv_obj_set_style_text_color(star, UiColorBackground(), 0);
    lv_obj_set_style_text_font(star, UiFont(), 0);
    lv_obj_center(star);
    lv_obj_clear_flag(star, LV_OBJ_FLAG_CLICKABLE);

    st.chrome.section_title = lv_label_create(header_row);
    lv_label_set_text(st.chrome.section_title, LangString_CLOUD_PENDING);
    lv_obj_set_style_text_font(st.chrome.section_title, UiFont(UiFontRole::TitleEmphasis), 0);
    lv_obj_set_style_text_color(st.chrome.section_title, UiColorForeground(), 0);
    lv_obj_clear_flag(st.chrome.section_title, LV_OBJ_FLAG_CLICKABLE);

    st.chrome.status_lbl = lv_label_create(header_row);
    lv_label_set_text(st.chrome.status_lbl, "");
    lv_obj_set_style_pad_left(st.chrome.status_lbl, kStatusGap, 0);
    lv_obj_set_style_text_font(st.chrome.status_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(st.chrome.status_lbl, UiColorForeground(), 0);
    lv_obj_clear_flag(st.chrome.status_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(st.chrome.status_lbl, LV_OBJ_FLAG_HIDDEN);

    // 传输内容共用细外框；App 列表的页码贴底放在框外
    lv_obj_t* list_frame = lv_obj_create(body);
    st.chrome.list_frame = list_frame;
    lv_obj_remove_style_all(list_frame);
    lv_obj_set_width(list_frame, Cloud_ContentWidth());
    lv_obj_set_flex_grow(list_frame, 1);
    lv_obj_set_style_bg_color(list_frame, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(list_frame, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(list_frame, kListFrameBorderW, 0);
    lv_obj_set_style_border_color(list_frame, UiColorForeground(), 0);
    lv_obj_set_style_radius(list_frame, kListFrameRadius, 0);
    lv_obj_set_style_pad_all(list_frame, kListFramePad, 0);
    lv_obj_set_flex_flow(list_frame, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_frame, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list_frame, 0, 0);
    Cloud_DisableScroll(list_frame);
    lv_obj_clear_flag(list_frame, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* filter_row = lv_obj_create(list_frame);
    st.chrome.filter_row = filter_row;
    lv_obj_remove_style_all(filter_row);
    lv_obj_set_width(filter_row, lv_pct(100));
    const auto* selected_font = UiFont(UiFontRole::Emphasis);
    const int filter_height = (selected_font ? selected_font : UiFont())->line_height + kTabBorderW +
                              kSyncUnderlinePadBottom + 2 * UiSy(6) + kSyncDividerW;
    lv_obj_set_height(filter_row, std::max<int>(kFilterH, filter_height));
    lv_obj_set_style_border_side(filter_row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(filter_row, kSyncDividerW, 0);
    lv_obj_set_style_border_color(filter_row, UiColorForeground(), 0);
    lv_obj_set_flex_flow(filter_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(filter_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    Cloud_DisableScroll(filter_row);
    lv_obj_clear_flag(filter_row, LV_OBJ_FLAG_CLICKABLE);
    const char* filter_names[kFilterTabCount] = {
        LangString_CLOUD_TAB_ALL,
        LangString_CLOUD_TAB_WALLPAPER,
        LangString_CLOUD_TAB_BOOK,
        LangString_CLOUD_TAB_FONT,
    };
    for (int i = 0; i < kFilterTabCount; ++i) {
        lv_obj_t* btn = lv_obj_create(filter_row);
        lv_obj_remove_style_all(btn);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, lv_pct(100));
        Cloud_DisableScroll(btn);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(btn);
        lv_obj_add_event_cb(btn, OnFilterClicked, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, filter_names[i]);
        lv_obj_set_style_text_color(lbl, UiColorForeground(), 0);
        lv_obj_center(lbl);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        st.chrome.filter_btns[i] = btn;
        st.chrome.filter_lbls[i] = lbl;
    }
    RefreshFilterUi();

    lv_obj_t* list_body = lv_obj_create(list_frame);
    lv_obj_remove_style_all(list_body);
    lv_obj_set_width(list_body, lv_pct(100));
    lv_obj_set_style_pad_top(list_body, 2, 0);
    lv_obj_set_height(list_body, 0);
    lv_obj_set_flex_grow(list_body, 1);
    Cloud_DisableScroll(list_body);
    st.chrome.list_body = list_body;

    lv_obj_t* footer = lv_obj_create(list_frame);
    lv_obj_remove_style_all(footer);
    lv_obj_set_width(footer, lv_pct(100));
    lv_obj_set_height(footer, CloudSyncRowHeight());
    lv_obj_set_style_bg_opa(footer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(footer, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(footer, kSyncDividerW, 0);
    lv_obj_set_style_border_color(footer, UiColorForeground(), 0);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    Cloud_DisableScroll(footer);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_CLICKABLE);
    st.chrome.footer = footer;

    // 分割线 + 居中「刷新」下划线（多选时隐藏，换 multi_bar）
    st.chrome.sync_btn = lv_obj_create(footer);
    lv_obj_remove_style_all(st.chrome.sync_btn);
    lv_obj_set_width(st.chrome.sync_btn, lv_pct(100));
    lv_obj_set_height(st.chrome.sync_btn, lv_pct(100));
    lv_obj_set_style_bg_opa(st.chrome.sync_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(st.chrome.sync_btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(st.chrome.sync_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    Cloud_DisableScroll(st.chrome.sync_btn);
    lv_obj_add_flag(st.chrome.sync_btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(st.chrome.sync_btn);
    lv_obj_add_event_cb(st.chrome.sync_btn, OnSyncClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* sync_text = lv_obj_create(st.chrome.sync_btn);
    lv_obj_remove_style_all(sync_text);
    lv_obj_set_size(sync_text, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_border_side(sync_text, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(sync_text, kSyncUnderlineH, 0);
    lv_obj_set_style_border_color(sync_text, UiColorForeground(), 0);
    lv_obj_set_style_pad_bottom(sync_text, kSyncUnderlinePadBottom, 0);
    lv_obj_set_style_pad_hor(sync_text, kSyncUnderlinePadHor, 0);
    lv_obj_set_style_bg_opa(sync_text, LV_OPA_TRANSP, 0);
    Cloud_DisableScroll(sync_text);
    lv_obj_clear_flag(sync_text, LV_OBJ_FLAG_CLICKABLE);

    st.chrome.sync_lbl = lv_label_create(sync_text);
    lv_label_set_text(st.chrome.sync_lbl, LangString_CLOUD_REFRESH);
    lv_obj_set_style_text_font(st.chrome.sync_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(st.chrome.sync_lbl, UiColorForeground(), 0);
    lv_obj_clear_flag(st.chrome.sync_lbl, LV_OBJ_FLAG_CLICKABLE);

    st.chrome.multi_bar = lv_obj_create(footer);
    lv_obj_remove_style_all(st.chrome.multi_bar);
    lv_obj_set_width(st.chrome.multi_bar, lv_pct(100));
    lv_obj_set_height(st.chrome.multi_bar, lv_pct(100));
    lv_obj_set_style_bg_opa(st.chrome.multi_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(st.chrome.multi_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st.chrome.multi_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(st.chrome.multi_bar, kMultiBarGap, 0);
    Cloud_DisableScroll(st.chrome.multi_bar);
    lv_obj_clear_flag(st.chrome.multi_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(st.chrome.multi_bar, LV_OBJ_FLAG_HIDDEN);

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
        Cloud_DisableScroll(btn);
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
        Cloud_DisableScroll(text_wrap);
        lv_obj_clear_flag(text_wrap, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* lbl = lv_label_create(text_wrap);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, UiFont(), 0);
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
        Cloud_DisableScroll(dot);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        return dot;
    };
    make_multi_action(st.chrome.multi_bar, LangString_COMMON_CANCEL, OnMultiCancel);
    make_multi_dot(st.chrome.multi_bar);
    make_multi_action(st.chrome.multi_bar, LangString_COMMON_SELECT_ALL, OnMultiSelectAll);
    make_multi_dot(st.chrome.multi_bar);
    make_multi_action(st.chrome.multi_bar, LangString_COMMON_DELETE, OnMultiDelete);
    make_multi_dot(st.chrome.multi_bar);
    make_multi_action(st.chrome.multi_bar, LangString_CLOUD_SAVE, OnMultiSave);

    // 页码贴底，对齐壁纸页
    st.chrome.page_lbl = lv_label_create(scr);
    lv_label_set_text(st.chrome.page_lbl, "");
    lv_obj_set_width(st.chrome.page_lbl, LV_HOR_RES - kPageLblMargin);
    lv_obj_set_style_text_align(st.chrome.page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(st.chrome.page_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(st.chrome.page_lbl, UiColorForeground(), 0);
    lv_obj_align(st.chrome.page_lbl, LV_ALIGN_BOTTOM_MID, 0, -kPageLblBottom);
    lv_obj_clear_flag(st.chrome.page_lbl, LV_OBJ_FLAG_CLICKABLE);
    Cloud_DisableScroll(st.chrome.page_lbl);

    st.chrome.radio_status_lbl = lv_label_create(list_frame);
    lv_label_set_text(st.chrome.radio_status_lbl, "");
    lv_label_set_long_mode(st.chrome.radio_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(st.chrome.radio_status_lbl, Cloud_ContentWidth() - kListFramePad * 2);
    lv_obj_set_style_text_align(st.chrome.radio_status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(st.chrome.radio_status_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(st.chrome.radio_status_lbl, UiColorForeground(), 0);
    lv_obj_add_flag(st.chrome.radio_status_lbl, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(st.chrome.radio_status_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(st.chrome.radio_status_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(st.chrome.radio_status_lbl, LV_OBJ_FLAG_HIDDEN);
    Cloud_DisableScroll(st.chrome.radio_status_lbl);

    // 本地传输面板共用列表外框
    lv_obj_t* ap_panel = lv_obj_create(list_frame);
    st.chrome.ap_panel = ap_panel;
    lv_obj_remove_style_all(ap_panel);
    lv_obj_set_width(ap_panel, lv_pct(100));
    lv_obj_set_height(ap_panel, 0);
    lv_obj_set_flex_grow(ap_panel, 1);
    lv_obj_set_flex_flow(ap_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ap_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ap_panel, 0, 0);
    Cloud_DisableScroll(ap_panel);
    lv_obj_add_flag(ap_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(ap_panel, LV_DIR_VER);
    lv_obj_add_flag(ap_panel, LV_OBJ_FLAG_HIDDEN);

    auto make_step = [](lv_obj_t* parent) {
        lv_obj_t* half = lv_obj_create(parent);
        lv_obj_remove_style_all(half);
        lv_obj_set_width(half, lv_pct(100));
        lv_obj_set_height(half, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(half, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(half, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(half, kRowGap, 0);
        Cloud_DisableScroll(half);
        lv_obj_clear_flag(half, LV_OBJ_FLAG_CLICKABLE);
        return half;
    };
    auto add_center = [](lv_obj_t* parent, const char* text, bool bold) {
        lv_obj_t* lbl = lv_label_create(parent);
        lv_label_set_text(lbl, text);
        lv_obj_set_width(lbl, lv_pct(100));
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        const auto* font = UiFont(bold ? UiFontRole::Emphasis : UiFontRole::Body);
        lv_obj_set_style_text_font(lbl, font, 0);
        lv_obj_set_style_text_color(lbl, UiColorForeground(), 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        return lbl;
    };
    lv_obj_t* ap_top = make_step(ap_panel);
    lv_obj_set_style_pad_top(ap_top, kListFramePad, 0);
    add_center(ap_top, LangString_CLOUD_AP_STEP1, true);
    add_center(ap_top, LangString_CLOUD_AP_JOIN, false);
    st.chrome.ap_qr_wifi = ApQrCreate(ap_top);
    st.chrome.ap_ssid_lbl = add_center(ap_top, "", false);
    st.chrome.ap_status_lbl = add_center(ap_top, "", false);
    lv_obj_add_flag(st.chrome.ap_status_lbl, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* ap_bot = make_step(ap_panel);
    lv_obj_set_style_border_side(ap_bot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(ap_bot, kSyncDividerW, 0);
    lv_obj_set_style_border_color(ap_bot, UiColorForeground(), 0);
    lv_obj_set_style_pad_top(ap_bot, kListFramePad * 2, 0);
    add_center(ap_bot, LangString_CLOUD_AP_STEP2, true);
    add_center(ap_bot, LangString_CLOUD_AP_OPEN, false);
    st.chrome.ap_url_lbl = add_center(ap_bot, "", false);
    add_center(ap_bot, LangString_CLOUD_AP_HINT, false);
    st.chrome.ap_qr_url = ApQrCreate(ap_bot);

    lv_obj_t* wifi_panel = lv_obj_create(list_frame);
    st.chrome.wifi_panel = wifi_panel;
    lv_obj_remove_style_all(wifi_panel);
    lv_obj_set_width(wifi_panel, lv_pct(100));
    lv_obj_set_height(wifi_panel, 0);
    lv_obj_set_flex_grow(wifi_panel, 1);
    Cloud_DisableScroll(wifi_panel);
    lv_obj_clear_flag(wifi_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(wifi_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* usb_panel = lv_obj_create(list_frame);
    st.chrome.usb_panel = usb_panel;
    lv_obj_remove_style_all(usb_panel);
    lv_obj_set_width(usb_panel, lv_pct(100));
    lv_obj_set_flex_grow(usb_panel, 1);
    lv_obj_set_flex_flow(usb_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(usb_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(usb_panel, kPanelGap, 0);
    lv_obj_set_style_pad_hor(usb_panel, UiSx(24), 0);
    Cloud_DisableScroll(usb_panel);
    lv_obj_add_flag(usb_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_update_layout(scr);
    st.data.page_size =
        std::max<int>(1, (lv_obj_get_content_height(list_body) + UiSy(4)) / (CloudRowHeight() + UiSy(4)));
}

static UiPageLifecycle s_page;

static void CancelCloudRequest() {
    auto& state = s_cloud;
    CloudCancelResourceRequest(state.request);
    state.request.reset();
    state.data.waiting_net = false;
    state.data.loading = false;
    CancelCloudImageViews();
}

static void PollCloudPage() {
    auto& state = s_cloud;
    PollCloudTransferSession();
    PollCloudImages();
    if (ApTransferNeedsStorageRestart()) {
        ShowStorageErrorDialog();
    }
    if (!state.request) {
        if (state.data.waiting_net && !CloudResourcesBusy()) {
            ScheduleCloudFetch();
        }
        return;
    }
    if (!state.request->completed.load(std::memory_order_acquire)) {
        RefreshCloudDownload();
        return;
    }
    const auto request = std::move(state.request);
    if (!OpStillValid(state.request_generation)) {
        return;
    }
    if (request->operation == CloudResourceOperation::Fetch) {
        ApplyCloudFetchResult(*request);
    } else if (request->operation == CloudResourceOperation::Delete) {
        ApplyCloudDeleteResult(*request);
    } else {
        ApplyCloudDownloadResult(*request);
    }
}

static void InvalidateSession() {
    auto& st = s_cloud;
    CancelCloudRequest();
    if (st.chrome.list_body) {
        lv_obj_clean(st.chrome.list_body);
    }
    s_preview.Reset();
    NetworkScreen::CloseTransfer();
    CloudRadioAbandon();
    CloudUsbPanelReset();
    CancelStatusClearTimer();
    ++st.xfer.op_generation;
    ++st.data.thumb_epoch;
    st.data.thumbs.clear();
    st.data.filtered.clear();
    st.data.selected.clear();
    st.data.cover_cache.clear();
    st.data.status_text[0] = '\0';
    st.data.multi = false;
    st.data.batch_busy = false;
    st.data.delete_busy = false;
    st.data.suppress_row_click_until_us = 0;
    st.data.suppress_row_click_idx = -1;
    st.data.loading = false;
    st.data.waiting_net = false;
    st.data.covers_loading = false;
    st.xfer.download_busy = false;
    st.xfer.dl_job_total = 0;
    st.xfer.dl_job_done = 0;
    CloseDialog();
    HideDlProgressPage();
    st.xfer.pending_index = -1;
    st.xfer.action_index = -1;
}

static void ApplyStopTransferUi() {
    auto& st = s_cloud;
    const bool was_busy = st.xfer.download_busy || st.data.batch_busy || st.data.delete_busy || st.xfer.dl_page_open ||
                          st.xfer.pending_index >= 0;
    if (was_busy) {
        ++st.xfer.op_generation;
    }
    st.xfer.download_busy = false;
    st.data.batch_busy = false;
    st.data.delete_busy = false;
    st.xfer.dl_job_total = 0;
    st.xfer.dl_job_done = 0;
    CloseDialog();
    HideDlProgressPage();
    if (!ScreenAlive() || !was_busy) {
        return;
    }
    if (s_preview.IsOpen()) {
        return;
    }
    RenderListPage();
}

static void RequestStopTransfer(const char* /*why*/) {
    CancelCloudRequest();
    ApplyStopTransferUi();
}

static bool Cloud_OnVkKey(const char* key) {
    if (key == nullptr) {
        return true;
    }
    auto& st = s_cloud;

    const bool home = std::strcmp(key, "vk_home") == 0;
    if (st.data.mode == kCloudModeWifi) {
        if (NetworkScreen::OnVkKey(key)) {
            return true;
        }
        if (home || std::strcmp(key, "vk_prev") == 0) {
            RequestStopTransfer(home ? "wifi_vk_home" : "wifi_vk_prev");
            ScreenLvAsync([](void*) {
                if (ScreenAlive()) {
                    ScreenNavigateBack();
                }
            });
        }
        return true;
    }
    if (st.xfer.pending_index >= 0) {
        if (home || std::strcmp(key, "vk_prev") == 0) {
            AbortDownloadJobFromUi();
        }
        return true;
    }
    if (st.xfer.dl_page_open) {
        if (home || std::strcmp(key, "vk_prev") == 0) {
            ScreenLvAsync([](void*) { AbortDownloadJobFromUi(); });
            return true;
        }
        return true;
    }
    if (s_preview.IsOpen()) {
        if (home || std::strcmp(key, "vk_prev") == 0) {
            ScreenLvAsync([](void*) { CloseWallpaperPreview(); });
            return true;
        }
        if (std::strcmp(key, "vk_next") == 0) {
            return true;
        }
        return true;
    }
    if (home) {
        RequestStopTransfer("vk_home");
        ScreenNavigateBack();
        return true;
    }
    // 等网/拉列表中仍可翻页看已有数据；离页再 StopTransfer

    if (std::strcmp(key, "vk_prev") == 0) {
        if (st.data.page > 0) {
            --st.data.page;
            RequestCloudRender();
            return true;
        }
        if (st.data.multi) {
            if (st.data.batch_busy || st.xfer.download_busy || st.data.delete_busy) {
                return true;
            }
            ScreenLvAsync([](void*) {
                if (ScreenAlive()) {
                    ExitMultiMode(true);
                }
            });
            return true;
        }
        RequestStopTransfer("vk_prev");
        ScreenLvAsync([](void*) {
            if (!ScreenAlive()) {
                return;
            }
            const char* active = VkKey_ActiveScreen();
            if (active == nullptr || std::strcmp(active, kScreenId) != 0) {
                ESP_LOGI(TAG, "vk_prev skip stale back: active=%s screen=%s", active != nullptr ? active : "null",
                         kScreenId);
                return;
            }
            ScreenNavigateBack();
        });
        return true;
    }
    if (std::strcmp(key, "vk_next") == 0) {
        if (st.data.page + 1 < PageCount()) {
            ++st.data.page;
            RequestCloudRender();
        }
        return true;
    }
    return true;
}

static bool CloudPageRepeatStep(int page_delta) {
    auto& st = s_cloud;
    // 翻页看已有列表，不依赖 SyncBusy；等网中空列表 PageCount=1 自然不动
    if (page_delta == 0 || s_preview.IsOpen() || st.dialogs.dialog_mask != nullptr || st.dialogs.action_mask != nullptr) {
        return false;
    }
    const int last = std::max(0, PageCount() - 1);
    int next = st.data.page + page_delta;
    if (next < 0) {
        next = 0;
    } else if (next > last) {
        next = last;
    }
    if (next == st.data.page) {
        return false;
    }
    st.data.page = next;
    RequestCloudRender();
    return page_delta < 0 ? st.data.page > 0 : st.data.page < last;
}

static bool Cloud_OnPageKey(const char* key, bool long_press) {
    auto& st = s_cloud;
    if (st.data.mode != kCloudModeApp || s_preview.IsOpen() || st.xfer.pending_index >= 0 || st.xfer.dl_page_open) {
        return false;
    }
    if (long_press) {
        VkPageRepeatTryStart(key, CloudPageRepeatStep);
    } else {
        CloudPageRepeatStep(std::strcmp(key, "vk_prev") == 0 ? -1 : 1);
    }
    return true;
}

static bool Cloud_OnVkKeyLongPress(const char* key) {
    if (s_cloud.data.mode != kCloudModeApp) {
        return false;
    }
    return VkPageRepeatTryStart(key, CloudPageRepeatStep);
}

static bool Cloud_OnVkKeyPressUp(const char* key) {
    return VkPageRepeatOnPressUp(key);
}

static void Cloud_OnScreenDeleted(lv_event_t* e) {
    auto& st = s_cloud;
    if (lv_event_get_target(e) != st.chrome.screen) {
        return;
    }

    ScreenPaintCoalesceReset(&s_cloud_paint);
    ScreenPaintCoalesceReset(&s_cloud_check_paint);
    InvalidateSession();
    if (auto* disp = Board::GetInstance().GetDisplay()) {
        disp->SetIdleStatusMode(IdleStatusMode::kClock);
    }
    s_preview.Reset();
    st.chrome.screen = nullptr;
    st.chrome.main_body = nullptr;
    st.chrome.list_body = nullptr;
    st.chrome.footer = nullptr;
    st.chrome.sync_btn = nullptr;
    st.chrome.sync_lbl = nullptr;
    st.chrome.multi_bar = nullptr;
    st.chrome.page_lbl = nullptr;
    st.chrome.radio_status_lbl = nullptr;
    st.chrome.section_title = nullptr;
    st.chrome.status_lbl = nullptr;
    st.chrome.header_row = nullptr;
    st.chrome.list_frame = nullptr;
    st.chrome.binding_panel = nullptr;
    st.chrome.wifi_panel = nullptr;
    st.chrome.ap_panel = nullptr;
    st.chrome.ap_ssid_lbl = nullptr;
    st.chrome.ap_url_lbl = nullptr;
    st.chrome.ap_status_lbl = nullptr;
    st.chrome.ap_qr_wifi = nullptr;
    st.chrome.ap_qr_url = nullptr;
    st.chrome.usb_panel = nullptr;
    st.chrome.filter_row = nullptr;
    for (int i = 0; i < kTabCount; ++i) {
        st.chrome.tab_btns[i] = nullptr;
        st.chrome.tab_lbls[i] = nullptr;
    }
    for (int i = 0; i < kFilterTabCount; ++i) {
        st.chrome.filter_btns[i] = nullptr;
        st.chrome.filter_lbls[i] = nullptr;
    }
    st.dialogs.storage_error_mask = nullptr;
    st.dialogs.dialog_mask = nullptr;
    st.dialogs.action_mask = nullptr;
    st.xfer.dl_page = nullptr;
    st.xfer.dl_bar = nullptr;
    st.xfer.dl_pct_lbl = nullptr;
    st.xfer.dl_count_lbl = nullptr;
    st.xfer.dl_title_lbl = nullptr;
    st.xfer.dl_page_open = false;
}

static void Cloud_RebootAfterStorageError() {
    TaskExecutor::Schedule([] {
        ApTransferStop(false);
        SystemRequestReboot();
    });
}

lv_obj_t* CloudScreen::Create() {
    auto& st = s_cloud;
    InvalidateSession();
    CancelStatusClearTimer();
    st.data.items.clear();
    st.data.filtered.clear();
    st.data.selected.clear();
    st.data.thumbs.clear();
    st.data.cover_cache.clear();
    ++st.data.thumb_epoch;
    st.data.error.clear();
    st.data.status_text[0] = '\0';
    st.data.page = 0;
    st.data.mode = kCloudModeAp;
    st.data.fetch_done = false;
    st.data.covers_loading = false;
    st.data.multi = false;
    st.data.batch_busy = false;
    st.data.delete_busy = false;
    st.data.suppress_row_click_until_us = 0;
    st.data.suppress_row_click_idx = -1;
    st.chrome.main_body = nullptr;
    st.chrome.list_body = nullptr;
    st.chrome.footer = nullptr;
    st.chrome.sync_btn = nullptr;
    st.chrome.sync_lbl = nullptr;
    st.chrome.multi_bar = nullptr;
    st.chrome.page_lbl = nullptr;
    st.chrome.radio_status_lbl = nullptr;
    st.chrome.section_title = nullptr;
    st.chrome.status_lbl = nullptr;
    st.chrome.header_row = nullptr;
    st.chrome.list_frame = nullptr;
    st.chrome.binding_panel = nullptr;
    st.chrome.wifi_panel = nullptr;
    st.chrome.ap_panel = nullptr;
    st.chrome.ap_ssid_lbl = nullptr;
    st.chrome.ap_url_lbl = nullptr;
    st.chrome.ap_status_lbl = nullptr;
    st.chrome.ap_qr_wifi = nullptr;
    st.chrome.ap_qr_url = nullptr;
    st.chrome.usb_panel = nullptr;
    st.chrome.filter_row = nullptr;
    for (int i = 0; i < kTabCount; ++i) {
        st.chrome.tab_btns[i] = nullptr;
        st.chrome.tab_lbls[i] = nullptr;
    }
    for (int i = 0; i < kFilterTabCount; ++i) {
        st.chrome.filter_btns[i] = nullptr;
        st.chrome.filter_lbls[i] = nullptr;
    }
    st.dialogs.storage_error_mask = nullptr;
    st.dialogs.dialog_mask = nullptr;
    st.dialogs.action_mask = nullptr;
    st.xfer.dl_page = nullptr;
    st.xfer.dl_bar = nullptr;
    st.xfer.dl_pct_lbl = nullptr;
    st.xfer.dl_count_lbl = nullptr;
    st.xfer.dl_title_lbl = nullptr;
    st.xfer.dl_page_open = false;
    st.xfer.dl_job_total = 0;
    st.xfer.dl_job_done = 0;
    st.xfer.dl_shown_percent = -1;

    ScreenSetIsHome(false);

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, UiFont(), 0);
    lv_obj_set_style_text_color(scr, UiColorForeground(), 0);
    st.chrome.screen = scr;
    lv_obj_add_event_cb(scr, Cloud_OnScreenDeleted, LV_EVENT_DELETE, nullptr);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    // 顶栏保持时钟（勿改成「传输」）
    if (auto* disp = Board::GetInstance().GetDisplay()) {
        disp->SetIdleStatusMode(IdleStatusMode::kClock);
    }
    if (status.notification_label) {
        lv_obj_add_flag(status.notification_label, LV_OBJ_FLAG_HIDDEN);
    }

    s_last_user_sync_us = 0;
    BuildCloudMainChrome(scr, status.height);
    s_preview.Build(scr, status.height, Cloud_ContentWidth(), StartWallpaperDownload);
    EnterCloudMode(kCloudModeAp);

    VkKey_AttachScreen(scr, kScreenId,
                       VkKeyScreenDesc{CloudScreen::Create, Cloud_OnVkKey, nullptr, nullptr, nullptr, nullptr,
                                       Cloud_OnVkKeyLongPress, Cloud_OnVkKeyPressUp, nullptr, nullptr, Cloud_OnPageKey});
    s_page.Bind(scr, {PollCloudPage, [](UiPagePowerEvent event) {
        if (event == UiPagePowerEvent::StopTransfers) {
            CloudScreen::StopTransfer();
        } else if (event == UiPagePowerEvent::FinishResume) {
            CloudScreen::ReloadApAfterStandby();
            if (s_cloud.data.mode == kCloudModeApp && !s_cloud.data.fetch_done) {
                ScheduleCloudFetch();
            } else {
                ResumeCloudSideHttpAfterDl();
            }
        }
    }, InvalidateSession, 100});
    return scr;
}

void CloudScreen::StopTransfer() {
    RequestStopTransfer("api");
}

void CloudScreen::ParkApForStandby() {
    ::ParkApForStandby();
}

void CloudScreen::ReloadApAfterStandby() {
    ::ReloadApAfterStandby();
}
