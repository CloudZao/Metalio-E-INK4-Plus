#include "cloud_screen/cloud_fetch.h"
#include "cloud_screen/cloud_screen_priv.h"
#include "cloud_screen/cloud_list.h"
#include "cloud_screen/cloud_ui_helpers.h"
#include "cloud_screen/push/push_resources_library.h"

#include <freertos/FreeRTOS.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/task.h>

#include "assets/lang_config.h"
#include "board.h"
#include "power_policy.h"
#include "reader/reader.h"
#include "screen_common.h"

void AsyncFetchDone(void* p) {
    auto* msg = static_cast<CloudFetchDoneMsg*>(p);
    auto& st = Cloud_State();
    if (!OpStillValid(msg->op_gen)) {
        delete msg;
        // 僵尸拉取结束：若当前页仍要列表则重开（勿在 Reset 清 cancel 后与下载叠跑）
        if (ScreenAlive() && !st.data.fetch_done && !st.xfer.download_busy && !SyncBusy() &&
            !s_cloud_fetch_alive.load(std::memory_order_acquire)) {
            ScheduleNetPrepAndFetch();
        }
        return;
    }
    st.data.loading = false;
    if (msg->ok) {
        st.data.items = std::move(msg->items);
        st.data.thumbs.clear();
        st.data.thumbs.resize(st.data.items.size());
        st.data.cover_bytes.clear();
        st.data.cover_bytes.resize(st.data.items.size());
        ++st.data.thumb_epoch;
        st.data.error.clear();
        st.data.page = 0;
        RebuildFiltered();
        ESP_LOGI(TAG, "sync done ok n=%u", static_cast<unsigned>(st.data.items.size()));
        // 先全量拉预览图再出列表，翻页不再走封面 HTTP
        st.data.covers_loading = !st.data.items.empty();
        SetStatusTip(st.data.items.empty() ? "" : Lang::Strings::CLOUD_LOAD_COVER, false);
    } else {
        st.data.items.clear();
        st.data.filtered.clear();
        st.data.thumbs.clear();
        st.data.cover_bytes.clear();
        ++st.data.thumb_epoch;
        st.data.covers_loading = false;
        st.data.error = msg->error[0] != '\0' ? msg->error : Lang::Strings::CLOUD_SYNC_FAIL;
        ESP_LOGW(TAG, "sync done fail err=%s", st.data.error.c_str());
        SetStatusTip("", false);
    }
    st.data.fetch_done = true;
    ExitMultiModeEx(false, false);
    SyncSelectedSize();
    RenderListPage();
    if (st.data.covers_loading) {
        ScheduleCoverThumbFill();
    }
    delete msg;
}

void AsyncSyncStopped(void* p) {
    auto* msg = static_cast<SyncStoppedMsg*>(p);
    auto& st = Cloud_State();
    if (msg == nullptr) {
        return;
    }
    if (OpStillValid(msg->op_gen)) {
        st.data.loading = false;
        SetStatusTip("", false);
        RenderListPage();
        if (!st.data.fetch_done && !SyncBusy() && !st.xfer.download_busy) {
            ScheduleNetPrepAndFetch();
        }
    } else if (ScreenAlive() && !st.data.fetch_done && !st.xfer.download_busy && !SyncBusy() &&
               !s_cloud_fetch_alive.load(std::memory_order_acquire)) {
        ScheduleNetPrepAndFetch();
    }
    delete msg;
}

void CloudFetchWorker(void* arg) {
    auto* work = static_cast<CloudFetchWork*>(arg);
    auto* msg = new CloudFetchDoneMsg{};
    msg->op_gen = work->op_gen;
    ESP_LOGI(TAG, "sync fetch begin gen=%u", static_cast<unsigned>(work->op_gen));
    const int64_t t0 = esp_timer_get_time();
    std::string err;
    {
        PowerNeedHold hold_net(PowerNeed::OtaDownload);
        msg->ok = reader::FetchPushResources(msg->items, err, &s_sync_gate);
    }
    const int64_t ms = (esp_timer_get_time() - t0) / 1000;
    if (s_sync_gate.IsCancelled() || err == Lang::Strings::CLOUD_CANCELLED) {
        auto* stopped = new SyncStoppedMsg{};
        stopped->op_gen = work->op_gen;
        delete work;
        delete msg;
        s_cloud_fetch_alive.store(false, std::memory_order_release);
        if (!ScreenLvAsync(AsyncSyncStopped, stopped)) {
            delete stopped;
        }
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "sync fetch end gen=%u ok=%d n=%u %dms err=%s",
             static_cast<unsigned>(work->op_gen), msg->ok ? 1 : 0,
             static_cast<unsigned>(msg->items.size()), static_cast<int>(ms), err.c_str());
    if (!msg->ok) {
        std::snprintf(msg->error, sizeof(msg->error), "%s", err.empty() ? Lang::Strings::CLOUD_SYNC_FAIL : err.c_str());
    }
    delete work;
    s_cloud_fetch_alive.store(false, std::memory_order_release);
    if (!ScreenLvAsync(AsyncFetchDone, msg)) {
        delete msg;
    }
    vTaskDelete(nullptr);
}

void ScheduleCloudFetch() {
    auto& st = Cloud_State();
    if (st.data.loading || st.xfer.download_busy || st.data.waiting_net) {
        return;
    }
    // 勿 Reset 清 cancel 叠跑僵尸：旧 worker 未退出则只 Abort，等其 Async 结束再重开
    if (s_cloud_fetch_alive.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "ScheduleCloudFetch: stale fetch alive, cancel+abort defer");
        s_sync_gate.RequestCancel();
        AbortCloudGatesOffLvgl();
        SetStatusTip(Lang::Strings::CLOUD_REFRESHING, false);
        return;
    }
    st.data.loading = true;
    st.data.error.clear();
    st.data.fetch_done = false;
    SetStatusTip(Lang::Strings::CLOUD_REFRESHING, false);
    // 已有列表时保留内容，仅标题旁提示；空列表仍走 Render 占位
    if (st.data.items.empty()) {
        RenderListPage();
    } else {
        ApplyStatusTipUi();
    }

    auto* work = new CloudFetchWork{};
    work->op_gen = st.xfer.op_generation;
    s_sync_gate.Reset();
    s_cloud_fetch_alive.store(true, std::memory_order_release);
    if (xTaskCreatePinnedToCore(CloudFetchWorker, "cloud_res_fetch", kCloudFetchStack, work,
                                tskIDLE_PRIORITY + 2, nullptr, 0) != pdPASS) {
        s_cloud_fetch_alive.store(false, std::memory_order_release);
        delete work;
        st.data.loading = false;
        st.data.error = Lang::Strings::CLOUD_TASK_FAIL;
        SetStatusTip("", false);
        RenderListPage();
    }
}

void AsyncNetPrepDone(void* p) {
    auto* msg = static_cast<NetPrepDoneMsg*>(p);
    auto& st = Cloud_State();
    const bool alive = ScreenAlive();
    const bool valid = OpStillValid(msg->op_gen);
    const bool cancelled = s_sync_gate.IsCancelled();
    ESP_LOGI(TAG,
             "net_prep async done ok=%d op_gen=%u cur_gen=%u alive=%d valid=%d cancelled=%d "
             "waiting_net=%d",
             msg->ok ? 1 : 0, static_cast<unsigned>(msg->op_gen),
             static_cast<unsigned>(st.xfer.op_generation), alive ? 1 : 0, valid ? 1 : 0,
             cancelled ? 1 : 0, st.data.waiting_net ? 1 : 0);
    if (!valid || cancelled) {
        if (valid) {
            st.data.waiting_net = false;
        }
        delete msg;
        return;
    }
    st.data.waiting_net = false;
    if (!msg->ok) {
        st.data.error = Lang::Strings::CLOUD_NET_NOT_READY;
        SetStatusTip("", false);
        RenderListPage();
        delete msg;
        return;
    }
    st.data.error.clear();
    delete msg;
    ScheduleCloudFetch();
}

void NetPrepTask(void* /*arg*/) {
    const TaskHandle_t self = xTaskGetCurrentTaskHandle();
    const UBaseType_t stack0 = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(TAG, "net_prep begin op_gen=%u core=%d stack_hwm=%u",
             static_cast<unsigned>(s_net_prep_attach_gen.load()), xPortGetCoreID(),
             static_cast<unsigned>(stack0));
    auto* msg = new NetPrepDoneMsg{};
    bool ok = false;
    {
        // 硬占网：离页后仍可把 Ensure 跑完；结果是否上屏看 attach_gen
        PowerNeedHold hold_net(PowerNeed::OtaDownload);
        for (;;) {
            const uint32_t gen_before = s_net_prep_attach_gen.load();
            ok = Board::GetInstance().EnsureNetworkReady();
            if (ok) {
                break;
            }
            // 超时后若 Schedule 已换挂接（再进页刷新了 attach_gen），再等一轮
            if (s_net_prep_attach_gen.load() == gen_before) {
                break;
            }
            ESP_LOGI(TAG, "net_prep: attach_gen refreshed %u->%u, Ensure again",
                     static_cast<unsigned>(gen_before),
                     static_cast<unsigned>(s_net_prep_attach_gen.load()));
        }
    }
    msg->ok = ok;
    msg->op_gen = s_net_prep_attach_gen.load();
    const bool valid = OpStillValid(msg->op_gen);
    const UBaseType_t stack1 = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(TAG,
             "net_prep after EnsureNetworkReady ok=%d valid=%d cancelled=%d "
             "stack_hwm=%u->%u owned=%d attach_gen=%u",
             msg->ok ? 1 : 0, valid ? 1 : 0, s_sync_gate.IsCancelled() ? 1 : 0,
             static_cast<unsigned>(stack0), static_cast<unsigned>(stack1),
             (s_net_prep_task == self) ? 1 : 0, static_cast<unsigned>(msg->op_gen));
    if (!valid) {
        delete msg;
    } else if (!ScreenLvAsync(AsyncNetPrepDone, msg)) {
        ESP_LOGW(TAG, "net_prep ScreenLvAsync failed op_gen=%u",
                 static_cast<unsigned>(msg->op_gen));
        delete msg;
    }
    if (s_net_prep_task == self) {
        s_net_prep_task = nullptr;
    }
    vTaskDelete(nullptr);
}

void ScheduleNetPrepAndFetch() {
    auto& st = Cloud_State();
    if (st.data.fetch_done || st.xfer.download_busy || SyncBusy()) {
        ESP_LOGI(TAG, "ScheduleNetPrep skip fetch_done=%d download=%d sync_busy=%d",
                 st.data.fetch_done ? 1 : 0, st.xfer.download_busy ? 1 : 0, SyncBusy() ? 1 : 0);
        return;
    }
    if (s_cloud_fetch_alive.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "ScheduleNetPrep: stale fetch alive, cancel+abort defer");
        s_sync_gate.RequestCancel();
        AbortCloudGatesOffLvgl();
        SetStatusTip(Lang::Strings::CLOUD_CONNECTING_NET, false);
        return;
    }
    s_sync_gate.Reset();
    st.data.waiting_net = true;
    st.data.error.clear();
    SetStatusTip(Lang::Strings::CLOUD_CONNECTING_NET, false);
    if (st.data.items.empty()) {
        RenderListPage();
    } else {
        ApplyStatusTipUi();
    }

    // 已有 cloud_net：挂接当前 op_gen + 刷新 Board 等网 deadline，勿再造任务
    if (s_net_prep_task != nullptr) {
        s_net_prep_attach_gen.store(st.xfer.op_generation);
        Board::GetInstance().RefreshNetworkWaitDeadline(30000);
        ESP_LOGI(TAG, "ScheduleNetPrep: reuse task=%p op_gen=%u",
                 static_cast<void*>(s_net_prep_task), static_cast<unsigned>(st.xfer.op_generation));
        return;
    }

    s_net_prep_attach_gen.store(st.xfer.op_generation);
    if (xTaskCreatePinnedToCore(NetPrepTask, "cloud_net", 4096, nullptr, tskIDLE_PRIORITY + 2,
                                &s_net_prep_task, 0) != pdPASS) {
        s_net_prep_task = nullptr;
        s_net_prep_attach_gen.store(0);
        st.data.waiting_net = false;
        st.data.error = Lang::Strings::CLOUD_NET_NOT_READY;
        SetStatusTip("", false);
        RenderListPage();
        ESP_LOGW(TAG, "ScheduleNetPrep: task create failed");
        return;
    }
    ESP_LOGI(TAG, "ScheduleNetPrep: started task=%p op_gen=%u", static_cast<void*>(s_net_prep_task),
             static_cast<unsigned>(st.xfer.op_generation));
}

