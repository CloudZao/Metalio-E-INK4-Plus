#include "cloud_screen/cloud_screen_priv.h"
#include "cloud_screen/download/cloud_download.h"
#include "cloud_screen/cloud_list.h"
#include "cloud_screen/cloud_ui_helpers.h"
#include "cloud_screen/push/push_resources_library.h"

#include <lvgl.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <esp_log.h>

#include "assets/lang_config.h"
#include "reader/reader.h"
#include "application.h"

int FindItemIndexByTaskId(const char* key) {
    if (key == nullptr || key[0] == '\0') {
        return -1;
    }
    auto& st = Cloud_State();
    for (int i = 0; i < static_cast<int>(st.data.items.size()); ++i) {
        if (st.data.items[static_cast<size_t>(i)].task_id == key) {
            return i;
        }
    }
    return -1;
}

void EraseItemAt(int idx) {
    auto& st = Cloud_State();
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
    if (idx < static_cast<int>(st.data.cover_bytes.size())) {
        st.data.cover_bytes.erase(st.data.cover_bytes.begin() + idx);
    }
    if (idx < static_cast<int>(st.data.selected.size())) {
        st.data.selected.erase(st.data.selected.begin() + idx);
    }
    if (st.preview.idx == idx) {
        st.preview.idx = -1;
    } else if (st.preview.idx > idx) {
        --st.preview.idx;
    }
    RebuildFiltered();
    ClampPage();
}

void ScheduleBatchContinueSoon(const char* why) {
    ESP_LOGI(TAG, "batch continue scheduled (%s)", why != nullptr ? why : "");
    Application::GetInstance().Schedule([]() {
        lv_timer_t* t = lv_timer_create(
            [](lv_timer_t* timer) {
                if (timer != nullptr) {
                    lv_timer_set_user_data(timer, nullptr);
                }
                auto& st2 = Cloud_State();
                if (!ScreenAlive() || !st2.data.batch_busy) {
                    return;
                }
                ESP_LOGI(TAG, "batch continue after settle");
                ContinueBatchSaveIfNeeded();
            },
            150, nullptr);
        if (t == nullptr) {
            ESP_LOGW(TAG, "batch continue timer create failed");
            ContinueBatchSaveIfNeeded();
            return;
        }
        lv_timer_set_repeat_count(t, 1);
    });
}

void AsyncDownloadDone(void* user_data) {
    auto* done = static_cast<CloudDownloadDoneMsg*>(user_data);
    auto& st = Cloud_State();
    if (!OpStillValid(done->op_gen)) {
        st.data.batch_save_ids.clear();
        st.data.batch_busy = false;
        st.xfer.download_busy = false;
        st.xfer.download_gen = 0;
        st.xfer.download_key.clear();
        st.xfer.dl_cancel_pending = false;
        st.xfer.dl_cancel_key.clear();
        st.xfer.dl_user_abort = false;
        HideDlProgressPage();
        delete done;
        ResumeCloudSideHttpAfterDl();
        StopDlWorker();
        return;
    }

    // 取消：断当前 HTTP；用户整单取消则不再续下
    if (done->cancelled && st.xfer.dl_cancel_pending && st.xfer.dl_cancel_key == done->key) {
        const bool user_abort = st.xfer.dl_user_abort;
        const bool batch = st.data.batch_busy && !user_abort;
        char done_key[160];
        std::snprintf(done_key, sizeof(done_key), "%s", done->key);
        st.xfer.dl_cancel_pending = false;
        st.xfer.dl_cancel_key.clear();
        st.xfer.download_busy = false;
        st.xfer.download_gen = 0;
        st.xfer.download_key.clear();
        st.xfer.download_type = reader::PushResourceType::kUnknown;

        if (user_abort) {
            st.data.batch_save_ids.clear();
            st.data.batch_busy = false;
            st.xfer.dl_user_abort = false;
            HideDlProgressPage();
            SetStatusTip(Lang::Strings::CLOUD_CANCELLED, true);
            RenderListPage();
            ResumeCloudSideHttpAfterDl();
            StopDlWorker();
        } else if (batch) {
            if (!st.data.batch_save_ids.empty() && st.data.batch_save_ids.front() == done_key) {
                st.data.batch_save_ids.erase(st.data.batch_save_ids.begin());
            }
            st.data.batch_save_ids.erase(
                std::remove(st.data.batch_save_ids.begin(), st.data.batch_save_ids.end(),
                            std::string(done_key)),
                st.data.batch_save_ids.end());
            ScheduleBatchContinueSoon("cancel");
        } else {
            HideDlProgressPage();
            RenderListPage();
            ResumeCloudSideHttpAfterDl();
            StopDlWorker();
        }
        delete done;
        return;
    }

    if (done->cancelled || done->op_gen != st.xfer.download_gen) {
        st.data.batch_save_ids.clear();
        st.data.batch_busy = false;
        st.xfer.download_busy = false;
        st.xfer.download_gen = 0;
        st.xfer.download_key.clear();
        st.xfer.dl_cancel_pending = false;
        st.xfer.dl_cancel_key.clear();
        st.xfer.dl_user_abort = false;
        HideDlProgressPage();
        delete done;
        ResumeCloudSideHttpAfterDl();
        if (ScreenAlive()) {
            RenderListPage();
        }
        StopDlWorker();
        return;
    }
    const bool batch = st.data.batch_busy && !st.xfer.dl_user_abort;
    char done_key[160];
    std::snprintf(done_key, sizeof(done_key), "%s", done->key);
    st.xfer.download_busy = false;
    st.xfer.download_gen = 0;
    st.xfer.download_key.clear();
    st.xfer.download_type = reader::PushResourceType::kUnknown;
    st.xfer.dl_cancel_pending = false;
    st.xfer.dl_cancel_key.clear();

    if (!done->ok) {
        st.data.batch_save_ids.clear();
        st.data.batch_busy = false;
        st.xfer.dl_user_abort = false;
        ExitMultiModeEx(false, false);
        HideDlProgressPage();
        SetStatusTip(done->error[0] != '\0' ? done->error : Lang::Strings::CLOUD_SAVE_FAIL, true);
        RenderListPage();
        ResumeCloudSideHttpAfterDl();
        delete done;
        StopDlWorker();
        return;
    }

    st.data.error.clear();
    ++st.xfer.dl_job_done;
    RefreshDlProgressPage(100);
    const int idx = FindItemIndexByTaskId(done_key);
    if (done->acked && idx >= 0) {
        EraseItemAt(idx);
    } else if (idx >= 0) {
        SetStatusTip(Lang::Strings::CLOUD_SAVED_SYNC_FAIL, true);
    }
    if (!batch) {
        st.xfer.dl_user_abort = false;
        HideDlProgressPage();
        RenderListPage();
        ResumeCloudSideHttpAfterDl();
        StopDlWorker();
    }
    delete done;

    if (batch) {
        if (!st.data.batch_save_ids.empty() && st.data.batch_save_ids.front() == done_key) {
            st.data.batch_save_ids.erase(st.data.batch_save_ids.begin());
        }
        ScheduleBatchContinueSoon("ok");
        return;
    }
}

void ContinueBatchSaveIfNeeded() {
    auto& st = Cloud_State();
    if (st.xfer.dl_user_abort) {
        st.data.batch_save_ids.clear();
        st.data.batch_busy = false;
        st.xfer.dl_user_abort = false;
        HideDlProgressPage();
        SetStatusTip(Lang::Strings::CLOUD_CANCELLED, true);
        RenderListPage();
        ResumeCloudSideHttpAfterDl();
        StopDlWorker();
        return;
    }
    while (!st.data.batch_save_ids.empty()) {
        const std::string id = st.data.batch_save_ids.front();
        const int idx = FindItemIndexByTaskId(id.c_str());
        if (idx < 0) {
            st.data.batch_save_ids.erase(st.data.batch_save_ids.begin());
            continue;
        }
        StartResourceDownload(idx);
        return;
    }
    st.data.batch_busy = false;
    ExitMultiModeEx(false, false);
    HideDlProgressPage();
    SetStatusTip("", false);
    RenderListPage();
    ResumeCloudSideHttpAfterDl();
    StopDlWorker();
}

