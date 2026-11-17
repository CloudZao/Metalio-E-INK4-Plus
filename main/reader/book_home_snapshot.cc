#include "book_home_snapshot.h"

#include "book_library.h"
#include "settings.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <nvs.h>
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
/** 统计已增量更新时，Hydrate 不得用旧 NVS 覆盖内存摘要 */
bool s_agg_dirty = false;
std::string s_recent_rel[kRecentMax];
AggregateStats s_agg;
struct DayRecord { uint32_t date = 0, seconds = 0; };
std::array<DayRecord, 14> s_days{};
uint32_t s_undated_seconds = 0;
std::atomic<bool> s_persist_days_pending{false};

void AddDayLocked(uint32_t date, uint32_t seconds) {
    if (date < 20250101 || date > 20991231 || seconds == 0) return;
    auto slot = std::find_if(s_days.begin(), s_days.end(), [date](const DayRecord& r) { return r.date == date; });
    if (slot == s_days.end()) {
        slot = std::min_element(s_days.begin(), s_days.end(), [](const DayRecord& a, const DayRecord& b) { return a.date < b.date; });
        if (slot->date > date) return;
        *slot = {date, 0};
    }
    slot->seconds = UINT32_MAX - slot->seconds < seconds ? UINT32_MAX : slot->seconds + seconds;
}

std::atomic<bool> s_persist_recent_pending{false};
std::atomic<bool> s_persist_agg_pending{false};
std::atomic<bool> s_persist_task_running{false};
/** 删书等变更代数（仅 RemoveBook 递增） */

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

bool PersistDaysNow() {
    std::string value = "1;";
    {
        std::lock_guard<std::mutex> lock(s_mu);
        value += "0:" + std::to_string(s_undated_seconds) + ";";
        for (const auto& day : s_days) {
            if (!day.date) continue;
            char entry[40];
            std::snprintf(entry, sizeof(entry), "%u:%u;", static_cast<unsigned>(day.date), static_cast<unsigned>(day.seconds));
            value += entry;
        }
    }
    nvs_handle_t handle;
    esp_err_t error = nvs_open(kNvsNs, NVS_READWRITE, &handle);
    if (error != ESP_OK) return false;
    error = nvs_set_str(handle, "hs_days", value.c_str());
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error == ESP_OK;
}

void PersistTask(void* /*arg*/) {
    HydrateFromNvs(); // merge persisted days before any pending early-boot deltas are written
    // 单任务排空 pending：写完再看一眼，避免「退出窗口」丢尾写；禁止在持 s_mu 时开 NVS
    for (;;) {
        const bool do_recent = s_persist_recent_pending.exchange(false, std::memory_order_acq_rel);
        const bool do_days = s_persist_days_pending.exchange(false, std::memory_order_acq_rel);
        const bool do_agg = s_persist_agg_pending.exchange(false, std::memory_order_acq_rel);
        if (do_recent) {
            PersistRecentNow();
            ESP_LOGI(TAG, "nvs recent flushed");
        }
        if (do_agg) {
            PersistAggNow();
            ESP_LOGI(TAG, "nvs aggregate flushed");
        }
        if (do_days && !PersistDaysNow()) {
            // Keep the request for the next existing checkpoint/save; do not spin on Flash failure.
            s_persist_days_pending.store(true, std::memory_order_release);
            s_persist_task_running.store(false, std::memory_order_release);
            ESP_LOGW(TAG, "daily history save failed; pending kept for next save");
            vTaskDeleteWithCaps(nullptr);
            return;
        }
        if (!do_recent && !do_agg && !do_days) {
            // 先清 running，再二次确认 pending（与 SchedulePersist 的 CAS 对齐）
            s_persist_task_running.store(false, std::memory_order_release);
            if (s_persist_recent_pending.load(std::memory_order_acquire) ||
                s_persist_agg_pending.load(std::memory_order_acquire) ||
                s_persist_days_pending.load(std::memory_order_acquire)) {
                bool expected = false;
                if (s_persist_task_running.compare_exchange_strong(expected, true,
                                                                   std::memory_order_acq_rel)) {
                    continue;  // 本任务续跑，不必再 create
                }
            }
            break;
        }
    }
    vTaskDeleteWithCaps(nullptr);
}

void SchedulePersist(bool recent, bool agg, bool days = false) {
    if (days) s_persist_days_pending.store(true, std::memory_order_release);
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
    const std::string days = settings.GetString("hs_days", "");
    if (days.compare(0, 2, "1;") == 0) {
        size_t offset = 2;
        for (size_t n = 0; n < s_days.size() + 1 && offset < days.size(); ++n) {
            unsigned date = 0, seconds = 0; int used = 0;
            if (std::sscanf(days.c_str() + offset, "%u:%u;%n", &date, &seconds, &used) != 2 || used <= 0) break;
            if (date == 0) {
                s_undated_seconds = UINT32_MAX - s_undated_seconds < seconds ? UINT32_MAX : s_undated_seconds + seconds;
            } else {
                AddDayLocked(date, seconds); // pre-hydration records are increments, not replacements
            }
            offset += static_cast<size_t>(used);
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
        ESP_LOGW(TAG, "recent record rejected: invalid book path");
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
        ESP_LOGI(TAG, "recent records updated count=%u", static_cast<unsigned>(w));
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
            ESP_LOGI(TAG, "recent record removed");
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

std::string LoadLatestAbsPath() {
    std::lock_guard<std::mutex> lock(s_mu);
    for (const auto& rel : s_recent_rel) {
        if (!rel.empty()) return RelToAbs(rel);
    }
    return {};
}

size_t LoadRecentAbsPaths(std::vector<std::string>& out) {
    out.clear();
    std::lock_guard<std::mutex> lock(s_mu);
    for (const auto& relative : s_recent_rel) {
        if (relative.empty()) continue;
        const std::string absolute = RelToAbs(relative);
        if (!absolute.empty()) out.push_back(absolute);
    }
    return out.size();
}

AggregateStats LoadAggregateStats() {
    std::lock_guard<std::mutex> lock(s_mu);
    // UI：只返回内存，绝不打开 NVS
    return s_agg;
}

void UpdateAggregateProgress(uint32_t old_seconds, bool old_finished, uint32_t seconds, bool finished) {
    if (old_seconds == seconds && old_finished == finished) return;
    {
        std::lock_guard<std::mutex> lock(s_mu);
        const int64_t total = int64_t(s_agg.total_seconds) + int64_t(seconds) - old_seconds;
        s_agg.total_seconds = static_cast<uint32_t>(std::max<int64_t>(0, std::min<int64_t>(UINT32_MAX, total)));
        s_agg.finished_count = std::max(0, s_agg.finished_count + int(finished) - int(old_finished));
        s_agg.valid = true;
        s_agg_dirty = true;
    }
    SchedulePersist(false, true);
}

void AddDailyReadingSeconds(uint32_t date, uint32_t seconds) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(s_mu);
        if (date == 0) {
            s_undated_seconds = UINT32_MAX - s_undated_seconds < seconds ? UINT32_MAX : s_undated_seconds + seconds;
            changed = seconds != 0;
        } else if (date >= 20250101 && date <= 20991231) {
            // Date recovery assigns the saved undated interval once, never losing offline time.
            AddDayLocked(date, s_undated_seconds);
            changed = s_undated_seconds != 0 || seconds != 0;
            s_undated_seconds = 0;
            AddDayLocked(date, seconds);
        }
    }
    if (changed) SchedulePersist(false, false, true);
}

WeekStats LoadWeekStats() {
    WeekStats result;
    const time_t now = time(nullptr);
    tm local{};
    if (now == static_cast<time_t>(-1) || !localtime_r(&now, &local) ||
        local.tm_year < 125 || local.tm_year > 199) {
        std::lock_guard<std::mutex> lock(s_mu);
        result.undated_seconds = s_undated_seconds;
        return result;
    }
    const uint32_t today = (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
    AddDailyReadingSeconds(today, 0);
    local.tm_hour = 12; local.tm_min = 0; local.tm_sec = 0;
    local.tm_mday -= (local.tm_wday + 6) % 7;
    for (int i = 0; i < 7; ++i) {
        local.tm_isdst = -1;
        if (mktime(&local) == static_cast<time_t>(-1)) return {};
        result.dates[i] = (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
        ++local.tm_mday;
    }
    std::lock_guard<std::mutex> lock(s_mu);
    result.valid = s_hydrated;
    for (int i = 0; i < 7; ++i) {
        for (const auto& day : s_days) {
            if (day.date == result.dates[i]) result.seconds[i] = day.seconds;
        }
        result.total_seconds += result.seconds[i];
    }
    return result;
}

}  // namespace book_home_snapshot
}  // namespace reader
