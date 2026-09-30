#include "wallpaper_screen/wallpaper_screen_priv.h"
#include "wallpaper_screen/wallpaper_list.h"

#include <freertos/FreeRTOS.h>
#include <lvgl.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>
#include <utility>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>

#include "reader/reader.h"

// 先确保本页缩略图完成，再刷屏，避免首帧白底和重复全刷。
void FillPageThumbsSync(int page) {
    if (!Wallpaper_State().sd_ready || Wallpaper_State().list.files.empty() || Wallpaper_State().list.list_per_page <= 0) {
        return;
    }
    if (Wallpaper_State().list.thumbs.size() != Wallpaper_State().list.files.size()) {
        Wallpaper_State().list.thumbs.resize(Wallpaper_State().list.files.size());
    }
    if (page < 0) {
        page = 0;
    }
    const int start = page * Wallpaper_State().list.list_per_page;
    if (start >= static_cast<int>(Wallpaper_State().list.files.size())) {
        return;
    }
    const int end = std::min(start + Wallpaper_State().list.list_per_page, static_cast<int>(Wallpaper_State().list.files.size()));
    const int dw = FrameInner(Wallpaper_State().list.grid_cell_w > 0 ? Wallpaper_State().list.grid_cell_w
                                                                    : kGridCellWDefault,
                              kGridFrameBorder);
    const int dh = FrameInner(Wallpaper_State().list.grid_cover_h > 0 ? Wallpaper_State().list.grid_cover_h
                                                                    : kGridCoverHDefault,
                              kGridFrameBorder);
    for (int i = start; i < end; ++i) {
        if (Wallpaper_State().list.thumbs[static_cast<size_t>(i)] != nullptr &&
            !Wallpaper_State().list.thumbs[static_cast<size_t>(i)]->empty()) {
            continue;
        }
        auto img = std::make_unique<reader::RasterImage>();
        if (!reader::DecodeImageFileToL8(Wallpaper_State().list.files[static_cast<size_t>(i)].path, dw, dh, *img) ||
            img->empty()) {
            img->Reset();
        } else {
            img->BindDsc();
        }
        Wallpaper_State().list.thumbs[static_cast<size_t>(i)] = std::move(img);
    }
}

struct ThumbWorkItem {
    int index = -1;
    char path[192] = {};
};

struct ThumbBatchMsg {
    uint32_t epoch = 0;
    std::vector<std::pair<int, reader::RasterImage*>> items;
};

void ApplyThumbsAsync(void* p) {
    auto* msg = static_cast<ThumbBatchMsg*>(p);
    Wallpaper_State().workers.thumb_busy.store(false);
    Wallpaper_State().workers.thumb_task = nullptr;
    if (msg == nullptr) {
        return;
    }
    if (!Wallpaper_State().screen_alive || msg->epoch != Wallpaper_State().epoch) {
        for (auto& it : msg->items) {
            delete it.second;
        }
        delete msg;
        return;
    }

    // 先拆掉仍引用旧/即将被替换缓冲的控件，再 reset unique_ptr，避免 UAF
    if (Wallpaper_State().ui.list_host != nullptr) {
        lv_obj_clean(Wallpaper_State().ui.list_host);
        Wallpaper_State().ui.list_empty = nullptr;
    }

    for (auto& it : msg->items) {
        if (it.first < 0 || it.first >= static_cast<int>(Wallpaper_State().list.thumbs.size())) {
            delete it.second;
            continue;
        }
        if (it.second != nullptr) {
            if (!it.second->empty()) {
                it.second->BindDsc();
            }
            Wallpaper_State().list.thumbs[static_cast<size_t>(it.first)].reset(it.second);
        }
    }
    delete msg;
    if (Wallpaper_State().mode == UiMode::kList) {
        RebuildListPageInternal(false);  // 本批已解码，勿再 ScheduleThumbFill
    }
}

struct ThumbFillWork {
    uint32_t epoch = 0;
    int decode_w = 0;
    int decode_h = 0;
    std::vector<ThumbWorkItem> items;
};

void ThumbFillTask(void* arg) {
    auto* work = static_cast<ThumbFillWork*>(arg);
    auto* msg = new ThumbBatchMsg{};
    if (work != nullptr) {
        msg->epoch = work->epoch;
        const int dw = work->decode_w > 0 ? work->decode_w : 120;
        const int dh = work->decode_h > 0 ? work->decode_h : 160;
        for (const auto& w : work->items) {
            auto* img = new reader::RasterImage();
            if (reader::DecodeImageFileToL8(w.path, dw, dh, *img) && !img->empty()) {
                msg->items.emplace_back(w.index, img);
            } else {
                img->Reset();
                msg->items.emplace_back(w.index, img);
            }
        }
        delete work;
    }
    if (lv_async_call(ApplyThumbsAsync, msg) != LV_RESULT_OK) {
        for (auto& it : msg->items) {
            delete it.second;
        }
        delete msg;
        Wallpaper_State().workers.thumb_busy.store(false);
        Wallpaper_State().workers.thumb_task = nullptr;
    }
    vTaskDeleteWithCaps(nullptr);
}

void ScheduleThumbFill() {
    if (!Wallpaper_State().screen_alive || Wallpaper_State().mode != UiMode::kList || Wallpaper_State().list.files.empty()) {
        return;
    }
    if (Wallpaper_State().workers.thumb_busy.load()) {
        return;
    }
    Wallpaper_ClampListPage();
    const int start = Wallpaper_State().list.list_page * Wallpaper_State().list.list_per_page;
    const int end = std::min(start + Wallpaper_State().list.list_per_page, static_cast<int>(Wallpaper_State().list.files.size()));
    auto* work = new ThumbFillWork{};
    work->epoch = Wallpaper_State().epoch;
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
        return;
    }
    if (Wallpaper_State().workers.thumb_busy.exchange(true)) {
        delete work;
        return;
    }
    // 栈放 SPIRAM：内部 DRAM 常仅数十 KB，12KB 连续块不足时会创建失败
    constexpr uint32_t kThumbStack = 12 * 1024;
    if (xTaskCreatePinnedToCoreWithCaps(ThumbFillTask, "wp_thumb", kThumbStack, work, 4,
                                        &Wallpaper_State().workers.thumb_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t largest_int =
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const size_t largest_psram =
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        ESP_LOGW(TAG,
                 "thumb task create failed: need stack=%u in SPIRAM; "
                 "int free=%u largest=%u; psram free=%u largest=%u; items=%u",
                 static_cast<unsigned>(kThumbStack),
                 static_cast<unsigned>(free_int),
                 static_cast<unsigned>(largest_int),
                 static_cast<unsigned>(free_psram),
                 static_cast<unsigned>(largest_psram),
                 static_cast<unsigned>(work->items.size()));
        delete work;
        Wallpaper_State().workers.thumb_busy.store(false);
        Wallpaper_State().workers.thumb_task = nullptr;
    }
}

