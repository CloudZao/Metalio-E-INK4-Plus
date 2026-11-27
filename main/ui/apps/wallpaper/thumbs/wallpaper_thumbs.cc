#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <vector>
#include <esp_log.h>
#include "sysmgmt/tasks/task_executor.h"
#include "business/reader/list_thumbnail_cache.h"
#include "business/reader/reader.h"
#include "ui/apps/wallpaper/wallpaper_list.h"
#include "ui/apps/wallpaper/wallpaper_screen_priv.h"
#include "ui/apps/wallpaper/thumbs/wallpaper_thumb_priority.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"

struct WallpaperThumbsThumbWorkItem {
    int index = -1;                         // 文件列表索引
    char path[SD_FILE_PATH_CAPACITY] = {};  // 图片文件路径
    int64_t mtime = 0;                      // 文件版本
    size_t bytes = 0;                       // 列表收集的文件长度
    uint64_t cost = 0;                      // 仅影响后台处理顺序的预计成本
};

struct WallpaperThumbsThumbFillWork : ScreenUiCompletion {
    uint32_t seq = 0;  // 缩略图任务批次，用于取消旧任务
    uint32_t epoch = 0;
    uint32_t generation = 0;                           // 页面生命周期代数
    int decode_w = 0;                                  // 解码目标宽度
    int decode_h = 0;                                  // 解码目标高度
    std::vector<WallpaperThumbsThumbWorkItem> items;   // 当前批次待解码项目
    StorageError storage_error; // 文件访问错误
    ImageDecodeError error = ImageDecodeError::kNone;  // 批次中断原因，向未完成项目反馈
};

static std::atomic<uint32_t> s_thumb_seq{0};
static std::atomic<WallpaperThumbsThumbFillWork*> s_thumb_posted{nullptr};

struct WallpaperThumbsThumbOneMsg : ScreenUiCompletion {
    uint32_t epoch = 0;
    uint32_t generation = 0;                           // 页面生命周期代数
    int index = -1;                                    // 文件列表索引
    char path[SD_FILE_PATH_CAPACITY] = {};             // 图片文件路径
    std::unique_ptr<RasterImage> img;                  // 交由 UI 接收的解码结果
    StorageError storage_error; // 文件访问错误
    ImageDecodeError error = ImageDecodeError::kNone;  // 本次缩略图解码错误
};

static void ApplyThumbOne(ScreenUiCompletion* completion) {
    std::unique_ptr<WallpaperThumbsThumbOneMsg> msg(static_cast<WallpaperThumbsThumbOneMsg*>(completion));
    const bool alive = Wallpaper_State().screen_alive && msg->epoch == Wallpaper_State().epoch &&
                       !Wallpaper_State().decode_paused.load() &&
                       msg->generation == Wallpaper_State().decode_generation.load();
    const int per_page = Wallpaper_State().list.list_per_page;
    const int start = 0;
    const bool slot_ok =
        alive && msg->img != nullptr && msg->index >= 0 && msg->index >= start - per_page &&
        msg->index < start + 2 * per_page && msg->index < static_cast<int>(Wallpaper_State().list.thumbs.size()) &&
        msg->index < static_cast<int>(Wallpaper_State().list.files.size()) &&
        std::strcmp(Wallpaper_State().list.files[static_cast<size_t>(msg->index)].path, msg->path) == 0;
    if (!slot_ok) {
        return;
    }
    // 在途结果也遵守当前页及相邻页的缓存范围。
    Wallpaper_State().list.thumbs[static_cast<size_t>(msg->index)] = std::move(msg->img);
    Wallpaper_State().list.files[static_cast<size_t>(msg->index)].thumb_error = msg->error;
    Wallpaper_State().list.files[static_cast<size_t>(msg->index)].thumb_storage_error = msg->storage_error;
    if (Wallpaper_State().mode == UiMode::kList) {
        PaintCellThumb(msg->index);
    }
}

static void ApplyThumbBatch(ScreenUiCompletion* completion) {
    std::unique_ptr<WallpaperThumbsThumbFillWork> work(static_cast<WallpaperThumbsThumbFillWork*>(completion));
    auto& state = Wallpaper_State();
    if (work->error == ImageDecodeError::kNone || !state.screen_alive.load() || state.decode_paused.load() ||
        work->epoch != state.epoch.load() || work->generation != state.decode_generation.load() ||
        work->seq != s_thumb_seq.load()) {
        return;
    }
    for (const auto& item : work->items) {
        if (item.index < 0 || item.index >= static_cast<int>(state.list.thumbs.size()) ||
            item.index >= static_cast<int>(state.list.files.size()) ||
            state.list.thumbs[static_cast<size_t>(item.index)] != nullptr ||
            std::strcmp(state.list.files[static_cast<size_t>(item.index)].path, item.path) != 0) {
            continue;
        }
        state.list.files[static_cast<size_t>(item.index)].thumb_error = work->error;
        state.list.files[static_cast<size_t>(item.index)].thumb_storage_error =
            work->error == ImageDecodeError::kNoMemory ? StorageError{} : work->storage_error;
        if (state.mode == UiMode::kList) {
            PaintCellThumb(item.index);
        }
    }
}

static void RunThumbWork(WallpaperThumbsThumbFillWork* work) {
    struct CancelContext {
        WallpaperThumbsThumbFillWork* work;  // 缩略图批次
        WallpaperDecodeToken token;          // 页面解码生命周期
    };

    CancelContext cancel{work, {work->epoch, work->generation}};
    auto cancelled = [](void* p) {
        auto* ctx = static_cast<CancelContext*>(p);
        return s_thumb_seq.load() != ctx->work->seq || Wallpaper_DecodeCancelled(&ctx->token);
    };
    for (auto& item : work->items) {
        if (cancelled(&cancel)) {
            return;
        }
        item.cost = Wallpaper_EstimateThumbCost(item.path, item.bytes, cancelled, &cancel);
    }
    std::sort(work->items.begin(), work->items.end(),
              [](const WallpaperThumbsThumbWorkItem& a, const WallpaperThumbsThumbWorkItem& b) {
                  return a.cost != b.cost ? a.cost < b.cost : a.index < b.index;
              });
    for (const auto& item : work->items) {
        if (s_thumb_seq.load(std::memory_order_acquire) != work->seq || Wallpaper_State().epoch != work->epoch) {
            break;
        }
        auto img = std::unique_ptr<RasterImage>(new (std::nothrow) RasterImage());
        if (img == nullptr) {
            work->error = ImageDecodeError::kNoMemory;
            ESP_LOGE(TAG, "thumb alloc fail idx=%d", item.index);
            break;
        }
        StorageError storage_error; // 文件访问错误
        ImageDecodeError error = ImageDecodeError::kNone;
        const bool ok =
            DecodeImageFileToL8(item.path, work->decode_w, work->decode_h, *img, cancelled, &cancel, &error, &storage_error) &&
            !img->empty();
        if (cancelled(&cancel)) {
            break;
        }
        if (!ok) {
            img->Reset();
        } else {
            CacheListThumbnail(item.path, work->decode_w, work->decode_h, item.bytes, item.mtime, *img);
        }
        if (Wallpaper_State().epoch != work->epoch) {
            break;
        }
        const bool page_left = s_thumb_seq.load(std::memory_order_acquire) != work->seq;
        auto* msg = new (std::nothrow) WallpaperThumbsThumbOneMsg{};
        if (msg == nullptr) {
            work->error = ImageDecodeError::kNoMemory;
            ESP_LOGE(TAG, "thumbnail completion allocation failed: index=%d", item.index);
            break;
        }
        msg->apply = ApplyThumbOne;
        msg->epoch = work->epoch;
        msg->generation = work->generation;
        msg->index = item.index;
        std::snprintf(msg->path, sizeof(msg->path), "%s", item.path);
        msg->img = std::move(img);
        msg->error = error;
        msg->storage_error = storage_error;
        if (storage_error && !work->storage_error) {
            work->storage_error = storage_error;
            LogStorageError(TAG, storage_error);
        }
        ScreenCompleteWork(msg);
        if (page_left) {
            break;
        }
    }
}

static void ThumbFillTask(void* /*arg*/) {
    for (;;) {
        std::unique_ptr<WallpaperThumbsThumbFillWork> work(s_thumb_posted.exchange(nullptr, std::memory_order_acq_rel));
        if (work == nullptr) {
            Wallpaper_State().workers.thumb_busy.store(false, std::memory_order_release);
            if (s_thumb_posted.load(std::memory_order_acquire) != nullptr &&
                !Wallpaper_State().workers.thumb_busy.exchange(true, std::memory_order_acq_rel)) {
                continue;
            }
            return;
        }
        try {
            RunThumbWork(work.get());
        } catch (const std::bad_alloc&) {
            work->error = ImageDecodeError::kNoMemory;
            ESP_LOGE(TAG, "thumbnail batch failed: no memory");
        }
        ScreenCompleteWork(work.release());
    }
}

static bool StartThumbTask() {
    return TaskExecutor::Schedule([] { ThumbFillTask(nullptr); }, TaskDomain::Image);
}

void CancelThumbFill() {
    s_thumb_seq.fetch_add(1, std::memory_order_acq_rel);
    auto* old = s_thumb_posted.exchange(nullptr, std::memory_order_acq_rel);
    delete old;
}

void ScheduleThumbFill() try {
    if (Wallpaper_State().decode_paused.load() || Wallpaper_State().list.loading) {
        return;
    }
    if (!Wallpaper_State().screen_alive || Wallpaper_State().list.files.empty()) {
        CancelThumbFill();
        return;
    }
    if (Wallpaper_State().mode != UiMode::kList) {
        return;
    }
    Wallpaper_ClampListPage();
    const int start = 0;
    const int end =
        std::min(start + Wallpaper_State().list.list_per_page, static_cast<int>(Wallpaper_State().list.files.size()));
    auto work = std::make_unique<WallpaperThumbsThumbFillWork>();
    work->apply = ApplyThumbBatch;
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
        WallpaperThumbsThumbWorkItem item;
        item.index = i;
        item.bytes = Wallpaper_State().list.files[static_cast<size_t>(i)].size_bytes;
        item.mtime = Wallpaper_State().list.files[i].mtime;
        std::snprintf(item.path, sizeof(item.path), "%s", Wallpaper_State().list.files[static_cast<size_t>(i)].path);
        work->items.push_back(item);
    }
    if (work->items.empty()) {
        CancelThumbFill();
        return;
    }
    const unsigned n = static_cast<unsigned>(work->items.size());
    const int page = Wallpaper_State().list.list_page + 1;
    work->seq = s_thumb_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto* old = s_thumb_posted.exchange(work.release(), std::memory_order_acq_rel);
    delete old;
    ESP_LOGI(TAG, "thumb fill async n=%u page=%d", n, page);
    if (Wallpaper_State().workers.thumb_busy.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (!StartThumbTask()) {
        auto* leftover = s_thumb_posted.exchange(nullptr, std::memory_order_acq_rel);
        if (leftover != nullptr) {
            leftover->error = ImageDecodeError::kFailed;
            leftover->storage_error = {StorageErrorCode::Rejected, StorageStage::Submit};
            ScreenCompleteWork(leftover);
        }
        Wallpaper_State().workers.thumb_busy.store(false, std::memory_order_release);
    }
} catch (const std::bad_alloc&) {
    CancelThumbFill();
    ESP_LOGE(TAG, "thumbnail scheduling allocation failed");
    auto& state = Wallpaper_State();
    for (size_t i = 0; i < state.list.files.size() && i < state.list.thumbs.size(); ++i) {
        if (state.list.thumbs[i] == nullptr) {
            state.list.files[i].thumb_error = ImageDecodeError::kNoMemory;
            state.list.files[i].thumb_storage_error = {};
            PaintCellThumb(static_cast<int>(i));
        }
    }
}
