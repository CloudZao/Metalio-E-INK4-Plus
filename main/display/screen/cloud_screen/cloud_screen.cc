#include "cloud_screen/cloud_screen.h"
#include "cloud_screen/cloud_screen_priv.h"
#include "cloud_screen/cloud_chrome.h"
#include "cloud_screen/download/cloud_download.h"
#include "cloud_screen/cloud_fetch.h"
#include "cloud_screen/cloud_list.h"
#include "cloud_screen/cloud_ui_helpers.h"
#include "cloud_screen/preview/cloud_wallpaper_preview.h"
#include "cloud_screen/push/push_resources_library.h"
#include "cloud_screen/push/push_resources_priv.h"

#include <freertos/FreeRTOS.h>
#include <lvgl.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <esp_log.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "board.h"
#include "reader/reader.h"
#include "reader/download_gate.h"
#include "screen_common.h"
#include "vk_key_handler.h"
#include "vk_page_repeat.h"
#include "application.h"
#include "display.h"
#include "api_http.h"

UiState& Cloud_State() {
    static UiState s;
    return s;
}

lv_timer_t* s_status_clear_timer = nullptr;
DownloadGate s_dl_gate{};
DownloadGate s_sync_gate{};  // 进页/手动拉列表刷新
int64_t s_last_user_sync_us = 0;  // 上次手动刷新；0=尚未点过
std::atomic<bool> s_ui_net_held{false};
std::atomic<bool> s_thumb_busy{false};
std::atomic<bool> s_thumb_abort{false};  // 下载开始时置位，封面 Read 循环退出
std::atomic<bool> s_dl_owns_http{false};  // 下载/批量独占 HTTP，禁止旁路 CreateHttp
std::atomic<bool> s_cloud_fetch_alive{false};  // 拉取任务仍在（含已 Invalidate 的僵尸）
TaskHandle_t s_net_prep_task = nullptr;
std::atomic<uint32_t> s_net_prep_attach_gen{0};  // 页挂接的 op_gen；复用等网时刷新
TaskHandle_t s_thumb_task = nullptr;
std::atomic<bool> s_wallpaper_preview_busy{false};
TaskHandle_t s_wallpaper_preview_task = nullptr;
TaskHandle_t s_delete_task = nullptr;
std::atomic<int64_t> s_dl_prog_last_ui_us{0};
TaskHandle_t s_dl_task = nullptr;
QueueHandle_t s_dl_q = nullptr;  // CloudDownloadWork*；nullptr=毒丸退出
ScreenPaintCoalesce s_cloud_paint{};
ScreenPaintCoalesce s_cloud_check_paint{};

void DetachCloudNetPrep(const char* why) {
    Cloud_State().data.waiting_net = false;
    if (s_net_prep_task == nullptr) {
        return;
    }
    ESP_LOGI(TAG,
             "DetachCloudNetPrep (%s): keep task=%p for reuse; RefreshNetworkWait on re-enter",
             why != nullptr ? why : "?", static_cast<void*>(s_net_prep_task));
}

// Close 绑定 Http：必须丢到主循环，禁止在 LVGL 锁内 Disconnect join
void AbortCloudGatesOffLvgl() {
    Application::GetInstance().Schedule([]() {
        s_dl_gate.AbortBoundHttp();
        s_sync_gate.AbortBoundHttp();
    });
}

void InvalidateSession() {
    auto& st = Cloud_State();
    ESP_LOGI(TAG,
             "InvalidateSession: net_prep_task=%p waiting_net=%d loading=%d download_busy=%d "
             "op_gen=%u->%u",
             static_cast<void*>(s_net_prep_task), st.data.waiting_net ? 1 : 0, st.data.loading ? 1 : 0,
             st.xfer.download_busy ? 1 : 0, static_cast<unsigned>(st.xfer.op_generation),
             static_cast<unsigned>(st.xfer.op_generation + 1));
    // 停传输业务；等网任务可复用（勿杀句柄）
    DetachCloudNetPrep("InvalidateSession");
    s_dl_gate.RequestCancel();
    s_sync_gate.RequestCancel();
    AbortCloudGatesOffLvgl();
    CancelStatusClearTimer();
    ++st.xfer.op_generation;
    ++st.data.thumb_epoch;
    ++st.preview.epoch;
    st.data.thumbs.clear();
    st.data.filtered.clear();
    st.data.selected.clear();
    st.data.cover_bytes.clear();
    st.data.batch_save_ids.clear();
    st.data.status_text[0] = '\0';
    st.data.multi = false;
    st.data.batch_busy = false;
    st.data.delete_busy = false;
    st.data.suppress_row_click_until_us = 0;
    st.data.suppress_row_click_idx = -1;
    st.xfer.download_key.clear();
    st.xfer.dl_cancel_pending = false;
    st.xfer.dl_cancel_key.clear();
    // 封面/预览任务靠 epoch 自停并清 busy；勿在此空句柄（否则可叠任务）
    st.preview.idx = -1;
    st.preview.open = false;
    st.data.loading = false;
    st.data.waiting_net = false;
    st.data.covers_loading = false;
    st.xfer.download_busy = false;
    s_dl_owns_http.store(false, std::memory_order_release);
    st.xfer.dl_user_abort = false;
    st.xfer.dl_job_total = 0;
    st.xfer.dl_job_done = 0;
    HideDlProgressPage();
    st.xfer.download_type = reader::PushResourceType::kUnknown;
    st.xfer.download_gen = 0;
    st.xfer.download_key.clear();
    st.xfer.pending_index = -1;
    st.xfer.action_index = -1;
    StopDlWorker();
}

void AsyncStopTransferUi(void* /*user*/) {
    ApplyStopTransferUi();
}

void ApplyStopTransferUi() {
    auto& st = Cloud_State();
    const bool was_busy =
        st.xfer.download_busy || st.data.batch_busy || st.data.delete_busy || st.xfer.dl_page_open;
    st.xfer.download_busy = false;
    st.xfer.download_gen = 0;
    st.xfer.download_key.clear();
    st.xfer.dl_cancel_pending = false;
    st.xfer.dl_cancel_key.clear();
    st.data.batch_save_ids.clear();
    st.data.batch_busy = false;
    st.data.delete_busy = false;
    st.xfer.dl_user_abort = false;
    st.xfer.dl_job_total = 0;
    st.xfer.dl_job_done = 0;
    s_dl_owns_http.store(false, std::memory_order_release);
    HideDlProgressPage();
    StopDlWorker();
    if (!ScreenAlive() || !was_busy) {
        return;
    }
    if (st.preview.open) {
        StyleDownloadBtn(true);
        return;
    }
    RenderListPage();
}

void RequestStopTransfer(const char* why) {
    ESP_LOGI(TAG, "stop transfer/sync (%s) net_prep_task=%p waiting_net=%d",
             why != nullptr ? why : "?", static_cast<void*>(s_net_prep_task),
             Cloud_State().data.waiting_net ? 1 : 0);
    // 页业务停干净；底层 EnsureNetworkReady/扫网可继续
    DetachCloudNetPrep(why != nullptr ? why : "stop");
    // LVGL 锁内只置位；Close/join 丢主循环，避免踩坏 timer 链
    s_dl_gate.RequestCancel();
    s_sync_gate.RequestCancel();
    AbortCloudGatesOffLvgl();
    if (!ScreenLvAsync(AsyncStopTransferUi, nullptr)) {
        ApplyStopTransferUi();
    }
}

bool Cloud_OnVkKey(const char* key) {
    if (key == nullptr) {
        return true;
    }
    auto& st = Cloud_State();
    if (std::strcmp(key, "vk_home") == 0) {
        RequestStopTransfer("vk_home");
        ScreenRequestHome();
        return true;
    }
    if (st.xfer.dl_page_open) {
        if (std::strcmp(key, "vk_prev") == 0) {
            ScreenLvAsync([](void*) { AbortDownloadJobFromUi(); });
            return true;
        }
        return true;
    }
    if (st.preview.open) {
        if (std::strcmp(key, "vk_prev") == 0) {
            ScreenLvAsync([](void*) { CloseWallpaperPreview(); });
            return true;
        }
        if (std::strcmp(key, "vk_next") == 0) {
            return true;
        }
        return true;
    }
    // 等网/拉列表中仍可翻页看已有数据；离页再 StopTransfer
    if (st.dialogs.dialog_mask != nullptr || st.dialogs.action_mask != nullptr) {
        return true;
    }
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
                ESP_LOGI(TAG, "vk_prev skip stale back: active=%s screen=%s",
                         active != nullptr ? active : "null", kScreenId);
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

bool CloudPageRepeatStep(int page_delta) {
    auto& st = Cloud_State();
    // 翻页看已有列表，不依赖 SyncBusy；等网中空列表 PageCount=1 自然不动
    if (page_delta == 0 || st.preview.open || st.dialogs.dialog_mask != nullptr ||
        st.dialogs.action_mask != nullptr) {
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

bool Cloud_OnVkKeyLongPress(const char* key) {
    return VkPageRepeatTryStart(key, CloudPageRepeatStep);
}

bool Cloud_OnVkKeyPressUp(const char* key) {
    return VkPageRepeatOnPressUp(key);
}

void Cloud_OnScreenDeleted(lv_event_t* e) {
    auto& st = Cloud_State();
    if (lv_event_get_target(e) != st.chrome.screen) {
        return;
    }
    ScreenPaintCoalesceReset(&s_cloud_paint);
    ScreenPaintCoalesceReset(&s_cloud_check_paint);
    InvalidateSession();
    ReleaseUiKeepNet();
    if (auto* disp = Board::GetInstance().GetDisplay()) {
        disp->SetIdleStatusMode(IdleStatusMode::kClock);
    }
    if (st.preview.img != nullptr) {
        lv_image_set_src(st.preview.img, nullptr);
    }
    if (st.preview.raster != nullptr) {
        delete st.preview.raster;
        st.preview.raster = nullptr;
    }
    st.chrome.screen = nullptr;
    st.chrome.main_body = nullptr;
    st.chrome.list_body = nullptr;
    st.chrome.footer = nullptr;
    st.chrome.sync_btn = nullptr;
    st.chrome.sync_lbl = nullptr;
    st.chrome.multi_bar = nullptr;
    st.chrome.page_lbl = nullptr;
    st.chrome.section_title = nullptr;
    st.chrome.status_lbl = nullptr;
    for (int i = 0; i < kTabCount; ++i) {
        st.chrome.tab_btns[i] = nullptr;
        st.chrome.tab_lbls[i] = nullptr;
    }
    st.preview.body = nullptr;
    st.preview.title = nullptr;
    st.preview.meta = nullptr;
    st.preview.status = nullptr;
    st.preview.img = nullptr;
    st.preview.img_host = nullptr;
    st.preview.download_btn = nullptr;
    st.preview.download_lbl = nullptr;
    st.dialogs.dialog_mask = nullptr;
    st.dialogs.action_mask = nullptr;
    st.xfer.dl_page = nullptr;
    st.xfer.dl_bar = nullptr;
    st.xfer.dl_pct_lbl = nullptr;
    st.xfer.dl_count_lbl = nullptr;
    st.xfer.dl_title_lbl = nullptr;
    st.xfer.dl_page_open = false;
}

lv_obj_t* CloudScreen::Create() {
    auto& st = Cloud_State();
    InvalidateSession();
    CancelStatusClearTimer();
    st.data.items.clear();
    st.data.filtered.clear();
    st.data.selected.clear();
    st.data.batch_save_ids.clear();
    st.data.thumbs.clear();
    st.data.cover_bytes.clear();
    // InvalidateSession 已 bump epoch；勿清 busy/句柄，孤儿任务靠 epoch 自停
    ++st.data.thumb_epoch;
    st.data.error.clear();
    st.data.status_text[0] = '\0';
    st.data.page = 0;
    st.data.filter_tab = 0;
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
    st.chrome.section_title = nullptr;
    st.chrome.status_lbl = nullptr;
    for (int i = 0; i < kTabCount; ++i) {
        st.chrome.tab_btns[i] = nullptr;
        st.chrome.tab_lbls[i] = nullptr;
    }
    st.preview.body = nullptr;
    st.preview.title = nullptr;
    st.preview.meta = nullptr;
    st.preview.status = nullptr;
    st.preview.img = nullptr;
    st.preview.img_host = nullptr;
    st.preview.download_btn = nullptr;
    st.preview.download_lbl = nullptr;
    st.preview.raster = nullptr;
    st.preview.idx = -1;
    st.preview.open = false;
    // 勿清 preview busy：孤儿靠 epoch 自停
    ++st.preview.epoch;
    st.dialogs.dialog_mask = nullptr;
    st.dialogs.action_mask = nullptr;
    st.xfer.dl_page = nullptr;
    st.xfer.dl_bar = nullptr;
    st.xfer.dl_pct_lbl = nullptr;
    st.xfer.dl_count_lbl = nullptr;
    st.xfer.dl_title_lbl = nullptr;
    st.xfer.dl_page_open = false;
    st.xfer.dl_user_abort = false;
    st.xfer.dl_job_total = 0;
    st.xfer.dl_job_done = 0;
    st.xfer.dl_shown_percent = -1;
    st.xfer.download_key.clear();
    st.xfer.dl_cancel_pending = false;
    st.xfer.dl_cancel_key.clear();
    st.xfer.download_type = reader::PushResourceType::kUnknown;

    ScreenSetIsHome(false);

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, Cloud_UiFont(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    Cloud_DisableScroll(scr);
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
    BuildWallpaperPreviewChrome(scr, status.height);

    ScheduleNetPrepAndFetch();

    VkKey_AttachScreen(scr, kScreenId,
                       VkKeyScreenDesc{CloudScreen::Create, Cloud_OnVkKey, nullptr, nullptr, nullptr,
                                       nullptr, Cloud_OnVkKeyLongPress, Cloud_OnVkKeyPressUp});
    return scr;
}

void CloudScreen::StopTransfer() {
    RequestStopTransfer("api");
}
