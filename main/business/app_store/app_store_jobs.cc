#include <mutex>
#include <new>
#include <utility>
#include <esp_log.h>
#include "app_store.h"
#include "assets/lang_app_store.h"
#include "assets/lang_config.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "sysmgmt/tasks/task_executor.h"

struct AppStoreWork {
    AppStoreOperation operation;             // 工作类型
    std::string server;                            // 服务器基址快照
    AppStoreCatalogPage pagination;                // 请求页快照
    AppStoreItem item;                             // 操作目标快照
    std::shared_ptr<AppStoreResult> result;  // 页面与工作任务共享结果
};

static std::mutex s_mutex;
static std::weak_ptr<AppStoreResult> s_request;

static void Worker(const std::shared_ptr<AppStoreWork>& work) {
    auto& result = *work->result;
    try {
        PowerNeedHold hold(work->operation == AppStoreOperation::Install ||
                                   work->operation == AppStoreOperation::Update
                               ? PowerNeed::OtaDownload
                               : PowerNeed::ForegroundNetwork);
        switch (work->operation) {
            case AppStoreOperation::Scan:
                result.scanned = AppStoreScan(result.installed, result.error);
                result.ok = result.scanned;
                break;
            case AppStoreOperation::Refresh:
            case AppStoreOperation::PageFetch:
                result.pagination = work->pagination;
                result.ok =
                    AppStoreFetch(work->server, result.catalog, result.error, result.cancelled, &result.pagination);
                result.fetched = result.ok;
                break;
            case AppStoreOperation::Install:
                result.ok = AppStoreInstall(work->server, work->item, result.error, result.cancelled, &result.progress);
                break;
            case AppStoreOperation::Detail:
                result.candidate = work->item;
                result.ok = AppStoreDetail(work->server, result.candidate, result.error, result.cancelled);
                break;
            case AppStoreOperation::CheckUpdate: {
                bool available = false;
                result.ok = AppStoreCheckUpdate(work->server, work->item, result.candidate, available, result.error,
                                                result.cancelled);
                result.latest = !available;
                break;
            }
            case AppStoreOperation::Update:
                result.ok = AppStoreInstall(work->server, work->item, result.error, result.cancelled, &result.progress);
                break;
            case AppStoreOperation::Remove:
                result.ok = AppStoreRemove(work->item, result.error);
                break;
        }
        if (result.cancelled.load() && result.progress.total.load() == 0) {
            result.ok = false;
            result.error = LangString_STORE_CANCELLED;
        }
        if (work->operation == AppStoreOperation::Install || work->operation == AppStoreOperation::Update ||
            work->operation == AppStoreOperation::Remove) {
            std::string scan_error;
            result.scanned = AppStoreScan(result.installed, scan_error);
            if (!result.scanned && result.error.empty()) {
                result.error = scan_error;
                result.ok = false;
            }
        }
    } catch (const std::bad_alloc&) {
        result.ok = false;
        result.error = LangString_STORE_MEMORY_ERROR;
        ESP_LOGE("AppStore", "operation=execute stage=allocate result=no_memory");
    }
    if (!result.ok && !result.cancelled.load()) {
        ESP_LOGW("AppStore", "operation=execute kind=%u result=failed", unsigned(work->operation));
    }
    result.done.store(true);
}

std::shared_ptr<AppStoreResult> AppStoreSubmit(AppStoreOperation operation, const std::string& server,
                                            const AppStoreItem& item, AppStoreCatalogPage pagination) try {
    std::lock_guard<std::mutex> lock(s_mutex);
    const auto active = s_request.lock();
    if (active && !active->done.load(std::memory_order_acquire)) {
        return {};
    }
    auto result = std::make_shared<AppStoreResult>();
    result->error.reserve(128);
    auto work = std::make_shared<AppStoreWork>(AppStoreWork{operation, server, std::move(pagination), item, result});
    if (!TaskExecutor::Schedule([work] { Worker(work); }, TaskDomain::NetworkIo)) {
        return {};
    }
    s_request = result;
    return result;
} catch (const std::bad_alloc&) {
    ESP_LOGE("AppStore", "operation=submit result=no_memory");
    return {};
}

bool AppStoreBusy() {
    std::lock_guard<std::mutex> lock(s_mutex);
    const auto result = s_request.lock();
    return result && !result->done.load(std::memory_order_acquire);
}
