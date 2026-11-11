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
#include <freertos/task.h>

#include "assets/lang_config.h"
#include "board.h"
#include "api_http.h"
#include "power_policy.h"
#include "reader/reader.h"
#include "reader/image_util.h"
#include "screen_common.h"

// 黑白显示路径仅保留低亮度像素，先进行有序抖动以保留封面层次。
static void QuantizeThumbBinary(reader::RasterImage& img) {
    static const uint8_t kBayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    uint8_t* px = img.pixels.data();
    const int w = img.width;
    const int h = img.height;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const unsigned t = static_cast<unsigned>(kBayer[(y & 3) * 4 + (x & 3)]) * 16u + 8u;
            px[y * w + x] = px[y * w + x] >= t ? 255 : 0;
        }
    }
}

static bool CoverHttpCancelled(void*) {
    return s_dl_owns_http.load(std::memory_order_acquire) || s_thumb_abort.load(std::memory_order_acquire);
}

std::shared_ptr<reader::HttpTempFile> Cloud_DownloadCover(const char* url, reader::HttpCacheApp app) {
    if (CoverHttpCancelled(nullptr)) return {};
    if (!Board::GetInstance().EnsureNetworkReady()) return {};
    std::string error;
    auto cache = reader::DownloadHttpToCache(url, error, nullptr, CoverHttpCancelled, nullptr, app);
    if (!cache && !CoverHttpCancelled(nullptr)) ESP_LOGW(TAG, "cover download failed: %s", error.c_str());
    return cache;
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
        if (msg->index >= static_cast<int>(st.data.cover_cache.size())) {
            st.data.cover_cache.resize(st.data.items.size());
        }
        if (msg->index < static_cast<int>(st.data.cover_cache.size()) && msg->cache) {
            st.data.cover_cache[static_cast<size_t>(msg->index)] = std::move(msg->cache);
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
    {
        // 封面下载短时硬占网；须在 vTaskDelete 前析构（任务自杀不跑栈析构）
        PowerNeedHold hold_net(PowerNeed::OtaDownload);
        for (size_t i = 0; i < n; ++i) {
            if (Cloud_State().data.thumb_epoch != epoch || s_thumb_abort.load(std::memory_order_acquire)) {
                break;
            }
            const CoverThumbWorkItem& w = work->items[i];
            // 禁止抛 bad_alloc：上次崩在 operator new→malloc NULL→throw（EXCVADDR=0）
            auto* img = new (std::nothrow) reader::RasterImage();
            if (img == nullptr) {
                ESP_LOGE(TAG, "cover RasterImage allocation failed idx=%d", w.index);
                break;
            }
            bool ok = false;
            std::shared_ptr<reader::HttpTempFile> cache;
            if (w.url[0] != '\0') {
                ESP_LOGI(TAG, "cover dl begin idx=%d", w.index);
                if ((cache = Cloud_DownloadCover(w.url))) {
                    ok = reader::DecodeImageFileToL8(cache->path.c_str(), kThumbDecodeW,
                                                     kThumbDecodeH, *img, CoverHttpCancelled) &&
                         !img->empty();
                    if (ok) {
                        QuantizeThumbBinary(*img);
                    }
                    ESP_LOGI(TAG, "cover decode idx=%d ok=%d", w.index, ok ? 1 : 0);
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
                // 保留下载完成的缓存，供详情按目标尺寸解码。
            }
            auto* msg = new (std::nothrow) CoverThumbOneMsg{};
            if (msg == nullptr) {
                ESP_LOGE(TAG, "cover result allocation failed idx=%d", w.index);
                delete img;
                break;
            }
            msg->epoch = epoch;
            msg->index = w.index;
            msg->img = img;
            msg->cache = std::move(cache);
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
    if (st.data.cover_cache.size() != st.data.items.size()) {
        st.data.cover_cache.resize(st.data.items.size());
    }
    if (st.data.thumbs.size() != st.data.items.size()) {
        st.data.thumbs.resize(st.data.items.size());
    }
    // 原始预览图留在 SD 缓存；内存只保留目标尺寸的缩略图。
    auto* work = new (std::nothrow) CoverThumbFillWork{};
    if (work == nullptr) {
        ESP_LOGE(TAG, "cover work allocation failed");
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

