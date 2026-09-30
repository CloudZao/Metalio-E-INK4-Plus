#include "cloud_screen/cloud_screen_priv.h"
#include "cloud_screen/cloud_list.h"
#include "cloud_screen/cloud_ui_helpers.h"
#include "cloud_screen/push/push_resources_library.h"

#include <freertos/FreeRTOS.h>
#include <lvgl.h>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <utility>
#include <new>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/task.h>

#include "assets/lang_config.h"
#include "board.h"
#include "api_http.h"
#include "power_policy.h"
#include "reader/reader.h"
#include "reader/image_util.h"
#include "screen_common.h"

bool Cloud_DownloadUrlToBuffer(const char* url, std::vector<uint8_t>& buf, size_t max_bytes) {
    buf.clear();
    if (url == nullptr || url[0] == '\0' || max_bytes == 0) {
        return false;
    }
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        ESP_LOGW(TAG, "cover http CreateHttp failed int=%u psram=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        return false;
    }

    http->SetTimeout(15000);
    api::ApplyCommonHeaders(http);
    if (!http->Open("GET", url)) {
        ESP_LOGW(TAG, "cover http Open fail");
        return false;
    }
    if (s_dl_owns_http.load(std::memory_order_acquire) ||
        s_thumb_abort.load(std::memory_order_acquire)) {
        http->Close();
        return false;
    }

    const int status = http->GetStatusCode();
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "cover http status=%d", status);
        http->Close();
        return false;
    }

    const size_t content_length = http->GetBodyLength();
    if (content_length > max_bytes) {
        ESP_LOGW(TAG, "cover http too large cl=%u max=%u", static_cast<unsigned>(content_length),
                 static_cast<unsigned>(max_bytes));
        http->Close();
        return false;
    }
    size_t cap = content_length > 0 ? content_length : max_bytes;
    if (cap > max_bytes) {
        cap = max_bytes;
    }

    // 读块放堆上，压低任务栈占用
    constexpr size_t kChunk = 1024;
    std::unique_ptr<char[]> chunk(new (std::nothrow) char[kChunk]);
    if (chunk == nullptr) {
        ESP_LOGE(TAG, "cover chunk alloc fail int=%u psram=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        http->Close();
        return false;
    }
    while (buf.size() < cap) {
        if (s_dl_owns_http.load(std::memory_order_acquire) ||
            s_thumb_abort.load(std::memory_order_acquire)) {
            http->Close();
            buf.clear();
            return false;
        }
        const int n = http->Read(chunk.get(), kChunk);
        if (n < 0) {
            http->Close();
            buf.clear();
            return false;
        }
        if (n == 0) {
            break;
        }
        buf.insert(buf.end(), chunk.get(), chunk.get() + static_cast<size_t>(n));
    }
    http->Close();
    ESP_LOGI(TAG, "cover http done bytes=%u cl=%u", static_cast<unsigned>(buf.size()),
             static_cast<unsigned>(content_length));
    return !buf.empty();
}

void ApplyCoverThumbOneAsync(void* p) {
    auto* msg = static_cast<CoverThumbOneMsg*>(p);
    if (msg == nullptr) {
        return;
    }
    auto& st = Cloud_State();
    if (!ScreenAlive() || msg->epoch != st.data.thumb_epoch) {
        delete msg->img;
        if (msg->done) {
            s_thumb_busy.store(false);
            s_thumb_task = nullptr;
        }
        delete msg;
        return;
    }
    if (msg->index >= 0 && msg->index < static_cast<int>(st.data.thumbs.size()) && msg->img != nullptr) {
        if (msg->index >= static_cast<int>(st.data.cover_bytes.size())) {
            st.data.cover_bytes.resize(st.data.items.size());
        }
        if (msg->index < static_cast<int>(st.data.cover_bytes.size()) && !msg->bytes.empty()) {
            st.data.cover_bytes[static_cast<size_t>(msg->index)] = std::move(msg->bytes);
        }
        if (!msg->img->empty()) {
            msg->img->BindDsc();
        }
        st.data.thumbs[static_cast<size_t>(msg->index)].reset(msg->img);
        msg->img = nullptr;
        // 封面全量加载中列表尚未画出；就绪后整页 Render，勿 Patch 空行
        if (!st.data.covers_loading) {
            PatchRowThumb(msg->index);
        }
    } else {
        delete msg->img;
    }
    if (msg->done) {
        s_thumb_busy.store(false);
        s_thumb_task = nullptr;
        if (st.data.covers_loading) {
            st.data.covers_loading = false;
            SetStatusTip(Lang::Strings::CLOUD_REFRESHED, true);
            RenderListPage();
        }
    }
    delete msg;
}

void CoverThumbFillTask(void* arg) {
    auto* work = static_cast<CoverThumbFillWork*>(arg);
    if (work == nullptr) {
        s_thumb_busy.store(false);
        s_thumb_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    const uint32_t epoch = work->epoch;
    const size_t n = work->items.size();
    // 诊断：sp 落点区分片内(0x3FC8…) / PSRAM(0x3C…)；配合 malloc 失败日志钉下次崩因
    auto log_cover_diag = [](const char* phase, int idx) {
        void* sp = nullptr;
#if defined(__GNUC__)
        sp = __builtin_frame_address(0);
#endif
        const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t free_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const size_t large_int = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        const size_t large_ps = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG,
                 "cover diag %s idx=%d sp=%p hwm=%u int=%u/%u psram=%u/%u",
                 phase != nullptr ? phase : "?", idx, sp,
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                 static_cast<unsigned>(free_int), static_cast<unsigned>(large_int),
                 static_cast<unsigned>(free_ps), static_cast<unsigned>(large_ps));
    };
    log_cover_diag("task_start", -1);
    {
        // 封面下载短时硬占网；须在 vTaskDelete 前析构（任务自杀不跑栈析构）
        PowerNeedHold hold_net(PowerNeed::OtaDownload);
        for (size_t i = 0; i < n; ++i) {
            if (Cloud_State().data.thumb_epoch != epoch || s_thumb_abort.load(std::memory_order_acquire)) {
                break;
            }
            const CoverThumbWorkItem& w = work->items[i];
            log_cover_diag("before_new_img", w.index);
            // 禁止抛 bad_alloc：上次崩在 operator new→malloc NULL→throw（EXCVADDR=0）
            auto* img = new (std::nothrow) reader::RasterImage();
            if (img == nullptr) {
                const bool intact = heap_caps_check_integrity_all(true);
                ESP_LOGE(TAG,
                         "cover RasterImage new fail idx=%d heap_ok=%d int=%u psram=%u",
                         w.index, intact ? 1 : 0,
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
                break;
            }
            bool ok = false;
            std::vector<uint8_t> bytes;
            if (w.url[0] != '\0') {
                ESP_LOGI(TAG, "cover dl begin idx=%d", w.index);
                if (Cloud_DownloadUrlToBuffer(w.url, bytes, kThumbMaxDownloadBytes)) {
                    ok = reader::DecodeImageToL8(bytes.data(), bytes.size(), kThumbDecodeW,
                                                 kThumbDecodeH, *img) &&
                         !img->empty();
                    ESP_LOGI(TAG, "cover decode idx=%d ok=%d bytes=%u", w.index, ok ? 1 : 0,
                             static_cast<unsigned>(bytes.size()));
                } else {
                    ESP_LOGW(TAG, "cover dl fail idx=%d", w.index);
                }
            }
            if (Cloud_State().data.thumb_epoch != epoch) {
                delete img;
                break;
            }
            if (!ok) {
                // 占位空图：避免同页反复重试刷网；翻页/ack 删项会 bump epoch 重建
                img->Reset();
                // 下载成功但缩略解码失败时仍保留 bytes，供详情按大尺寸再解
            }
            log_cover_diag("before_new_msg", w.index);
            auto* msg = new (std::nothrow) CoverThumbOneMsg{};
            if (msg == nullptr) {
                const bool intact = heap_caps_check_integrity_all(true);
                ESP_LOGE(TAG, "cover CoverThumbOneMsg new fail idx=%d heap_ok=%d", w.index,
                         intact ? 1 : 0);
                delete img;
                break;
            }
            msg->epoch = epoch;
            msg->index = w.index;
            msg->img = img;
            msg->bytes = std::move(bytes);
            msg->done = (i + 1 == n);
            // cloud_cover 非 LVGL：禁止裸 lv_async_call
            if (!ScreenLvAsync(ApplyCoverThumbOneAsync, msg)) {
                delete msg->img;
                delete msg;
                if (i + 1 == n) {
                    s_thumb_busy.store(false);
                    s_thumb_task = nullptr;
                }
            }
        }
        // 中途 epoch 失效：须清 busy（否则再进页 Schedule 被挡）
        if (Cloud_State().data.thumb_epoch != epoch) {
            s_thumb_busy.store(false);
            s_thumb_task = nullptr;
        } else if (n == 0) {
            s_thumb_busy.store(false);
            s_thumb_task = nullptr;
        }
        delete work;
    }
    log_cover_diag("task_end", -1);
    vTaskDelete(nullptr);
}

void ScheduleCoverThumbFill() {
    auto& st = Cloud_State();
    if (!ScreenAlive() || st.data.items.empty()) {
        return;
    }
    // 下载/批量/删除进行中勿叠封面 HTTP
    if (st.xfer.download_busy || st.data.batch_busy || st.data.delete_busy ||
        s_dl_owns_http.load(std::memory_order_acquire)) {
        return;
    }
    if (s_thumb_busy.load()) {
        return;
    }
    if (st.data.cover_bytes.size() != st.data.items.size()) {
        st.data.cover_bytes.resize(st.data.items.size());
    }
    if (st.data.thumbs.size() != st.data.items.size()) {
        st.data.thumbs.resize(st.data.items.size());
    }
    // 全量预览图进 PSRAM；翻页只读 thumbs，不再按页 HTTP
    auto* work = new (std::nothrow) CoverThumbFillWork{};
    if (work == nullptr) {
        ESP_LOGE(TAG, "cover work alloc fail int=%u psram=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        if (st.data.covers_loading) {
            st.data.covers_loading = false;
            SetStatusTip(Lang::Strings::CLOUD_COVER_FAIL, true);
            RenderListPage();
        }
        return;
    }
    work->epoch = st.data.thumb_epoch;
    work->items.reserve(st.data.items.size());
    for (size_t i = 0; i < st.data.items.size(); ++i) {
        if (st.data.thumbs[i] != nullptr) {
            continue;
        }
        const reader::CloudPushResource& item = st.data.items[i];
        if (!item.HasCoverThumb()) {
            continue;
        }
        CoverThumbWorkItem w;
        w.index = static_cast<int>(i);
        const std::string& cover_url = item.CoverThumbUrl();
        if (cover_url.size() >= sizeof(w.url)) {
            ESP_LOGW(TAG, "cover url too long idx=%u len=%u", static_cast<unsigned>(i),
                     static_cast<unsigned>(cover_url.size()));
            continue;
        }
        std::snprintf(w.url, sizeof(w.url), "%s", cover_url.c_str());
        work->items.push_back(w);
    }
    if (work->items.empty()) {
        delete work;
        if (st.data.covers_loading) {
            st.data.covers_loading = false;
            SetStatusTip(Lang::Strings::CLOUD_REFRESHED, true);
            RenderListPage();
        }
        return;
    }
    s_thumb_abort.store(false, std::memory_order_release);
    if (s_thumb_busy.exchange(true)) {
        delete work;
        return;
    }
    if (xTaskCreatePinnedToCore(CoverThumbFillTask, "cloud_cover", kCoverThumbStack, work, 4,
                                &s_thumb_task, 0) != pdPASS) {
        s_thumb_busy.store(false);
        s_thumb_task = nullptr;
        delete work;
        ESP_LOGW(TAG, "cover thumb task create failed");
        if (st.data.covers_loading) {
            st.data.covers_loading = false;
            SetStatusTip(Lang::Strings::CLOUD_COVER_FAIL, true);
            RenderListPage();
        }
    }
}

