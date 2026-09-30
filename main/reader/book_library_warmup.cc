#include "book_library_warmup.h"

#include "book_home_snapshot.h"
#include "book_library.h"
#include "book_progress_cache.h"
#include "book_session.h"
#include "sd_paths.h"
#include "SdCardManager.hpp"

#include <atomic>
#include <sys/stat.h>
#include <utility>
#include <vector>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace reader {
namespace book_library_warmup {
namespace {

constexpr const char* TAG = "BookLibWarm";
/** DRAM 栈：扫盘 / Peek；禁止 PSRAM 栈碰 Flash 旁路路径以外的 NVS（本任务只碰 SD） */
constexpr uint32_t kWorkerStack = 10 * 1024;
constexpr UBaseType_t kWorkerPriority = tskIDLE_PRIORITY + 2;

std::atomic<bool> s_running{false};
std::atomic<uint8_t> s_phase{static_cast<uint8_t>(Phase::kIdle)};
std::atomic<uint32_t> s_peek_done{0};
std::atomic<uint32_t> s_peek_total{0};
std::atomic<bool> s_cache_ready{false};

void SetPhase(Phase phase) {
    s_phase.store(static_cast<uint8_t>(phase), std::memory_order_release);
    if (phase == Phase::kIdle || phase == Phase::kScan) {
        s_peek_done.store(0, std::memory_order_relaxed);
        s_peek_total.store(0, std::memory_order_relaxed);
    }
}

void SetPeekProgress(uint32_t done, uint32_t total) {
    s_peek_done.store(done, std::memory_order_relaxed);
    s_peek_total.store(total, std::memory_order_relaxed);
}

void PeekAllIntoCache(const std::vector<BookInfo>& books) {
    SetPhase(Phase::kPeek);
    const uint32_t n = static_cast<uint32_t>(books.size());
    SetPeekProgress(0, n);

    std::vector<std::pair<std::string, BookSession::ProgressPeek>> items;
    items.reserve(books.size());

    for (size_t i = 0; i < books.size(); ++i) {
        if ((i & 15u) == 0u) {
            vTaskDelay(1);
        }

        const BookInfo& info = books[i];
        SetPeekProgress(static_cast<uint32_t>(i + 1), n);

        struct stat st {};
        if (info.path.empty() || stat(info.path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            ESP_LOGD(TAG, "peek skip missing: %s", info.path.c_str());
            continue;
        }

        const BookSession::ProgressPeek peek = BookSession::PeekProgress(info.path.c_str());
        items.emplace_back(info.path, peek);
    }

    // Peek 窗口内可能删书：落缓存前再滤一遍，并据此写聚合（覆盖删书路径的扣减亦正确）
    std::vector<std::pair<std::string, BookSession::ProgressPeek>> kept;
    kept.reserve(items.size());
    uint32_t total_seconds = 0;
    int finished_count = 0;
    for (auto& it : items) {
        struct stat st {};
        if (stat(it.first.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (total_seconds <= UINT32_MAX - it.second.reading_seconds) {
            total_seconds += it.second.reading_seconds;
        } else {
            total_seconds = UINT32_MAX;
        }
        if (it.second.progress_x10 >= 1000) {
            ++finished_count;
        }
        kept.push_back(std::move(it));
    }

    book_progress_cache::ReplaceAll(std::move(kept));
    s_cache_ready.store(true, std::memory_order_release);
    book_home_snapshot::StoreAggregateStats(total_seconds, finished_count);

    ESP_LOGI(TAG, "peek done cached=%u agg_sec=%u finished=%d",
             static_cast<unsigned>(book_progress_cache::Size()),
             static_cast<unsigned>(total_seconds), finished_count);
}

void WorkerTask(void* /*arg*/) {
    {
        book_home_snapshot::HydrateFromNvs();

        ESP_LOGI(TAG, "开机书库预热: 开始");
        SetPhase(Phase::kScan);
        s_cache_ready.store(false, std::memory_order_release);

        if (!SdCardManager::GetInstance().IsMounted()) {
            ESP_LOGW(TAG, "开机书库预热: 跳过(SD 未挂载)");
            book_progress_cache::ReplaceAll({});
            s_cache_ready.store(true, std::memory_order_release);
        } else {
            std::vector<BookInfo> books;
            if (!ScanBookLibrary(kDefaultBooksDir, books)) {
                ESP_LOGW(TAG, "开机书库预热: 扫盘失败 dir=%s", kDefaultBooksDir);
                book_progress_cache::ReplaceAll({});
                s_cache_ready.store(true, std::memory_order_release);
            } else {
                OfferBookLibraryCache(books);
                ESP_LOGI(TAG, "开机书库预热: 扫盘完成 n=%u, 开始 Peek 进度/时长",
                         static_cast<unsigned>(books.size()));
                PeekAllIntoCache(books);
                books.clear();
                books.shrink_to_fit();
            }
        }

        ESP_LOGI(TAG, "开机书库预热: 结束");
    }

    SetPhase(Phase::kIdle);
    s_running.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

}  // namespace

void RequestBootWarmup() {
    bool expected = false;
    if (!s_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        ESP_LOGD(TAG, "warmup already running");
        return;
    }
    SetPhase(Phase::kScan);
    SetPeekProgress(0, 0);
    s_cache_ready.store(false, std::memory_order_release);

    const BaseType_t ok =
        xTaskCreatePinnedToCore(WorkerTask, "book_lib_warm", kWorkerStack, nullptr,
                                kWorkerPriority, nullptr, 0);
    if (ok != pdPASS) {
        SetPhase(Phase::kIdle);
        s_running.store(false, std::memory_order_release);
        s_cache_ready.store(false, std::memory_order_release);
        ESP_LOGW(TAG, "xTaskCreate(book_lib_warm) failed");
    }
}

bool IsBusy() {
    return s_running.load(std::memory_order_acquire);
}

Progress GetProgress() {
    Progress p;
    p.busy = s_running.load(std::memory_order_acquire);
    p.phase = static_cast<Phase>(s_phase.load(std::memory_order_acquire));
    p.done = s_peek_done.load(std::memory_order_relaxed);
    p.total = s_peek_total.load(std::memory_order_relaxed);
    if (!p.busy) {
        p.phase = Phase::kIdle;
    }
    return p;
}

bool IsProgressCacheReady() {
    return s_cache_ready.load(std::memory_order_acquire) && book_progress_cache::IsReady();
}

}  // namespace book_library_warmup
}  // namespace reader
