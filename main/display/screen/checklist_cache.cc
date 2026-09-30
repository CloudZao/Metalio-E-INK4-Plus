#include "checklist_cache.h"

#include <cstring>
#include <mutex>

#include <esp_heap_caps.h>

namespace {

std::mutex s_mu;
checklist_cache_item_t* s_items = nullptr; // SPIRAM
size_t s_count = 0;
bool s_ready = false;
bool s_writing = false;

bool EnsureStorageLocked()
{
    if (s_items != nullptr) {
        return true;
    }
    s_items = static_cast<checklist_cache_item_t*>(heap_caps_calloc(
        CHECKLIST_CACHE_MAX_ITEMS, sizeof(checklist_cache_item_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_items == nullptr) {
        // SPIRAM 失败再退内部，避免功能全挂
        s_items = static_cast<checklist_cache_item_t*>(heap_caps_calloc(
            CHECKLIST_CACHE_MAX_ITEMS, sizeof(checklist_cache_item_t), MALLOC_CAP_8BIT));
    }
    return s_items != nullptr;
}

}  // namespace

void checklist_cache_begin(void)
{
    std::lock_guard<std::mutex> lock(s_mu);
    if (!EnsureStorageLocked()) {
        s_writing = false;
        return;
    }
    s_count = 0;
    s_ready = false;
    s_writing = true;
}

bool checklist_cache_append(const char* id, const char* title, const char* status,
                            const char* plan_date, const char* plan_time)
{
    std::lock_guard<std::mutex> lock(s_mu);
    if (!s_writing || s_items == nullptr || s_count >= CHECKLIST_CACHE_MAX_ITEMS) {
        return false;
    }
    checklist_cache_item_t& dst = s_items[s_count];
    strlcpy(dst.id, id != nullptr ? id : "", sizeof(dst.id));
    strlcpy(dst.title, title != nullptr ? title : "", sizeof(dst.title));
    strlcpy(dst.status, status != nullptr ? status : "pending", sizeof(dst.status));
    strlcpy(dst.plan_date, plan_date != nullptr ? plan_date : "", sizeof(dst.plan_date));
    strlcpy(dst.plan_time, plan_time != nullptr ? plan_time : "", sizeof(dst.plan_time));
    ++s_count;
    return true;
}

void checklist_cache_end(void)
{
    std::lock_guard<std::mutex> lock(s_mu);
    s_writing = false;
    if (s_items != nullptr) {
        s_ready = true;
    }
}

bool checklist_cache_ready(void)
{
    std::lock_guard<std::mutex> lock(s_mu);
    return s_ready;
}

size_t checklist_cache_copy_all(checklist_cache_item_t* out, size_t max_out)
{
    if (out == nullptr || max_out == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(s_mu);
    if (s_items == nullptr || !s_ready) {
        return 0;
    }
    size_t n = 0;
    for (size_t i = 0; i < s_count && n < max_out; ++i) {
        out[n] = s_items[i];
        ++n;
    }
    return n;
}

size_t checklist_cache_copy_pending(checklist_cache_item_t* out, size_t max_out)
{
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
