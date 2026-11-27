#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <new>
#include <mutex>
#include <utility>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/stat.h>
#include "cloud_resource_service.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "boards/common/board/board.h"
#include "boards/common/storage/sd_access.h"
#include "infra/storage/sd_paths.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "sysmgmt/tasks/task_executor.h"

static constexpr const char* TAG = "CloudResources";
static std::mutex s_mutex;
static std::weak_ptr<CloudResourceRequest> s_request; // 执行闭包持有工作，等待冲突时允许接收者放弃

static void FinishRequest(CloudResourceRequest& request) {
    request.stage.store(CloudResourceStage::Finished, std::memory_order_release);
    request.completed.store(true, std::memory_order_release);
}

static void DownloadProgress(int percent, void* context) {
    auto* request = static_cast<CloudResourceRequest*>(context);
    request->percent.store(std::clamp(percent, 0, 100), std::memory_order_release);
}

static bool ResolveDestination(const CloudPushResource& item, CloudSaveChoice choice,
                               std::string& path, bool& conflict) {
    conflict = false;
    path = item.LocalPath();
    struct stat info{};
    const int found = stat(path.c_str(), &info);
    if (found == 0 && choice == CloudSaveChoice::kAsk) {
        conflict = true;
        return false;
    }
    if (found != 0 && errno != ENOENT) {
        return false;
    }
    if (found == 0 && choice == CloudSaveChoice::kReplace && !S_ISREG(info.st_mode)) {
        return false;
    }
    if (found != 0 || choice != CloudSaveChoice::kKeepBoth) {
        return true;
    }
    const size_t slash = path.find_last_of('/');
    const size_t dot = path.find_last_of('.');
    const size_t split = dot != std::string::npos && dot > slash + 1 ? dot : path.size();
    const std::string stem = path.substr(0, split), extension = path.substr(split);
    for (unsigned n = 1; n <= 9999; ++n) {
        path = stem + "-" + std::to_string(n) + extension;
        if (path.size() - slash - 1 > SD_FILE_NAME_MAX_BYTES) {
            return false;
        }
        if (stat(path.c_str(), &info) != 0) {
            return errno == ENOENT;
        }
    }
    return false;
}

static bool RunDownloads(CloudResourceRequest& request, CloudSaveChoice choice) {
    for (size_t i = request.current.load(); i < request.inputs.size(); ++i) {
        if (request.gate.IsCancelled()) {
            return false;
        }
        request.current.store(i, std::memory_order_release);
        request.percent.store(0, std::memory_order_release);
        if (choice == CloudSaveChoice::kSkip) {
            request.outcomes[i] = CloudResourceOutcome::Skipped;
            request.skipped.fetch_add(1, std::memory_order_release);
            choice = CloudSaveChoice::kAsk;
            request.current.store(i + 1, std::memory_order_release);
            continue;
        }
        SdAccessTransaction transaction;
        std::string destination;
        bool conflict = false;
        const auto& input = request.inputs[i];
        if (!transaction || !ResolveDestination(input.resource, choice, destination, conflict)) {
            if (conflict) {
                return true;
            }
            request.failed = 1;
            request.outcomes[i] = CloudResourceOutcome::Failed;
            std::snprintf(request.error, sizeof(request.error), "%s", LangString_CLOUD_SAVE_FAIL);
            ESP_LOGE(TAG, "operation=download stage=destination result=failed errno=%d", errno);
            return false;
        }
        std::string error;
        const bool saved = DownloadPushResource(input.resource, error, DownloadProgress, &request, &request.gate,
                                                input.cover ? input.cover->path.c_str() : nullptr,
                                                destination.c_str(), choice == CloudSaveChoice::kReplace);
        if (!saved) {
            if (!request.gate.IsCancelled() && error != LangString_CLOUD_CANCELLED) {
                request.failed = 1;
                request.outcomes[i] = CloudResourceOutcome::Failed;
                std::snprintf(request.error, sizeof(request.error), "%s",
                              error.empty() ? LangString_CLOUD_DOWNLOAD_FAIL : error.c_str());
                ESP_LOGE(TAG, "operation=download stage=save result=failed");
            }
            return false;
        }
        // 文件已提交，随后取消仍需尝试交付该文件的回执。
        request.outcomes[i] = CloudResourceOutcome::Saved;
        request.succeeded.fetch_add(1, std::memory_order_release);
        std::string error_ack;
        if (AckPushResourceDownloaded(input.resource, error_ack)) {
            request.outcomes[i] = CloudResourceOutcome::Acknowledged;
        } else {
            ESP_LOGW(TAG, "operation=download stage=ack result=failed action=keep_file");
        }
        request.current.store(i + 1, std::memory_order_release);
        choice = CloudSaveChoice::kAsk;
    }
    return false;
}

static void RunRequest(const CloudResourceRequestPtr& request, CloudSaveChoice choice) {
    bool conflict = false;
    {
        PowerNeedHold network(PowerNeed::OtaDownload);
        request->stage.store(CloudResourceStage::Running, std::memory_order_release);
        try {
            if (!request->gate.IsCancelled()) {
                if (request->operation == CloudResourceOperation::Fetch) {
                    std::string error;
                    if (!FetchPushResources(request->items, error, &request->gate) && !request->gate.IsCancelled()) {
                        request->failed = 1;
                        std::snprintf(request->error, sizeof(request->error), "%s",
                                      error.empty() ? LangString_CLOUD_SYNC_FAIL : error.c_str());
                        ESP_LOGW(TAG, "operation=fetch result=failed");
                    }
                } else if (request->operation == CloudResourceOperation::Download) {
                    conflict = RunDownloads(*request, choice);
                } else {
                    for (size_t i = 0; i < request->inputs.size(); ++i) {
                        if (request->gate.IsCancelled()) {
                            break;
                        }
                        std::string error;
                        bool ok = DeletePushResourceRemote(request->inputs[i].resource, error, &request->gate);
                        if (!ok && !request->gate.IsCancelled()) {
                            vTaskDelay(pdMS_TO_TICKS(400));
                            ok = DeletePushResourceRemote(request->inputs[i].resource, error, &request->gate);
                        }
                        if (ok) {
                            request->outcomes[i] = CloudResourceOutcome::Deleted;
                            request->succeeded.fetch_add(1, std::memory_order_release);
                        } else if (request->gate.IsCancelled()) {
                            break;
                        } else {
                            request->outcomes[i] = CloudResourceOutcome::Failed;
                            ++request->failed;
                            if (request->error[0] == '\0') {
                                std::snprintf(request->error, sizeof(request->error), "%s",
                                              error.empty() ? LangString_CLOUD_DELETE_FAIL : error.c_str());
                            }
                            ESP_LOGW(TAG, "operation=delete result=failed index=%u", unsigned(i));
                        }
                    }
                }
            }
        } catch (const std::bad_alloc&) {
            ++request->failed;
            std::snprintf(request->error, sizeof(request->error), "%s", LangString_COMMON_MEMORY_RETRY);
            ESP_LOGE(TAG, "operation=run kind=%u stage=allocate result=no_memory", unsigned(request->operation));
        }
    }
    if (conflict) {
        request->stage.store(CloudResourceStage::Conflict, std::memory_order_release);
        if (!request->gate.IsCancelled()) {
            return;
        }
    }
    FinishRequest(*request);
}

static bool ScheduleRequest(const CloudResourceRequestPtr& request, CloudSaveChoice choice) {
    const auto domain = request->operation == CloudResourceOperation::Download ? TaskDomain::Transfer : TaskDomain::NetworkIo;
    return TaskExecutor::Schedule([request, choice] { RunRequest(request, choice); }, domain);
}

static CloudResourceRequestPtr SubmitRequest(CloudResourceRequestPtr request, CloudSaveChoice choice) {
    std::lock_guard<std::mutex> lock(s_mutex);
    const auto previous = s_request.lock();
    if (previous && !previous->completed.load(std::memory_order_acquire)) {
        return {};
    }
    if (!ScheduleRequest(request, choice)) {
        return {};
    }
    s_request = request;
    return request;
}

CloudResourceRequestPtr CloudFetchResources() try {
    return SubmitRequest(std::make_shared<CloudResourceRequest>(), CloudSaveChoice::kAsk);
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=fetch stage=submit result=no_memory");
    return {};
}

CloudResourceRequestPtr CloudDeleteResources(std::vector<CloudPushResource> items) try {
    if (items.empty()) {
        return {};
    }
    auto request = std::make_shared<CloudResourceRequest>();
    request->operation = CloudResourceOperation::Delete;
    request->inputs.reserve(items.size());
    for (auto& item : items) {
        request->inputs.push_back({std::move(item), {}});
    }
    request->outcomes.resize(request->inputs.size(), CloudResourceOutcome::Pending);
    return SubmitRequest(std::move(request), CloudSaveChoice::kAsk);
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=delete stage=submit result=no_memory");
    return {};
}

CloudResourceRequestPtr CloudDownloadResources(std::vector<CloudDownloadItem> items, CloudSaveChoice choice) try {
    if (items.empty() || choice > CloudSaveChoice::kSkip) {
        return {};
    }
    auto request = std::make_shared<CloudResourceRequest>();
    request->operation = CloudResourceOperation::Download;
    request->inputs = std::move(items);
    request->outcomes.resize(request->inputs.size(), CloudResourceOutcome::Pending);
    return SubmitRequest(std::move(request), choice);
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=download stage=submit result=no_memory");
    return {};
}

bool CloudResolveSaveConflict(const CloudResourceRequestPtr& request, CloudSaveChoice choice) try {
    if (!request || request->operation != CloudResourceOperation::Download || choice == CloudSaveChoice::kAsk ||
        choice > CloudSaveChoice::kSkip || request->gate.IsCancelled()) {
        return false;
    }
    auto expected = CloudResourceStage::Conflict;
    if (!request->stage.compare_exchange_strong(expected, CloudResourceStage::Queued, std::memory_order_acq_rel)) {
        return false;
    }
    if (ScheduleRequest(request, choice)) {
        return true;
    }
    request->stage.store(CloudResourceStage::Conflict, std::memory_order_release);
    if (request->gate.IsCancelled()) {
        FinishRequest(*request);
    }
    return false;
} catch (const std::bad_alloc&) {
    request->stage.store(CloudResourceStage::Conflict, std::memory_order_release);
    if (request->gate.IsCancelled()) {
        FinishRequest(*request);
    }
    ESP_LOGE(TAG, "operation=resolve_conflict stage=submit result=no_memory");
    return false;
}

void CloudCancelResourceRequest(const CloudResourceRequestPtr& request) try {
    if (!request || request->completed.load(std::memory_order_acquire)) {
        return;
    }
    request->gate.RequestCancel();
    auto expected = CloudResourceStage::Conflict;
    if (request->stage.compare_exchange_strong(expected, CloudResourceStage::Finished, std::memory_order_acq_rel)) {
        request->completed.store(true, std::memory_order_release);
        return;
    }
    if (!request->completed.load(std::memory_order_acquire) &&
        !TaskExecutor::Schedule([request] { request->gate.AbortBoundHttp(); }, TaskDomain::Cleanup)) {
        ESP_LOGW(TAG, "operation=cancel stage=abort_submit result=rejected action=retain_cancel_until_timeout");
    }
} catch (const std::bad_alloc&) {
    ESP_LOGW(TAG, "operation=cancel stage=allocate result=no_memory action=retain_cancel_until_timeout");
}

bool CloudResourcesBusy() {
    std::lock_guard<std::mutex> lock(s_mutex);
    const auto request = s_request.lock();
    return request && !request->completed.load(std::memory_order_acquire);
}

static bool CloudImageCancelled(void* context) {
    return static_cast<CloudImageRequest*>(context)->gate.IsCancelled();
}

static void QuantizeCloudThumbnail(RasterImage& image) {
    static constexpr uint8_t bayer[] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            auto& pixel = image.pixels[static_cast<size_t>(y) * image.width + x];
            const unsigned threshold = unsigned(bayer[(y & 3) * 4 + (x & 3)]) * 16u + 8u;
            pixel = pixel >= threshold ? 255 : 0;
        }
    }
}

CloudImageRequestPtr CloudLoadImages(std::vector<CloudImageItem> items) try {
    if (items.empty()) {
        return {};
    }
    bool network = false;
    for (const auto& item : items) {
        if (item.width <= 0 || item.height <= 0) {
            return {};
        }
        network |= !item.cache && item.path.empty() && !item.url.empty();
    }
    auto request = std::make_shared<CloudImageRequest>();
    request->items = std::move(items);
    if (!TaskExecutor::Schedule([request] {
        {
            PowerNeedHold hold(PowerNeed::OtaDownload);
            for (auto& item : request->items) {
                if (request->gate.IsCancelled()) {
                    break;
                }
                try {
                    if (!item.cache && item.path.empty() && !item.url.empty()) {
                        std::string error;
                        if (Board::GetInstance().EnsureNetworkReady() && !request->gate.IsCancelled()) {
                            item.cache = DownloadHttpToCache(item.url.c_str(), error, &request->gate,
                                                             CloudImageCancelled, request.get(), item.cache_app);
                        }
                    }
                    const char* path = item.cache ? item.cache->path.c_str() : item.path.c_str();
                    item.image = std::make_unique<RasterImage>();
                    if (path[0] && DecodeImageFileToL8(path, item.width, item.height, *item.image,
                                                     CloudImageCancelled, request.get(), &item.error) &&
                        !item.image->empty()) {
                        if (item.binary) {
                            QuantizeCloudThumbnail(*item.image);
                        }
                    } else {
                        item.image->Reset();
                        if (!request->gate.IsCancelled()) {
                            ESP_LOGW(TAG, "operation=image stage=load result=failed error=%u", unsigned(item.error));
                        }
                    }
                } catch (const std::bad_alloc&) {
                    item.image.reset();
                    item.error = ImageDecodeError::kNoMemory;
                    ESP_LOGE(TAG, "operation=image stage=allocate result=no_memory");
                    break;
                }
            }
        }
        request->completed.store(true, std::memory_order_release);
    }, network ? TaskDomain::NetworkIo : TaskDomain::Image)) {
        return {};
    }
    return request;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=image stage=submit result=no_memory");
    return {};
}

void CloudCancelImages(const CloudImageRequestPtr& request) try {
    if (!request || request->completed.load(std::memory_order_acquire)) {
        return;
    }
    request->gate.RequestCancel();
    if (!TaskExecutor::Schedule([request] { request->gate.AbortBoundHttp(); }, TaskDomain::Cleanup)) {
        ESP_LOGW(TAG, "operation=cancel_image stage=abort_submit result=rejected action=retain_cancel_until_timeout");
    }
} catch (const std::bad_alloc&) {
    ESP_LOGW(TAG, "operation=cancel_image stage=allocate result=no_memory action=retain_cancel_until_timeout");
}
