#include <algorithm>
#include <atomic>
#include <mutex>
#include <new>
#include <string>
#include <vector>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "list_thumbnail_cache.h"
#include "infra/diagnostics/firmware_memory.h"
#include "boards/common/storage/sd_access.h"

struct CachedListThumbnail {
    std::string path;           // 文件标识，仅用于缓存匹配，不输出日志
    int width = 0, height = 0;  // 请求的解码边界
    size_t bytes = 0;           // 文件版本：长度
    int64_t mtime = 0;          // 文件版本：修改时间
    uint64_t used = 0;          // 最近使用次序
    RasterImage image;          // 缓存持有像素，控件使用独立副本
};

static std::mutex s_mutex;
static std::vector<CachedListThumbnail> s_cache;
static uint64_t s_used = 0;
static std::atomic<bool> s_reclaim_pending{false};
static unsigned s_mount_generation = 0;  // 缓存所属介质代次

static void CheckMediumLocked() {
    const unsigned generation = SdAccessMountGeneration();
    const bool reclaim = s_reclaim_pending.exchange(false);
    if (generation != s_mount_generation || reclaim) {
        std::vector<CachedListThumbnail>().swap(s_cache);
        s_mount_generation = generation;
    }
}

static constexpr size_t kCapacity = 2 * 1024 * 1024;  // 书架封面与壁纸缩略图共享的像素缓存上限

static size_t TrimListThumbnails() {
    size_t used = 0;
    for (const auto& item : s_cache) {
        used += item.image.pixels.capacity();
    }
    const size_t budget = std::min(kCapacity, FirmwarePsramCacheBudget(used));
    while (!s_cache.empty() && used > budget) {
        const auto oldest = std::min_element(s_cache.begin(), s_cache.end(),
                                             [](const auto& a, const auto& b) { return a.used < b.used; });
        used -= oldest->image.pixels.capacity();
        s_cache.erase(oldest);
    }
    return budget;
}

static void ReclaimListThumbnails(void*) {
    s_reclaim_pending.store(true);
    std::unique_lock<std::mutex> lock(s_mutex, std::try_to_lock);
    if (lock.owns_lock()) {
        s_reclaim_pending.store(false);
        std::vector<CachedListThumbnail>().swap(s_cache);
    }
}

bool GetListThumbnail(const char* path, int width, int height, size_t bytes, int64_t mtime, RasterImage& out) try {
    if (!path) {
        return false;
    }
    FirmwareMemoryDemand demand(size_t(std::max(0, width)) * std::max(0, height));
    std::lock_guard<std::mutex> lock(s_mutex);
    CheckMediumLocked();
    TrimListThumbnails();
    for (auto& item : s_cache) {
        if (item.path != path || item.width != width || item.height != height || item.bytes != bytes ||
            item.mtime != mtime) {
            continue;
        }
        if (!FirmwarePsramCanAllocate(item.image.pixels.size())) {
            return false;
        }
        out = item.image;
        item.used = ++s_used;
        return true;
    }
    return false;
} catch (const std::bad_alloc&) {
    ESP_LOGD("ListThumbCache", "cache copy unavailable");
    return false;
}

bool ListThumbnailUnavailable(const char* path, size_t bytes, int64_t mtime) {
    if (!path) {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    CheckMediumLocked();
    for (auto& item : s_cache) {
        if (item.path == path && item.bytes == bytes && item.mtime == mtime && item.image.empty()) {
            item.used = ++s_used;
            return true;
        }
    }
    return false;
}

void CacheListThumbnail(const char* path, int width, int height, size_t bytes, int64_t mtime,
                        const RasterImage& image) try {
    if (!path) {
        return;
    }
    static FirmwareCacheReclaimer reclaimer(ReclaimListThumbnails, nullptr);
    FirmwarePrefetchScope prefetch(image.pixels.size());
    if (!prefetch.Valid()) {
        return;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    CheckMediumLocked();
    const uint32_t reclaim_generation = FirmwareMemoryReclaimGeneration();
    size_t used = 0;
    for (auto it = s_cache.begin(); it != s_cache.end();) {
        if (it->path == path && it->width == width && it->height == height) {
            it = s_cache.erase(it);
        } else {
            used += it->image.pixels.capacity();
            ++it;
        }
    }
    const size_t budget = std::min(kCapacity, FirmwarePsramCacheBudget(used));
    if (image.pixels.size() > budget) {
        return;
    }
    while (!s_cache.empty() && (used + image.pixels.size() > budget || s_cache.size() >= 48)) {
        const auto oldest = std::min_element(s_cache.begin(), s_cache.end(),
                                             [](const auto& a, const auto& b) { return a.used < b.used; });
        used -= oldest->image.pixels.capacity();
        s_cache.erase(oldest);
    }
    if (!prefetch.Valid()) {
        return;
    }
    s_cache.push_back({path, width, height, bytes, mtime, ++s_used, image});
    if (reclaim_generation != FirmwareMemoryReclaimGeneration()) {
        s_reclaim_pending.store(false);
        std::vector<CachedListThumbnail>().swap(s_cache);
    }
} catch (const std::bad_alloc&) {
    ESP_LOGD("ListThumbCache", "cache storage unavailable");
}

void ClearListThumbnailCache() {
    std::lock_guard<std::mutex> lock(s_mutex);
    std::vector<CachedListThumbnail>().swap(s_cache);
    s_mount_generation = SdAccessMountGeneration();
    s_reclaim_pending.store(false);
}
