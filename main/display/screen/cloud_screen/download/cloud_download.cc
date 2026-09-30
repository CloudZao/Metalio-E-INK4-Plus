#include "cloud_screen/download/cloud_download.h"
#include "cloud_screen/cloud_screen_priv.h"
#include "cloud_screen/cloud_list.h"
#include "cloud_screen/cloud_ui_helpers.h"
#include "cloud_screen/preview/cloud_wallpaper_preview.h"
#include "cloud_screen/push/push_resources_library.h"

#include <freertos/FreeRTOS.h>
#include <lvgl.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/idf_additions.h>

#include "assets/lang_config.h"
#include "power_policy.h"
#include "reader/reader.h"
#include "screen_common.h"
#include "application.h"

void DrainDlQueue() {
    if (s_dl_q == nullptr) {
        return;
    }
    CloudDownloadWork* w = nullptr;
    while (xQueueReceive(s_dl_q, &w, 0) == pdTRUE) {
        delete w;
    }
}

void StopDlWorker() {
    if (s_dl_task == nullptr) {
        return;
    }
    if (s_dl_q == nullptr) {
        return;
    }
    DrainDlQueue();
    CloudDownloadWork* poison = nullptr;
    xQueueSend(s_dl_q, &poison, 0);
}

bool EnqueueDlWork(CloudDownloadWork* work) {
    if (work == nullptr) {
        return false;
    }
    if (s_dl_q == nullptr) {
        s_dl_q = xQueueCreate(2, sizeof(CloudDownloadWork*));
        if (s_dl_q == nullptr) {
            return false;
        }
    }
    if (s_dl_task == nullptr) {
        if (xTaskCreatePinnedToCoreWithCaps(CloudDownloadWorker, "cloud_res_dl", kCloudDownloadStack,
                                            nullptr, tskIDLE_PRIORITY + 2, &s_dl_task, 0,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
            s_dl_task = nullptr;
            return false;
        }
        ESP_LOGI(TAG, "cloud_res_dl task created (once)");
    }
    if (xQueueSend(s_dl_q, &work, 0) != pdTRUE) {
        return false;
    }
    return true;
}

void AbortDownloadJobFromUi() {
    auto& st = Cloud_State();
    if (!st.xfer.dl_page_open && !st.xfer.download_busy && !st.data.batch_busy) {
        return;
    }
    st.xfer.dl_user_abort = true;
    st.data.batch_save_ids.clear();
    if (st.xfer.download_busy && !st.xfer.dl_cancel_pending && !st.xfer.download_key.empty()) {
        st.xfer.dl_cancel_pending = true;
        st.xfer.dl_cancel_key = st.xfer.download_key;
        ESP_LOGI(TAG, "abort download job key=%s", st.xfer.dl_cancel_key.c_str());
        s_dl_gate.RequestCancel();
        Application::GetInstance().Schedule([]() { s_dl_gate.AbortBoundHttp(); });
        if (st.xfer.dl_title_lbl != nullptr) {
            lv_label_set_text(st.xfer.dl_title_lbl, Lang::Strings::CLOUD_CANCELLING);
        }
        return;
    }
    st.data.batch_busy = false;
    st.xfer.download_busy = false;
    st.xfer.dl_cancel_pending = false;
    st.xfer.dl_cancel_key.clear();
    HideDlProgressPage();
    SetStatusTip(Lang::Strings::CLOUD_CANCELLED, true);
    RenderListPage();
    ResumeCloudSideHttpAfterDl();
    StopDlWorker();
}

void ShowRowDownloadProgress(int percent) {
    auto& st = Cloud_State();
    if (!st.xfer.download_busy || st.xfer.download_gen != st.xfer.op_generation || !ScreenAlive()) {
        return;
    }
    RefreshDlProgressPage(percent);
}

bool CloudDlTasksActive() {
    const auto& st = Cloud_State();
    return st.xfer.download_busy || st.data.batch_busy || st.data.delete_busy;
}

void ShowPreviewWaitDlTip() {
    auto& st = Cloud_State();
    if (st.preview.status != nullptr) {
        lv_label_set_text(st.preview.status, Lang::Strings::CLOUD_WAIT_DOWNLOAD);
        lv_obj_clear_flag(st.preview.status, LV_OBJ_FLAG_HIDDEN);
    }
    StyleDownloadBtn(true);
}

void AbortCloudSideHttpForDownload() {
    auto& st = Cloud_State();
    s_dl_owns_http.store(true, std::memory_order_release);
    s_thumb_abort.store(true, std::memory_order_release);
    ++st.data.thumb_epoch;
    // 打断预览加载任务，详情页保持打开并提示等待
    ++st.preview.epoch;
    if (st.preview.open) {
        ClearWallpaperPreviewImage();
        ShowPreviewWaitDlTip();
    }
}

void ResumeCloudSideHttpAfterDl() {
    auto& st = Cloud_State();
    if (!ScreenAlive() || CloudDlTasksActive()) {
        return;
    }
    s_dl_owns_http.store(false, std::memory_order_release);
    ScheduleCoverThumbFill();
    if (st.preview.open && st.preview.idx >= 0) {
        if (st.preview.status != nullptr) {
            lv_label_set_text(st.preview.status, Lang::Strings::CLOUD_LOADING);
            lv_obj_clear_flag(st.preview.status, LV_OBJ_FLAG_HIDDEN);
        }
        ScheduleLoadWallpaperPreview(st.preview.idx);
    }
}

void AsyncDownloadProgress(void* user_data) {
    auto* msg = static_cast<CloudDownloadProgressMsg*>(user_data);
    ShowRowDownloadProgress(msg->percent);
    delete msg;
}

void OnDownloadProgress(int percent, void* /*user*/) {
    // 仅在 cloud_res_dl 任务调用：禁止碰 Cloud_State()（与 LVGL 并发会踩堆）
    if (s_dl_gate.IsCancelled()) {
        return;
    }
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }

    const int64_t now_us = esp_timer_get_time();
    const int64_t last_us = s_dl_prog_last_ui_us.load(std::memory_order_relaxed);
    const bool force = (percent <= 0) || (percent >= 99);
    if (!force && last_us != 0 && (now_us - last_us) < kDlProgUiIntervalUs) {
        return;
    }
    s_dl_prog_last_ui_us.store(now_us, std::memory_order_relaxed);

    auto* msg = new CloudDownloadProgressMsg{};
    msg->percent = percent;
    // cloud_res_dl 非 LVGL：裸 lv_async_call 会与 timer_handler 竞态踩堆
    if (!ScreenLvAsync(AsyncDownloadProgress, msg)) {
        delete msg;
    }
}

void CloudDownloadWorker(void* /*arg*/) {
    // PowerNeedHold 须在 vTaskDelete 前析构（任务自杀不跑栈析构）
    {
        PowerNeedHold hold_net(PowerNeed::OtaDownload);
        for (;;) {
            CloudDownloadWork* work = nullptr;
            if (xQueueReceive(s_dl_q, &work, portMAX_DELAY) != pdTRUE) {
                continue;
            }
            if (work == nullptr) {
                break;
            }

            auto* msg = new CloudDownloadDoneMsg{};
            msg->op_gen = work->op_gen;
            msg->type = work->item.type;
            std::string err;
            std::snprintf(msg->key, sizeof(msg->key), "%s", work->item.task_id.c_str());
            msg->ok = reader::DownloadPushResource(work->item, err, OnDownloadProgress, nullptr,
                                                   &s_dl_gate,
                                                   work->cover_bytes.empty()
                                                       ? nullptr
                                                       : work->cover_bytes.data(),
                                                   work->cover_bytes.size());
            msg->cancelled = s_dl_gate.IsCancelled() || err == Lang::Strings::CLOUD_CANCELLED;
            if (msg->cancelled) {
                ESP_LOGI(TAG, "download stopped key=%s", msg->key);
            }
            if (msg->ok) {
                std::string ack_err;
                msg->acked = reader::AckPushResourceDownloaded(work->item, ack_err);
                if (!msg->acked) {
                    ESP_LOGW(TAG, "ack push resource failed taskId=%s err=%s",
                             work->item.task_id.c_str(), ack_err.c_str());
                }
            } else if (!msg->cancelled) {
                std::snprintf(msg->error, sizeof(msg->error), "%s",
                              err.empty() ? Lang::Strings::CLOUD_DOWNLOAD_FAIL : err.c_str());
            }
            delete work;

            if (!ScreenLvAsync(AsyncDownloadDone, msg)) {
                delete msg;
            }
        }
    }
    s_dl_task = nullptr;
    vTaskDelete(nullptr);
}

void StartResourceDownload(int index) {
    auto& st = Cloud_State();
    if (st.xfer.download_busy || SyncBusy()) {
        return;
    }
    if (index < 0 || index >= static_cast<int>(st.data.items.size())) {
        return;
    }

    auto* work = new CloudDownloadWork{};
    work->op_gen = st.xfer.op_generation;
    work->item = st.data.items[static_cast<size_t>(index)];
    if (index < static_cast<int>(st.data.cover_bytes.size()) &&
        !st.data.cover_bytes[static_cast<size_t>(index)].empty()) {
        work->cover_bytes = st.data.cover_bytes[static_cast<size_t>(index)];
    }

    s_dl_gate.Reset();
    if (st.preview.open) {
        ++st.preview.epoch;
        st.preview.idx = -1;
        ClearWallpaperPreviewImage();
        ShowWallpaperPreviewMode(false);
    }
    if (!st.xfer.dl_page_open) {
        st.xfer.dl_job_total = st.data.batch_busy ? static_cast<int>(st.data.batch_save_ids.size()) : 1;
        if (st.xfer.dl_job_total < 1) {
            st.xfer.dl_job_total = 1;
        }
        st.xfer.dl_job_done = 0;
        st.xfer.dl_user_abort = false;
        ShowDlProgressPage();
    } else {
        RefreshDlProgressPage(0);
    }
    if (st.xfer.dl_title_lbl != nullptr) {
        const std::string shown =
            EllipsizeText(work->item.name, Cloud_UiFont(), LV_HOR_RES - 80);
        char title[192];
        std::snprintf(title, sizeof(title), Lang::Strings::CLOUD_SAVING_NAME_FMT,
                      shown.empty() ? "…" : shown.c_str());
        lv_label_set_text(st.xfer.dl_title_lbl, title);
    }
    AbortCloudSideHttpForDownload();
    st.xfer.download_busy = true;
    st.xfer.download_type = work->item.type;
    st.xfer.download_gen = work->op_gen;
    st.xfer.download_key = work->item.task_id;
    st.xfer.dl_cancel_pending = false;
    st.xfer.dl_cancel_key.clear();
    s_dl_prog_last_ui_us.store(0, std::memory_order_relaxed);

    if (!EnqueueDlWork(work)) {
        delete work;
        st.xfer.download_busy = false;
        st.xfer.download_gen = 0;
        st.xfer.download_key.clear();
        st.xfer.download_type = reader::PushResourceType::kUnknown;
        st.data.batch_save_ids.clear();
        st.data.batch_busy = false;
        st.xfer.dl_user_abort = false;
        HideDlProgressPage();
        SetStatusTip(Lang::Strings::CLOUD_TASK_FAIL, true);
        RenderListPage();
        ResumeCloudSideHttpAfterDl();
        StopDlWorker();
    }
}

