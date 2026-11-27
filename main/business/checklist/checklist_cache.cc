#include <atomic>
#include <cstring>
#include <mutex>
#include <new>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "checklist_cache.h"
#include "infra/diagnostics/firmware_memory.h"

static constexpr const char* TAG = "ChecklistCache";
static std::atomic<uint32_t> s_revision{0}; // 仅在缓存内容提交后发布
static std::mutex s_mu;
static checklist_cache_item_t* s_items = nullptr;  // SPIRAM
static size_t s_count = 0;
static bool s_ready = false;

static bool EnsureStorageLocked() {
    if (s_items != nullptr) {
        return true;
    }
    s_items = static_cast<checklist_cache_item_t*>(heap_caps_calloc(
        CHECKLIST_CACHE_MAX_ITEMS, sizeof(checklist_cache_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    const size_t storage_bytes = CHECKLIST_CACHE_MAX_ITEMS * sizeof(checklist_cache_item_t);
    if (s_items == nullptr && heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >
                                  storage_bytes + kFirmwareInternalFloorBytes) {
        // 仅在内部RAM安全余量允许时降级。
        s_items = static_cast<checklist_cache_item_t*>(
            heap_caps_calloc(CHECKLIST_CACHE_MAX_ITEMS, sizeof(checklist_cache_item_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    return s_items != nullptr;
}

bool checklist_cache_replace(const checklist_cache_item_t* items, size_t count) {
    if (items == nullptr && count != 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_mu);
    if (!EnsureStorageLocked()) {
        ESP_LOGE(TAG, "operation=replace stage=allocate result=failed");
        return false;
    }
    s_count = count < CHECKLIST_CACHE_MAX_ITEMS ? count : CHECKLIST_CACHE_MAX_ITEMS;
    if (s_count != 0) {
        std::memcpy(s_items, items, s_count * sizeof(*s_items));
    }
    s_ready = true;
    s_revision.fetch_add(1, std::memory_order_release);
    return true;
}

bool checklist_cache_ready(void) {
    std::lock_guard<std::mutex> lock(s_mu);
    return s_ready;
}

void checklist_cache_apply_action(const char* id, bool remove) {
    if (!id || !id[0]) {
        return;
    }
    std::lock_guard<std::mutex> lock(s_mu);
    for (size_t i = 0; i < s_count; ++i) {
        if (std::strcmp(s_items[i].id, id) != 0) {
            continue;
        }
        if (remove) {
            std::memmove(s_items + i, s_items + i + 1, (s_count - i - 1) * sizeof(*s_items));
            --s_count;
        } else {
            strlcpy(s_items[i].status, "done", sizeof(s_items[i].status));
        }
        s_revision.fetch_add(1, std::memory_order_release);
        break;
    }
}

size_t checklist_cache_copy_pending(checklist_cache_item_t* out, size_t max_out) {
    if (out == nullptr || max_out == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(s_mu);
    if (s_items == nullptr || !s_ready) {
        return 0;
    }
    size_t n = 0;
    for (size_t i = 0; i < s_count && n < max_out; ++i) {
        if (std::strcmp(s_items[i].status, "done") == 0) {
            continue;
        }
        out[n] = s_items[i];
        ++n;
    }
    return n;
}

bool checklist_cache_counts(const char* date, size_t* today, size_t* done, size_t* pending) {
    if (today == nullptr || done == nullptr || pending == nullptr) {
        return false;
    }
    *today = 0;
    *done = 0;
    *pending = 0;
    const bool has_date = date != nullptr && std::strlen(date) == 10;
    std::lock_guard<std::mutex> lock(s_mu);
    if (s_items == nullptr || !s_ready) {
        return false;
    }
    for (size_t i = 0; i < s_count; ++i) {
        if (has_date && std::strcmp(s_items[i].plan_date, date) == 0) {
            ++*today;
        }
        if (std::strcmp(s_items[i].status, "done") == 0) {
            ++*done;
        } else {
            ++*pending;
        }
    }
    return true;
}

uint32_t checklist_cache_revision(void) {
    return s_revision.load(std::memory_order_acquire);
}

bool ChecklistCopyItems(std::vector<checklist_cache_item_t>& out, uint32_t& revision) {
    std::lock_guard<std::mutex> lock(s_mu);
    try {
        if (s_count != 0) {
            out.assign(s_items, s_items + s_count);
        } else {
            out.clear();
        }
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "operation=copy_snapshot stage=allocate result=no_memory");
        return false;
    }
    revision = s_revision.load(std::memory_order_relaxed);
    return true;
}
