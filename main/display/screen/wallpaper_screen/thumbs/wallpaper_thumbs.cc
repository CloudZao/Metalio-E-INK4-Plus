#include "wallpaper_screen/wallpaper_screen_priv.h"
#include "wallpaper_screen/wallpaper_list.h"

#include <freertos/FreeRTOS.h>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>

#include "reader/reader.h"
#include "screen_common.h"

namespace {

struct ThumbWorkItem {
    int index = -1; // 文件列表索引
    char path[SD_FILE_PATH_CAPACITY] = {}; // 图片文件路径
};

struct ThumbFillWork {
    uint32_t seq = 0; // 缩略图任务批次，用于取消旧任务
    uint32_t epoch = 0;
    uint32_t generation = 0; // 页面生命周期代数
    int decode_w = 0; // 解码目标宽度
    int decode_h = 0; // 解码目标高度
    std::vector<ThumbWorkItem> items; // 当前批次待解码项目
};

std::atomic<uint32_t> s_thumb_seq{0};
std::atomic<ThumbFillWork*> s_thumb_posted{nullptr};

struct ThumbOneMsg {
    uint32_t epoch = 0;
    uint32_t generation = 0; // 页面生命周期代数
    int index = -1; // 文件列表索引
    char path[SD_FILE_PATH_CAPACITY] = {}; // 图片文件路径
    reader::RasterImage* img = nullptr; // 交由 UI 接收的解码结果
};

void ApplyThumbOne(void* p) {
    auto* msg = static_cast<ThumbOneMsg*>(p);
    if (msg == nullptr) {
        return;
    }
    const bool alive = Wallpaper_State().screen_alive && msg->epoch == Wallpaper_State().epoch &&
        !Wallpaper_State().decode_paused.load() &&
        msg->generation == Wallpaper_State().decode_generation.load();
    const int per_page = Wallpaper_State().list.list_per_page;
    const int start = Wallpaper_State().list.list_page * per_page;
    const bool slot_ok = alive && msg->img != nullptr && msg->index >= 0 &&
                         msg->index >= start - per_page && msg->index < start + 2 * per_page &&
                         msg->index < static_cast<int>(Wallpaper_State().list.thumbs.size()) &&
                         msg->index < static_cast<int>(Wallpaper_State().list.files.size()) &&
                         std::strcmp(Wallpaper_State().list.files[static_cast<size_t>(msg->index)].path, msg->path) == 0;
    if (!slot_ok) {
        delete msg->img;
        delete msg;
        return;
    }
    // 在途结果也遵守当前页及相邻页的缓存范围。
    Wallpaper_State().list.thumbs[static_cast<size_t>(msg->index)].reset(msg->img);
    msg->img = nullptr;
    if (Wallpaper_State().mode == UiMode::kList) {
        PaintCellThumb(msg->index);
    }
    delete msg;
}

void RunThumbWork(ThumbFillWork* work) {
    for (const auto& item : work->items) {
        if (s_thumb_seq.load(std::memory_order_acquire) != work->seq ||
            Wallpaper_State().epoch != work->epoch) {
            break;
        }
        auto* img = new (std::nothrow) reader::RasterImage();
        if (img == nullptr) {
            ESP_LOGW(TAG, "thumb alloc fail idx=%d", item.index);
            break;
        }
        struct CancelContext {
            ThumbFillWork* work; // 缩略图批次
            WallpaperDecodeToken token; // 页面解码生命周期
        };
        CancelContext cancel{work, {work->epoch, work->generation}};
        auto cancelled = [](void* p) {
            auto* ctx = static_cast<CancelContext*>(p);
            return s_thumb_seq.load() != ctx->work->seq || Wallpaper_DecodeCancelled(&ctx->token);
        };
        const bool ok = reader::DecodeImageFileToL8(item.path, work->decode_w, work->decode_h,
                                                   *img, cancelled, &cancel) && !img->empty();
        if (cancelled(&cancel)) { delete img; break; }
        if (!ok) {
            img->Reset();
        }
        if (Wallpaper_State().epoch != work->epoch) {
            delete img;
            break;
        }
        const bool page_left = s_thumb_seq.load(std::memory_order_acquire) != work->seq;
        auto* msg = new (std::nothrow) ThumbOneMsg{};
        if (msg == nullptr) {
            delete img;
            break;
        }
        msg->epoch = work->epoch;
        msg->generation = work->generation;
        msg->index = item.index;
        std::snprintf(msg->path, sizeof(msg->path), "%s", item.path);
        msg->img = img;
        if (!ScreenLvAsync(ApplyThumbOne, msg)) {
            delete msg->img;
            delete msg;
        }
        if (page_left) {
            break;
        }
    }
}

void ThumbFillTask(void* /*arg*/) {
    for (;;) {
        auto* work = s_thumb_posted.exchange(nullptr, std::memory_order_acq_rel);
        if (work == nullptr) {
            Wallpaper_State().workers.thumb_busy.store(false, std::memory_order_release);
            if (s_thumb_posted.load(std::memory_order_acquire) != nullptr &&
                !Wallpaper_State().workers.thumb_busy.exchange(true, std::memory_order_acq_rel)) {
                continue;
            }
            Wallpaper_State().workers.thumb_task = nullptr;
            vTaskDeleteWithCaps(nullptr);
            return;
        }
        RunThumbWork(work);
        delete work;
    }
}

bool StartThumbTask() {
    // 栈放 SPIRAM：内部 DRAM 常仅数十 KB，连续块不足时会创建失败
    constexpr uint32_t kThumbStack = 12 * 1024;
    if (xTaskCreatePinnedToCoreWithCaps(ThumbFillTask, "wp_thumb", kThumbStack, nullptr, 4,
                                        &Wallpaper_State().workers.thumb_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
        return true;
    }
    const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t largest_int = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    ESP_LOGW(TAG,
             "thumb task create failed: need stack=%u in SPIRAM; "
             "int free=%u largest=%u; psram free=%u largest=%u",
             static_cast<unsigned>(kThumbStack), static_cast<unsigned>(free_int),
             static_cast<unsigned>(largest_int), static_cast<unsigned>(free_psram),
             static_cast<unsigned>(largest_psram));
    return false;
}

}  // namespace

void CancelThumbFill() {
    s_thumb_seq.fetch_add(1, std::memory_order_acq_rel);
    auto* old = s_thumb_posted.exchange(nullptr, std::memory_order_acq_rel);
    delete old;
}

void ScheduleThumbFill() {
    if (Wallpaper_State().decode_paused.load()) return;
    if (!Wallpaper_State().screen_alive || Wallpaper_State().list.files.empty()) {
        CancelThumbFill();
        return;
    }
    if (Wallpaper_State().mode != UiMode::kList) {
        return;
    }
    Wallpaper_ClampListPage();
    const int start = Wallpaper_State().list.list_page * Wallpaper_State().list.list_per_page;
    const int end = std::min(start + Wallpaper_State().list.list_per_page,
                             static_cast<int>(Wallpaper_State().list.files.size()));
    auto* work = new (std::nothrow) ThumbFillWork{};
    if (work == nullptr) {
        return;
    }
    work->epoch = Wallpaper_State().epoch;
    work->generation = Wallpaper_State().decode_generation.load();
    work->decode_w = FrameInner(Wallpaper_State().list.grid_cell_w, kGridFrameBorder);
    work->decode_h = FrameInner(Wallpaper_State().list.grid_cover_h, kGridFrameBorder);
    work->items.reserve(static_cast<size_t>(end - start));
    for (int i = start; i < end; ++i) {
        if (i >= static_cast<int>(Wallpaper_State().list.thumbs.size())) {
            break;
        }
        if (Wallpaper_State().list.thumbs[static_cast<size_t>(i)] != nullptr) {
            continue;
        }
        ThumbWorkItem item;
        item.index = i;
        std::snprintf(item.path, sizeof(item.path), "%s", Wallpaper_State().list.files[static_cast<size_t>(i)].path);
        work->items.push_back(item);
    }
    if (work->items.empty()) {
        delete work;
        CancelThumbFill();
        return;
    }
    const unsigned n = static_cast<unsigned>(work->items.size());
    const int page = Wallpaper_State().list.list_page + 1;
    work->seq = s_thumb_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto* old = s_thumb_posted.exchange(work, std::memory_order_acq_rel);
    delete old;
    ESP_LOGI(TAG, "thumb fill async n=%u page=%d", n, page);
    if (Wallpaper_State().workers.thumb_busy.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (!StartThumbTask()) {
        auto* leftover = s_thumb_posted.exchange(nullptr, std::memory_order_acq_rel);
        delete leftover;
        Wallpaper_State().workers.thumb_busy.store(false, std::memory_order_release);
        Wallpaper_State().workers.thumb_task = nullptr;
    }
}
