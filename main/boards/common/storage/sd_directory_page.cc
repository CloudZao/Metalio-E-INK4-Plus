#include "sd_directory_page.h"
#include "sd_access.h"
#include "sd_paths.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <new>
#include <utility>
#include <sys/stat.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>

static const char* TAG = "SdDirPage";
static std::mutex s_pages_mutex;
static std::vector<SdDirectoryPage*> s_pages;

void SdCancelDirectoryPages() {
    std::lock_guard<std::mutex> lock(s_pages_mutex);
    for (auto* page : s_pages) {
        page->Cancel();
    }
}

void SdDirectoryPage::Close() {
    if (cursor_) {
        closedir(cursor_);
    }
    cursor_ = nullptr;
    root_ = 0;
    position_ = 0;
}

void SdDirectoryPage::CloseCount() {
    if (count_cursor_) {
        closedir(count_cursor_);
    }
    count_cursor_ = nullptr;
    count_root_ = 0;
    count_so_far_ = 0;
}

void SdDirectoryPage::Request(std::vector<std::string> roots, Filter filter, int page, int size, bool refresh,
                              void (*enrich)(Entry&), bool names_only) {
    {
        std::lock_guard<std::mutex> lock(s_pages_mutex);
        if (std::find(s_pages.begin(), s_pages.end(), this) == s_pages.end()) {
            s_pages.push_back(this);
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const bool changed = roots != roots_ || filter != filter_ || size != size_ || names_only != names_only_ || refresh;
    ++generation_;
    roots_ = std::move(roots);
    filter_ = filter;
    enrich_ = enrich;
    names_only_ = names_only;
    page_ = std::max(0, page);
    size_ = std::max(1, size);
    reset_ = reset_ || changed;
    if (changed) {
        total_.store(-1);
        known_pages_.store(page_ + 1);
        cache_.clear();
    }
    pending_ = true;
    ready_ = Recall(page_, result_);
}

void SdDirectoryPage::Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++generation_;
    pending_ = false;
    ready_ = false;
    cache_.clear();
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
    return active_generation_ != generation_.load() || sd_access::IsQuiescing();
}

bool SdDirectoryPage::Cached(int page) {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(cache_.begin(), cache_.end(), [page](const auto& item) { return item.result.page == page; });
}

bool SdDirectoryPage::Recall(int page, Result& result) try {
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

void SdDirectoryPage::Remember(const Result& result) try {
    if (!result.ok) {
        return;
    }
    size_t bytes = sizeof(CachedPage) + result.entries.capacity() * sizeof(Entry);
    for (const auto& entry : result.entries) {
        bytes += entry.path.capacity() + entry.title.capacity() + 2;
    }
    // 目录小对象可能使用内部 RAM，不能仅按 PSRAM 余量决定缓存量。
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t free_total = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t budget = std::min<size_t>(64 * 1024, std::min(free_internal / 8, free_total / 16));
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
    }
} catch (const std::bad_alloc&) {
    ESP_LOGD(TAG, "directory cache allocation unavailable");
}

bool SdDirectoryPage::Poll(Result& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_ && !busy_.load() && !sd_access::IsQuiescing()) {
        busy_.store(true);
        active_generation_ = generation_.load();
        pending_ = false;
        // 壁纸预取可能解码缩略图，目录工作任务使用外部栈。
        if (xTaskCreatePinnedToCoreWithCaps(Worker, "sd_dir_page", 16 * 1024, this, tskIDLE_PRIORITY + 1, nullptr, 0,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            busy_.store(false);
            if (ready_) {
                ESP_LOGW(TAG, "directory prefetch task unavailable; using cached page");
            } else {
                result_ = {};

                result_.page = page_;
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

bool SdDirectoryPage::Next(Entry& entry, bool& error) {
    while (root_ < cursor_roots_.size()) {
        if (active_generation_ != generation_.load() || sd_access::IsQuiescing()) {
            return false;
        }
        if (!cursor_) {
            cursor_ = opendir(cursor_roots_[root_].c_str());
            if (!cursor_) {
                if (errno != ENOENT) {
                    error = true;
                    return false;
                }
                ++root_;
                continue;
            }
        }
        errno = 0;
        dirent* ent = readdir(cursor_);
        if (!ent) {
            if (errno) {
                error = true;
                return false;
            }
            closedir(cursor_);
            cursor_ = nullptr;
            ++root_;
            continue;
        }
        // 无效文件很多时也要让出执行时间，并逐项检查取消。
        if ((++visited_ & 15u) == 0) {
            vTaskDelay(1);
        }
        const bool directory = ent->d_type == DT_DIR;
        if (ent->d_name[0] == '.' || !SdIsValidFileName(ent->d_name)) {
            continue;
        }
        char path[SD_FILE_PATH_CAPACITY];
        const int length = std::snprintf(path, sizeof(path), "%s/%s", cursor_roots_[root_].c_str(), ent->d_name);
        if (length < 0 || size_t(length) >= sizeof(path) || !cursor_filter_(path, directory)) {
            continue;
        }
        entry.metric = 0;
        entry.title.clear();
        entry.path = path;
        entry.directory = directory;
        ++position_;
        return true;
    }
    return false;
}

void SdDirectoryPage::Run(unsigned generation) {
    active_generation_ = generation;
    int page, size;
    bool names_only;
    void (*enrich)(Entry&);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        page = page_;
        size = size_;
        enrich = enrich_;
        names_only = names_only_;
        if (reset_ || roots_ != cursor_roots_ || filter_ != cursor_filter_) {
            Close();
            CloseCount();
            cursor_roots_ = roots_;
            cursor_filter_ = filter_;
            reset_ = false;
        }
    }
    const auto cancelled = [&] { return generation != generation_.load() || sd_access::IsQuiescing(); };
    bool error = false;
    size_t visited = 0;
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
            if (!names_only) {
                struct stat st{};
                if (stat(entry.path.c_str(), &st) == 0) {
                    entry.bytes = st.st_size;
                    entry.mtime = st.st_mtime;
                } else {
                    entry.bytes = 0;
                    entry.mtime = 0;
                    if (!prefetch) {
                        ESP_LOGW(TAG, "page entry stat failed errno=%d", errno);
                    }
                }
            }
            entry.prefetch = prefetch;
            entry.prefetch_distance = prefetch ? target - page : 0;
            if (!names_only && enrich) {
                enrich(entry);
            }
            result.entries.push_back(std::move(entry));
        }
        result.more = result.entries.size() == size_t(size);
        result.ok = !error;
        return result;
    };
    Result result;
    bool cached;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cached = Recall(page, result);
    }
    if (!cached) {
        result = read_page(page, false);
    }
    if (cancelled()) {
        return;
    }
    if (!result.ok) {
        ESP_LOGE(TAG, "directory page read failed errno=%d", errno);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelled()) {
            return;
        }
        known_pages_.store(std::max(known_pages_.load(), page + (result.more ? 2 : 1)));
        if (!cached) {
            Remember(result);
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
        if (esp_timer_get_time() - prefetch_start > 600000) {
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
        if (std::min<size_t>(64 * 1024, free_internal / 8) < size_t(target - page + 1) * page_bytes) {
            break;
        }
        result = read_page(target, true);
        if (cancelled() || !result.ok) {
            break;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelled()) {
            return;
        }
        Remember(result);
    }
    if (cancelled()) {
        return;
    }
    error = false;
    if (total_.load() >= 0) {
        return;
    }
    // 独立计数句柄不改变下一页游标；不读取 stat、元数据或文件内容。
    while (count_root_ < cursor_roots_.size() && !cancelled()) {
        const auto& root = cursor_roots_[count_root_];
        if (!count_cursor_) {
            count_cursor_ = opendir(root.c_str());
            if (!count_cursor_) {
                if (errno == ENOENT) {
                    ++count_root_;
                    continue;
                }
                error = true;
                break;
            }
        }
        while (!cancelled()) {
            errno = 0;
            dirent* ent = readdir(count_cursor_);
            if (!ent) {
                if (errno) {
                    error = true;
                }
                closedir(count_cursor_);
                count_cursor_ = nullptr;
                ++count_root_;
                break;
            }
            if (ent->d_name[0] != '.' && SdIsValidFileName(ent->d_name)) {
                char path[SD_FILE_PATH_CAPACITY];
                const int length = std::snprintf(path, sizeof(path), "%s/%s", root.c_str(), ent->d_name);
                if (length >= 0 && size_t(length) < sizeof(path) && cursor_filter_(path, ent->d_type == DT_DIR)) {
                    ++count_so_far_;
                }
            }
            if ((++visited & 15u) == 0) {
                vTaskDelay(1);
            }
        }
        if (error) {
            break;
        }
    }
    if (cancelled()) {
        return;
    }
    const int total = count_so_far_;
    CloseCount();
    if (error) {
        ESP_LOGE(TAG, "directory count failed errno=%d", errno);
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation == generation_.load()) {
            result_ = {};
            result_.page = page;
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

void SdDirectoryPage::Worker(void* arg) {
    auto* self = static_cast<SdDirectoryPage*>(arg);
    const unsigned generation = self->active_generation_;
    try {
        sd_access::Transaction transaction;
        if (transaction) {
            self->Run(generation);
        } else {
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (generation == self->generation_.load()) {
                self->result_ = {};
                self->result_.page = self->page_;
                self->ready_ = true;
            }
            ESP_LOGW(TAG, "directory access unavailable");
        }
    } catch (const std::bad_alloc&) {
        std::lock_guard<std::mutex> lock(self->mutex_);
        self->Close();
        self->CloseCount();
        if (generation == self->generation_.load()) {
            self->result_ = {};
            self->result_.page = self->page_;
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
