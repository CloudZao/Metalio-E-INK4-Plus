#include "book_home_snapshot.h"

#include "book_library.h"
#include "settings.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <sys/stat.h>

namespace reader {
namespace book_home_snapshot {
namespace {

constexpr const char* TAG = "BookHomeSnap";
/** 与 book_reader_prefs 同 ns，键名 ≤15 字符（ESP-IDF NVS 限制） */
constexpr const char* kNvsNs = "reader";
constexpr const char* kKeyRec[kRecentMax] = {"rec0", "rec1", "rec2", "rec3"};
constexpr const char* kKeyHsSec = "hs_sec";
constexpr const char* kKeyHsFin = "hs_fin";
constexpr const char* kKeyHsOk = "hs_ok";
/** 单条相对路径上限，避免撑爆 NVS 字符串 */
constexpr size_t kMaxRelLen = 200;
/** 异步落盘：必须 INTERNAL 栈，禁止 PSRAM 栈上碰 Flash；含 Settings/string 余量 */
constexpr uint32_t kPersistStack = 6 * 1024;

std::mutex s_mu;
bool s_hydrated = false;
/** NoteOpenedBook / RemoveBook 已改内存时，Hydrate 不得用 NVS 覆盖 MRU */
bool s_recent_dirty = false;
/** StoreAggregateStats 已写入内存时，Hydrate 不得用旧 NVS 覆盖聚合 */
bool s_agg_dirty = false;
std::string s_recent_rel[kRecentMax];
AggregateStats s_agg;

std::atomic<bool> s_persist_recent_pending{false};
std::atomic<bool> s_persist_agg_pending{false};
std::atomic<bool> s_persist_task_running{false};
/** 删书等变更代数（仅 RemoveBook 递增） */
std::atomic<uint32_t> s_mutation_epoch{0};

const char* BooksRoot() {
    return kDefaultBooksDir;
}

bool IsSafeRelPath(const char* rel) {
    if (rel == nullptr || rel[0] == '\0') {
        return false;
    }
    const size_t n = std::strlen(rel);
    if (n == 0 || n > kMaxRelLen) {
        return false;
    }
    if (rel[0] == '/' || rel[0] == '\\') {
        return false;
    }
    size_t seg_start = 0;
    for (size_t i = 0; i <= n; ++i) {
        if (i < n && (rel[i] == '\\' || rel[i] == ':')) {
            return false;
        }
        if (i == n || rel[i] == '/') {
            const size_t seg_len = i - seg_start;
            if (seg_len == 0) {
                return false;
            }
            if (seg_len == 1 && rel[seg_start] == '.') {
                return false;
            }
            if (seg_len == 2 && rel[seg_start] == '.' && rel[seg_start + 1] == '.') {
                return false;
            }
            seg_start = i + 1;
        }
    }
    return true;
}

bool AbsToRel(const char* abs, std::string& out) {
    out.clear();
    if (abs == nullptr || abs[0] == '\0') {
        return false;
    }
    const char* root = BooksRoot();
    const size_t root_len = std::strlen(root);
    if (root_len == 0) {
        return false;
    }
    if (std::strncmp(abs, root, root_len) != 0) {
        return false;
    }
    if (abs[root_len] == '\0' || abs[root_len] != '/') {
        return false;
    }
    const char* rel = abs + root_len + 1;
    if (!IsSafeRelPath(rel)) {
        return false;
    }
    out.assign(rel);
    return true;
}

std::string RelToAbs(const std::string& rel) {
    if (!IsSafeRelPath(rel.c_str())) {
        return {};
    }
    std::string abs = BooksRoot();
    abs.push_back('/');
    abs += rel;
    return abs;
}

bool FileExists(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    struct stat st {};
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

void CompactRecentLocked() {
    size_t w = 0;
    for (size_t i = 0; i < kRecentMax; ++i) {
        if (s_recent_rel[i].empty()) {
            continue;
        }
        if (w != i) {
            s_recent_rel[w] = std::move(s_recent_rel[i]);
            s_recent_rel[i].clear();
        }
        ++w;
    }
    for (size_t i = w; i < kRecentMax; ++i) {
        s_recent_rel[i].clear();
    }
}

void PersistRecentNow() {
    std::string snapshot[kRecentMax];
    {
        std::lock_guard<std::mutex> lock(s_mu);
        for (size_t i = 0; i < kRecentMax; ++i) {
            snapshot[i] = s_recent_rel[i];
        }
    }
    Settings settings(kNvsNs, true);
    for (size_t i = 0; i < kRecentMax; ++i) {
        if (snapshot[i].empty()) {
            settings.EraseKey(kKeyRec[i]);
        } else {
            settings.SetString(kKeyRec[i], snapshot[i]);
        }
    }
}

void PersistAggNow() {
    AggregateStats snap;
    {
        std::lock_guard<std::mutex> lock(s_mu);
        snap = s_agg;
    }
    Settings settings(kNvsNs, true);
    settings.SetString(kKeyHsSec, std::to_string(snap.total_seconds));
    settings.SetInt(kKeyHsFin, snap.finished_count);
    settings.SetInt(kKeyHsOk, snap.valid ? 1 : 0);
}

void PersistTask(void* /*arg*/) {
    // 单任务排空 pending：写完再看一眼，避免「退出窗口」丢尾写；禁止在持 s_mu 时开 NVS
    for (;;) {
        const bool do_recent = s_persist_recent_pending.exchange(false, std::memory_order_acq_rel);
        const bool do_agg = s_persist_agg_pending.exchange(false, std::memory_order_acq_rel);
        if (do_recent) {
            PersistRecentNow();
            ESP_LOGI(TAG, "nvs recent flushed");
        }
        if (do_agg) {
            PersistAggNow();
            ESP_LOGI(TAG, "nvs aggregate flushed");
        }
        if (!do_recent && !do_agg) {
            // 先清 running，再二次确认 pending（与 SchedulePersist 的 CAS 对齐）
            s_persist_task_running.store(false, std::memory_order_release);
            if (s_persist_recent_pending.load(std::memory_order_acquire) ||
                s_persist_agg_pending.load(std::memory_order_acquire)) {
                bool expected = false;
                if (s_persist_task_running.compare_exchange_strong(expected, true,
                                                                   std::memory_order_acq_rel)) {
                    continue;  // 本任务续跑，不必再 create
                }
            }
            break;
        }
    }
    vTaskDelete(nullptr);
}

void SchedulePersist(bool recent, bool agg) {
    if (recent) {
        s_persist_recent_pending.store(true, std::memory_order_release);
    }
    if (agg) {
        s_persist_agg_pending.store(true, std::memory_order_release);
    }
    bool expected = false;
    if (!s_persist_task_running.compare_exchange_strong(expected, true,
                                                       std::memory_order_acq_rel)) {
        return;  // 已有任务会排空 pending
    }
    const BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        PersistTask, "book_home_nvs", kPersistStack, nullptr, 5, nullptr, 0,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        s_persist_task_running.store(false, std::memory_order_release);
        ESP_LOGW(TAG, "xTaskCreate(book_home_nvs) failed; pending kept");
    }
}

void HydrateLocked() {
    if (s_hydrated) {
        return;
    }
    Settings settings(kNvsNs, false);
    if (!s_recent_dirty) {
        for (size_t i = 0; i < kRecentMax; ++i) {
            s_recent_rel[i].clear();
            const std::string v = settings.GetString(kKeyRec[i], "");
            if (IsSafeRelPath(v.c_str())) {
                s_recent_rel[i] = v;
            }
        }
        CompactRecentLocked();
    }

    if (!s_agg_dirty) {
        s_agg = {};
        if (settings.GetInt(kKeyHsOk, 0) != 0) {
            const std::string sec = settings.GetString(kKeyHsSec, "0");
            char* end = nullptr;
            const unsigned long long v = std::strtoull(sec.c_str(), &end, 10);
            if (end != sec.c_str() && end != nullptr && *end == '\0' && v <= UINT32_MAX) {
                s_agg.total_seconds = static_cast<uint32_t>(v);
            }
            const int fin = settings.GetInt(kKeyHsFin, 0);
            s_agg.finished_count = fin < 0 ? 0 : fin;
            s_agg.valid = true;
        }
    }
    s_hydrated = true;
    size_t recent_n = 0;
    for (size_t i = 0; i < kRecentMax; ++i) {
        if (!s_recent_rel[i].empty()) {
            ++recent_n;
        }
    }
    ESP_LOGI(TAG, "hydrated recent_n=%u agg_valid=%d sec=%u fin=%d",
             static_cast<unsigned>(recent_n), s_agg.valid ? 1 : 0,
             static_cast<unsigned>(s_agg.total_seconds), s_agg.finished_count);
}

}  // namespace

void HydrateFromNvs() {
    std::lock_guard<std::mutex> lock(s_mu);
    HydrateLocked();
}

void NoteOpenedBook(const char* book_abs_path) {
    std::string rel;
    if (!AbsToRel(book_abs_path, rel)) {
        ESP_LOGW(TAG, "note recent skip (bad path): %s", book_abs_path ? book_abs_path : "(null)");
        return;
    }

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(s_mu);
        // 未 hydrate 时也允许改内存（启动竞态）；不在此读 Flash
        if (!s_recent_rel[0].empty() && s_recent_rel[0] == rel) {
            return;
        }
        std::string next[kRecentMax];
        next[0] = rel;
        size_t w = 1;
        for (size_t i = 0; i < kRecentMax && w < kRecentMax; ++i) {
            if (s_recent_rel[i].empty() || s_recent_rel[i] == rel) {
                continue;
            }
            next[w++] = std::move(s_recent_rel[i]);
        }
        for (size_t i = 0; i < kRecentMax; ++i) {
            if (i < w) {
                s_recent_rel[i] = std::move(next[i]);
            } else {
                s_recent_rel[i].clear();
            }
        }
        s_recent_dirty = true;
        changed = true;
        ESP_LOGI(TAG, "recent MRU head=%s n=%u", s_recent_rel[0].c_str(), static_cast<unsigned>(w));
    }
    if (changed) {
        SchedulePersist(true, false);
    }
}

void RemoveBook(const char* book_abs_path) {
    std::string rel;
    if (!AbsToRel(book_abs_path, rel)) {
        return;
    }

    // 无论是否在 MRU：删书都推进代数，供 sync 检测 Peek 窗口内库变更
    s_mutation_epoch.fetch_add(1, std::memory_order_acq_rel);

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(s_mu);
        for (size_t i = 0; i < kRecentMax; ++i) {
            if (s_recent_rel[i] == rel) {
                s_recent_rel[i].clear();
                changed = true;
            }
        }
        if (changed) {
            CompactRecentLocked();
            s_recent_dirty = true;
            ESP_LOGI(TAG, "recent pruned %s", rel.c_str());
        }
    }
    if (changed) {
        SchedulePersist(true, false);
    }
}

void SubtractAggregateContribution(uint32_t reading_seconds, bool finished) {
    if (reading_seconds == 0 && !finished) {
        return;
    }
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(s_mu);
        if (!s_agg.valid) {
            return;
        }
        if (reading_seconds > 0) {
            if (s_agg.total_seconds >= reading_seconds) {
                s_agg.total_seconds -= reading_seconds;
            } else {
                s_agg.total_seconds = 0;
            }
            changed = true;
        }
        if (finished && s_agg.finished_count > 0) {
            --s_agg.finished_count;
            changed = true;
        }
        if (changed) {
            s_agg_dirty = true;
            ESP_LOGI(TAG, "aggregate subtract sec=%u finished=%d -> sec=%u fin=%d",
                     static_cast<unsigned>(reading_seconds), finished ? 1 : 0,
                     static_cast<unsigned>(s_agg.total_seconds), s_agg.finished_count);
        }
    }
    if (changed) {
        SchedulePersist(false, true);
    }
}

size_t LoadRecentAbsPaths(std::vector<std::string>& out) {
    out.clear();

    std::string rels[kRecentMax];
    {
        std::lock_guard<std::mutex> lock(s_mu);
        // UI 路径：禁止 NVS；未 hydrate 则空列表
        for (size_t i = 0; i < kRecentMax; ++i) {
            rels[i] = s_recent_rel[i];
        }
    }

    bool drop[kRecentMax] = {};
    bool any_drop = false;
    for (size_t i = 0; i < kRecentMax; ++i) {
        if (rels[i].empty()) {
            continue;
        }
        const std::string abs = RelToAbs(rels[i]);
        if (abs.empty() || !FileExists(abs.c_str())) {
            ESP_LOGW(TAG, "recent stale, drop %s", rels[i].c_str());
            drop[i] = true;
            any_drop = true;
            continue;
        }
        out.push_back(abs);
    }

    if (any_drop) {
        {
            std::lock_guard<std::mutex> lock(s_mu);
            for (size_t i = 0; i < kRecentMax; ++i) {
                if (drop[i] && !rels[i].empty()) {
                    for (size_t j = 0; j < kRecentMax; ++j) {
                        if (s_recent_rel[j] == rels[i]) {
                            s_recent_rel[j].clear();
                        }
                    }
                }
            }
            CompactRecentLocked();
            s_recent_dirty = true;
        }
        SchedulePersist(true, false);
    }
    return out.size();
}

AggregateStats LoadAggregateStats() {
    std::lock_guard<std::mutex> lock(s_mu);
    // UI：只返回内存，绝不打开 NVS
    return s_agg;
}

void StoreAggregateStats(uint32_t total_seconds, int finished_count) {
    if (finished_count < 0) {
        finished_count = 0;
    }
    {
        std::lock_guard<std::mutex> lock(s_mu);
        s_agg.valid = true;
        s_agg.total_seconds = total_seconds;
        s_agg.finished_count = finished_count;
        s_agg_dirty = true;  // 内存已是权威，避免之后 Hydrate 用旧 NVS 覆盖
        ESP_LOGI(TAG, "aggregate stored sec=%u finished=%d", static_cast<unsigned>(total_seconds),
                 finished_count);
    }
    // 开机 sync 在 DRAM 栈：可同步写；统一走异步亦安全
    SchedulePersist(false, true);
}

uint32_t MutationEpoch() {
    return s_mutation_epoch.load(std::memory_order_acquire);
}

}  // namespace book_home_snapshot
}  // namespace reader
