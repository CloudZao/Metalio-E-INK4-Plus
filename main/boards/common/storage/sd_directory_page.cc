#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <sys/stat.h>
#include "sd_directory_page.h"
#include "sd_access.h"
#include "infra/diagnostics/firmware_memory.h"
#include "infra/tasks/task_create.h"
#include "infra/storage/sd_paths.h"

static const char* TAG = "SdDirPage";
static std::mutex s_pages_mutex;
static std::vector<SdDirectoryPage*> s_pages;

void SdDirectoryPage::Request(std::vector<std::string> roots, Filter filter, int page, int size, bool refresh,
                              void (*enrich)(Entry&), bool names_only, const std::string& name_query, bool recursive,
                              const std::string& locate_path) {
    static FirmwareCacheReclaimer reclaimer(ReclaimCaches, nullptr);
    {
        std::lock_guard<std::mutex> lock(s_pages_mutex);
        if (std::find(s_pages.begin(), s_pages.end(), this) == s_pages.end()) {
            s_pages.push_back(this);
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const bool changed = roots != roots_ || filter != filter_ || size != size_ || names_only != names_only_ ||
                         name_query != name_query_ || recursive != recursive_ || refresh;
    ++generation_;
    roots_ = std::move(roots);
    filter_ = filter;
    name_query_ = name_query;
    locate_path_ = locate_path;
    recursive_ = recursive;
    enrich_ = enrich;
    names_only_ = names_only;
    page_ = std::max(0, page);
    size_ = std::max(1, size);
    reset_ = reset_ || changed;
    if (changed) {
        total_.store(-1);
        known_pages_.store(page_ + 1);
        std::vector<CachedPage>().swap(cache_);
    }
    pending_ = true;
    ready_ = locate_path.empty() && Recall(page_, result_);
}

bool SdDirectoryPage::Poll(Result& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_ && !busy_.load() && !SdAccessIsQuiescing()) {
        busy_.store(true);
        active_generation_ = generation_.load();
        pending_ = false;
        // 壁纸预取可能解码缩略图，目录工作任务使用外部栈。
        if (FirmwareCreateTask(Worker, "sd_dir_page", 16 * 1024, this, tskIDLE_PRIORITY + 1, nullptr, 0,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            busy_.store(false);
            if (ready_) {
                ESP_LOGW(TAG, "directory prefetch task unavailable; using cached page");
            } else {
                result_ = {};

                result_.page = page_;
                result_.error = {StorageErrorCode::Rejected, StorageStage::Submit};
                ready_ = true;
                ESP_LOGE(TAG, "directory task creation failed");
            }
        }
    }
    if (!ready_) {
        return false;
    }
    result = std::move(result_);
    ready_ = false;
    return true;
}

void SdDirectoryPage::Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++generation_;
    pending_ = false;
    ready_ = false;
    std::vector<CachedPage>().swap(cache_);
    result_ = {};
    reset_ = true;
    total_.store(-1);
    known_pages_.store(1);
    if (!busy_.load()) {
        Close();
        CloseCount();
    }
}

int SdDirectoryPage::Pages() const {
    const int total = total_.load();
    return total < 0 ? known_pages_.load() : std::max(1, (total + size_ - 1) / size_);
}

bool SdDirectoryPage::Cancelled() const {
    return active_generation_ != generation_.load() || SdAccessIsQuiescing();
}

bool SdDirectoryPage::Cached(int page) {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(cache_.begin(), cache_.end(), [page](const auto& item) { return item.result.page == page; });
}

void SdDirectoryPage::ReclaimCaches(void*) {
    std::unique_lock<std::mutex> pages_lock(s_pages_mutex, std::try_to_lock);
    if (!pages_lock.owns_lock()) {
        return;
    }
    for (auto* page : s_pages) {
        page->reclaim_pending_.store(true);
        std::unique_lock<std::mutex> lock(page->mutex_, std::try_to_lock);
        if (lock.owns_lock()) {
            std::vector<CachedPage>().swap(page->cache_);
            page->reclaim_pending_.store(false);
        }
    }
}

void SdDirectoryPage::Remember(const Result& result, uint32_t reclaim_generation) try {
    if (reclaim_pending_.exchange(false)) {
        std::vector<CachedPage>().swap(cache_);
    }
    if (!result.ok || !FirmwarePrefetchAllowed() ||
        reclaim_generation != FirmwareMemoryReclaimGeneration()) {
        return;
    }
    size_t bytes = sizeof(CachedPage) + result.entries.capacity() * sizeof(Entry);
    for (const auto& entry : result.entries) {
        bytes += entry.path.capacity() + entry.title.capacity() + 2;
    }
    // 目录小对象可能使用内部 RAM，不能仅按 PSRAM 余量决定缓存量。
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t free_total = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t headroom = free_internal > kFirmwareInternalFloorBytes ? free_internal - kFirmwareInternalFloorBytes : 0;
    const size_t budget = std::min({size_t{64 * 1024}, free_internal / 8, free_total / 16, headroom});
    size_t used = 0;
    for (const auto& item : cache_) {
        used += item.bytes;
    }
    while (!cache_.empty() && (used + bytes > budget || cache_.size() >= 8)) {
        const auto oldest = std::min_element(cache_.begin(), cache_.end(),
                                             [](const auto& a, const auto& b) { return a.used < b.used; });
        used -= oldest->bytes;
        cache_.erase(oldest);
    }
    if (bytes <= budget) {
        cache_.push_back({result, bytes, ++cache_used_});
        if (reclaim_pending_.exchange(false) || reclaim_generation != FirmwareMemoryReclaimGeneration()) {
            std::vector<CachedPage>().swap(cache_);
        }
    }
} catch (const std::bad_alloc&) {
    ESP_LOGD(TAG, "directory cache allocation unavailable");
}

bool SdDirectoryPage::Recall(int page, Result& result) try {
    if (reclaim_pending_.exchange(false)) {
        std::vector<CachedPage>().swap(cache_);
    }
    for (auto& item : cache_) {
        if (item.result.page != page) {
            continue;
        }
        item.used = ++cache_used_;
        result = item.result;
        return true;
    }
    return false;
} catch (const std::bad_alloc&) {
    ESP_LOGD(TAG, "directory cache copy unavailable; read page on demand");
    return false;
}

void SdDirectoryPage::Worker(void* arg) {
    auto* self = static_cast<SdDirectoryPage*>(arg);
    const unsigned generation = self->active_generation_;
    try {
        SdAccessTransaction transaction;
        if (transaction) {
            self->Run(generation);
        } else {
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (generation == self->generation_.load()) {
                self->result_ = {};
                self->result_.page = self->page_;
                self->result_.error = transaction.Error();
                self->ready_ = true;
            }
            LogStorageError(TAG, transaction.Error());
        }
    } catch (const std::bad_alloc&) {
        std::lock_guard<std::mutex> lock(self->mutex_);
        self->Close();
        self->CloseCount();
        if (generation == self->generation_.load()) {
            self->result_ = {};
            self->result_.page = self->page_;
            self->result_.error = {StorageErrorCode::NoMemory, StorageStage::DirectoryRead, ENOMEM};
            self->ready_ = true;
        }
        ESP_LOGE(TAG, "directory page allocation failed");
    }
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        if (!self->pending_ && generation != self->generation_.load()) {
            self->Close();
            self->CloseCount();
        }
        self->busy_.store(false);
    }
    vTaskDeleteWithCaps(nullptr);
}

bool SdDirectoryPage::Matches(const char* path, bool directory) const {
    if (!cursor_filter_(path, directory)) {
        return false;
    }
    if (cursor_name_query_.empty()) {
        return true;
    }
    if (directory) {
        return false;
    }
    const char* name = std::strrchr(path, '/');
    name = name ? name + 1 : path;
    const auto fold = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    return std::search(name, name + std::strlen(name), cursor_name_query_.begin(), cursor_name_query_.end(),
                       [&](unsigned char a, unsigned char b) { return fold(a) == fold(b); }) != name + std::strlen(name);
}

bool SdDirectoryPage::NextDirectoryEntry(bool counting, Entry& entry, StorageError& error) {
    DIR*& cursor = counting ? count_cursor_ : cursor_;
    size_t& root = counting ? count_root_ : root_;
    auto& path = counting ? count_path_ : cursor_path_;
    auto& parents = counting ? count_parents_ : cursor_parents_;
    while (root < cursor_roots_.size()) {
        if (Cancelled()) {
            return false;
        }
        if (!cursor) {
            if (path.empty()) {
                path = cursor_roots_[root];
            }
            cursor = opendir(path.c_str());
            if (!cursor) {
                if (errno == ENOENT && cursor_roots_.size() != 1 && parents.empty()) {
                    ++root;
                    path.clear();
                    continue;
                }
                error = StorageErrorFromErrno(StorageStage::DirectoryOpen, errno);
                return false;
            }
        }
        errno = 0;
        dirent* ent = readdir(cursor);
        if (!ent) {
            if (errno) {
                error = StorageErrorFromErrno(StorageStage::DirectoryRead, errno);
                return false;
            }
            closedir(cursor);
            cursor = nullptr;
            if (!parents.empty()) {
                const long offset = parents.back();
                parents.pop_back();
                path.resize(path.find_last_of('/'));
                cursor = opendir(path.c_str());
                if (!cursor) {
                    error = StorageErrorFromErrno(StorageStage::DirectoryOpen, errno);
                    return false;
                }
                errno = 0;
                seekdir(cursor, offset);
                if (errno) {
                    error = StorageErrorFromErrno(StorageStage::DirectoryRead, errno);
                    return false;
                }
            } else {
                ++root;
                path.clear();
            }
            continue;
        }
        if ((++visited_ & 15u) == 0) {
            vTaskDelay(1);
        }
        if (ent->d_name[0] == '.' || !SdIsValidFileName(ent->d_name)) {
            continue;
        }
        const bool directory = ent->d_type == DT_DIR;
        char full_path[SD_FILE_PATH_CAPACITY];
        const int length = std::snprintf(full_path, sizeof(full_path), "%s/%s", path.c_str(), ent->d_name);
        if (length < 0 || size_t(length) >= sizeof(full_path)) {
            error = StorageErrorFromErrno(StorageStage::DirectoryRead, ENAMETOOLONG);
            return false;
        }
        if (directory && cursor_recursive_) {
            const long offset = telldir(cursor);
            if (offset < 0) {
                error = StorageErrorFromErrno(StorageStage::DirectoryRead, errno ? errno : EIO);
                return false;
            }
            if (parents.size() == parents.capacity()) {
                const size_t bytes = std::max<size_t>(1, parents.capacity() * 2) * sizeof(long);
                if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) <
                    kFirmwareInternalFloorBytes + bytes) {
                    error = {StorageErrorCode::NoMemory, StorageStage::DirectoryRead, ENOMEM};
                    return false;
                }
            }
            parents.push_back(offset);
            closedir(cursor);
            cursor = nullptr;
            path = full_path;
            continue;
        }
        if (!Matches(full_path, directory)) {
            continue;
        }
        entry.metric = 0;
        entry.title.clear();
        entry.path = full_path;
        entry.directory = directory;
        return true;
    }
    return false;
}

bool SdDirectoryPage::Next(Entry& entry, StorageError& error) {
    if (!NextDirectoryEntry(false, entry, error)) {
        return false;
    }
    ++position_;
    return true;
}

void SdDirectoryPage::Close() {
    if (cursor_) {
        closedir(cursor_);
    }
    cursor_ = nullptr;
    root_ = 0;
    position_ = 0;
    cursor_path_.clear();
    cursor_parents_.clear();
}

void SdDirectoryPage::CloseCount() {
    if (count_cursor_) {
        closedir(count_cursor_);
    }
    count_cursor_ = nullptr;
    count_root_ = 0;
    count_so_far_ = 0;
    count_path_.clear();
    count_parents_.clear();
}

void SdDirectoryPage::LocatePage(const std::string& path, int size, int& page, StorageError& error) {
    Close();
    Entry entry;
    while (!Cancelled() && Next(entry, error)) {
        if (entry.path == path) {
            page = static_cast<int>((position_ - 1) / size);
            break;
        }
    }
    Close();
}

void SdDirectoryPage::Run(unsigned generation) {
    const uint32_t reclaim_generation = FirmwareMemoryReclaimGeneration();
    active_generation_ = generation;
    int page, size;
    bool names_only;
    std::string locate_path;
    void (*enrich)(Entry&);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        page = page_;
        size = size_;
        enrich = enrich_;
        names_only = names_only_;
        locate_path = locate_path_;
        if (reset_ || roots_ != cursor_roots_ || filter_ != cursor_filter_) {
            Close();
            CloseCount();
            cursor_roots_ = roots_;
            cursor_filter_ = filter_;
            cursor_name_query_ = name_query_;
            cursor_recursive_ = recursive_;
            reset_ = false;
        }
    }
    const auto cancelled = [&] { return generation != generation_.load() || SdAccessIsQuiescing(); };
    StorageError error;
    if (!locate_path.empty()) {
        LocatePage(locate_path, size, page, error);
        if (cancelled()) {
            return;
        }
    }
    auto read_page = [&](int target, bool prefetch) {
        const size_t start = size_t(target) * size;
        if (position_ > start) {
            Close();
        }
        Result result;
        result.page = target;
        Entry entry;
        while (!cancelled() && position_ < start) {
            if (!Next(entry, error)) {
                break;
            }
        }
        while (!cancelled() && result.entries.size() < size_t(size)) {
            if (!Next(entry, error)) {
                break;
            }
            entry.bytes = 0;
            entry.mtime = 0;
            entry.error = {};
            if (!names_only) {
                struct stat st{};
                if (stat(entry.path.c_str(), &st) == 0) {
                    entry.bytes = st.st_size;
                    entry.mtime = st.st_mtime;
                } else {
                    entry.error = StorageErrorFromErrno(StorageStage::Stat, errno);
                    entry.bytes = 0;
                    entry.mtime = 0;
                    if (!prefetch) {
                        ESP_LOGW(TAG, "entry stage=stat code=%u errno=%d", unsigned(entry.error.code), entry.error.system_error);
                    }
                }
            }
            entry.prefetch = prefetch;
            if (!names_only && enrich) {
                enrich(entry);
            }
            result.entries.push_back(std::move(entry));
        }
        result.more = result.entries.size() == size_t(size);
        result.error = error;
        result.ok = !error;
        return result;
    };
    Result result;
    bool cached;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cached = !error && Recall(page, result);
    }
    if (error) {
        result.page = page;
        result.error = error;
    } else if (!cached) {
        result = read_page(page, false);
    }
    if (cancelled()) {
        return;
    }
    if (!result.ok) {
        LogStorageError(TAG, result.error);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelled()) {
            return;
        }
        known_pages_.store(std::max(known_pages_.load(), page + (result.more ? 2 : 1)));
        if (!cached || !locate_path.empty()) {
            if (!cached) {
                Remember(result, reclaim_generation);
            }
            result_ = result;
            ready_ = true;
        }
    }
    if (!result.ok) {
        return;
    }
    vTaskPrioritySet(nullptr, tskIDLE_PRIORITY);
    // 最多向前四页，时间和内存任一达到预算即结束；前台请求随时作废预取。
    size_t page_bytes = sizeof(CachedPage) + result.entries.capacity() * sizeof(Entry);
    for (const auto& entry : result.entries) {
        page_bytes += entry.path.capacity() + entry.title.capacity() + 2;
    }
    vTaskDelay(1);
    const int64_t prefetch_start = esp_timer_get_time();
    for (int target = page + 1; target <= page + 4 && result.more && !cancelled(); ++target) {
        FirmwarePrefetchScope prefetch(page_bytes);
        if (!prefetch.Valid() || reclaim_generation != FirmwareMemoryReclaimGeneration() ||
            esp_timer_get_time() - prefetch_start > 600000) {
            break;
        }
        if (total_.load() >= 0 && target >= Pages()) {
            break;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (Recall(target, result)) {
                continue;
            }
        }
        const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t headroom = free_internal > kFirmwareInternalFloorBytes ? free_internal - kFirmwareInternalFloorBytes : 0;
        if (std::min({size_t{64 * 1024}, free_internal / 8, headroom}) < size_t(target - page + 1) * page_bytes) {
            break;
        }
        result = read_page(target, true);
        if (cancelled() || !result.ok || !prefetch.Valid()) {
            break;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelled()) {
            return;
        }
        Remember(result, reclaim_generation);
    }
    if (cancelled()) {
        return;
    }
    error = {};
    if (total_.load() >= 0) {
        return;
    }
    // 独立计数句柄不改变下一页游标；不读取 stat、元数据或文件内容。
    Entry counted_entry;
    while (!cancelled() && NextDirectoryEntry(true, counted_entry, error)) {
        ++count_so_far_;
    }
    if (cancelled()) {
        return;
    }
    const int total = count_so_far_;
    CloseCount();
    if (error) {
        LogStorageError(TAG, error);
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation == generation_.load()) {
            result_ = {};
            result_.page = page;
            result_.error = error;
            ready_ = true;
        }
        Close();
    } else {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation == generation_.load()) {
            total_.store(total);
        }
    }
}

void SdCancelDirectoryPages() {
    std::lock_guard<std::mutex> lock(s_pages_mutex);
    for (auto* page : s_pages) {
        page->Cancel();
    }
}
