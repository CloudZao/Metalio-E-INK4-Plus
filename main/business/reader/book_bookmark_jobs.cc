#include <new>
#include <mutex>
#include <esp_log.h>
#include "assets/lang_screen.h"
#include "assets/lang_config.h"
#include "boards/common/storage/sd_access.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "sysmgmt/tasks/task_executor.h"
#include "book_bookmark_jobs.h"

static std::mutex s_mutex;
static std::weak_ptr<BookBookmarkResult> s_request;

BookBookmarkRequest BookBookmarkSubmit(const BookBookmarkStore& store, BookBookmark entry,
                                       std::vector<uint32_t> remove,
                                       std::function<void()> completed) try {
    std::lock_guard<std::mutex> lock(s_mutex);
    const auto previous = s_request.lock();
    if (previous && !previous->completed.load(std::memory_order_acquire)) {
        return {};
    }
    auto result = std::make_shared<BookBookmarkResult>();
    result->store = store;
    result->error.reserve(128);
    auto access = std::make_shared<SdAccessDeferredTransaction>();
    if (!*access || !TaskExecutor::Schedule([result, entry = std::move(entry), remove = std::move(remove),
                                           completed = std::move(completed), access]() mutable {
        {
            PowerNeedHold hold(PowerNeed::StandbyInhibit);
            try {
                SdAccessTransaction transaction(*access);
                if (transaction) {
                    result->ok = remove.empty() ? result->store.Add(std::move(entry), result->error)
                                                : result->store.Remove(remove, result->error);
                } else {
                    result->error = LangString_BOOK_NO_SD;
                }
                if (!result->ok) {
                    ESP_LOGE("BookBookmarks", "operation=save result=failed");
                }
            } catch (const std::bad_alloc&) {
                result->error = LangString_COMMON_MEMORY_RETRY;
                ESP_LOGE("BookBookmarks", "operation=save result=no_memory");
            }
            access.reset();
        }
        result->completed.store(true, std::memory_order_release);
        if (completed) {
            completed();
        }
    }, TaskDomain::Storage)) {
        return {};
    }
    s_request = result;
    return result;
} catch (const std::bad_alloc&) {
    ESP_LOGE("BookBookmarks", "operation=submit result=no_memory");
    return {};
}
