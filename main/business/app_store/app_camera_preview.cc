#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include "app_camera_preview.h"
#include "boards/common/camera/camera_capture.h"
#include "infra/tasks/task_create.h"
#include "sysmgmt/power/power_lifecycle.h"

struct AppCameraPreview {
    std::atomic<bool> closed{false}; // 后台不访问 ELF 引用
    std::atomic<bool> enabled{false}; // 应用的取景需求
    std::atomic<bool> foreground{false}; // 仅当前页面可继续采集
    std::atomic<bool> worker{false}; // 含硬件收尾阶段
    std::atomic<uint32_t> generation{0}; // 请求或前台状态变化时作废旧任务
    int width = 0, height = 0; // 任务启动前确定，运行期间不修改
    std::mutex mutex; // 仅保护状态快照和最新帧交接
    xz_app_camera_status_t status{sizeof(xz_app_camera_status_t)};
    std::shared_ptr<RasterImage> latest; // 最多保留一帧，旧帧直接替换
};

struct AppCameraPreviewWork {
    std::shared_ptr<AppCameraPreview> state; // 任务持有宿主状态，不借用 ELF
    uint32_t generation; // 启动时的请求代次
    bool cancelled = false; // 取消一旦被观察到，本任务不再恢复
};

static std::atomic<unsigned> s_preview_workers{0}; // 包含离页后的硬件收尾

static bool PreviewUnavailable(const AppCameraPreview& state) {
    return state.closed.load() || !state.enabled.load() || !state.foreground.load() ||
           PowerLifecycleIsStandby() || PowerLifecycleIsStandbyResuming() || PowerLifecycleIsPoweringOff();
}

static bool PreviewCancelled(void* context) {
    auto& work = *static_cast<AppCameraPreviewWork*>(context);
    if (work.cancelled || work.state->generation.load() != work.generation || PreviewUnavailable(*work.state)) {
        work.cancelled = true;
    }
    return work.cancelled;
}

static bool PreviewFrame(RasterImage&& image, void* context) {
    auto& work = *static_cast<AppCameraPreviewWork*>(context);
    auto& state = *work.state;
    if (PreviewCancelled(&work)) {
        return false;
    }
    auto frame = std::make_shared<RasterImage>(std::move(image));
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (PreviewCancelled(&work)) {
            return false;
        }
        state.latest = std::move(frame);
        ++state.status.frames;
    }
    // 控制取景请求频率，并在等待期间及时响应待机及退出；不持有显示 DMA 门闩。
    for (unsigned elapsed = 0; elapsed < 800; elapsed += 20) {
        if (PreviewCancelled(&work)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

static void PreviewTask(void* context) {
    std::unique_ptr<AppCameraPreviewWork> work(static_cast<AppCameraPreviewWork*>(context));
    auto state = work->state;
    int result = XZ_APP_OK;
    esp_err_t error = ESP_OK;
    try {
        CameraCaptureRequest request;
        request.max_width = state->width;
        request.max_height = state->height;
        request.cancelled = PreviewCancelled;
        request.context = work.get();
        if (!PreviewCancelled(work.get())) {
            error = CameraStreamPreview(request, PreviewFrame, work.get());
        }
        switch (error) {
            case ESP_OK:
                break;
            case ESP_ERR_NO_MEM:
                result = XZ_APP_ERR_MEMORY;
                break;
            case ESP_ERR_NOT_FOUND:
                result = XZ_APP_ERR_NOT_FOUND;
                break;
            case ESP_ERR_NOT_SUPPORTED:
                result = XZ_APP_ERR_UNSUPPORTED;
                break;
            case ESP_ERR_INVALID_ARG:
                result = XZ_APP_ERR_INVALID;
                break;
            case ESP_ERR_INVALID_STATE:
                result = XZ_APP_ERR_BUSY;
                break;
            default:
                result = XZ_APP_ERR_IO;
                break;
        }
    } catch (const std::bad_alloc&) {
        result = XZ_APP_ERR_MEMORY;
        error = ESP_ERR_NO_MEM;
    } catch (...) {
        result = XZ_APP_ERR_IO;
        error = ESP_FAIL;
    }
    if (PreviewCancelled(work.get())) {
        result = XZ_APP_OK;
    }
    bool failed = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->status.running = 0;
        if (state->generation.load() == work->generation && !state->closed.load()) {
            state->status.result = result;
            if (result != XZ_APP_OK) {
                state->enabled.store(false);
                failed = true;
            }
        }
        if (work->cancelled || failed) {
            state->latest.reset();
        }
    }
    if (failed) {
        ESP_LOGE("AppCamera", "operation=preview stage=capture result=%d code=%d", result, int(error));
    }
    ESP_LOGI("AppCamera", "operation=preview result=stopped code=%d", result);
    work.reset();
    state->worker.store(false);
    --s_preview_workers;
    state.reset();
    vTaskDeleteWithCaps(nullptr);
}

static int PreviewStart(const std::shared_ptr<AppCameraPreview>& state) {
    AppCameraPreviewWork* owner = nullptr;
    uint32_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (PreviewUnavailable(*state) || state->worker.exchange(true)) {
            return XZ_APP_OK;
        }
        generation = state->generation.load();
        owner = new (std::nothrow) AppCameraPreviewWork{state, generation};
        ++s_preview_workers;
        state->status.running = 1;
        state->status.result = XZ_APP_OK;
    }
    if (!owner || FirmwareCreateTask(PreviewTask, "app_camera", 16384, owner, 2, nullptr, tskNO_AFFINITY,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        delete owner;
        ESP_LOGE("AppCamera", "operation=preview stage=task result=no_memory");
        std::lock_guard<std::mutex> lock(state->mutex);
        state->status.running = 0;
        if (state->generation.load() == generation && !state->closed.load()) {
            state->enabled.store(false);
            state->status.result = XZ_APP_ERR_MEMORY;
        }
        --s_preview_workers;
        state->worker.store(false);
        return XZ_APP_ERR_MEMORY;
    }
    ESP_LOGI("AppCamera", "operation=preview result=started");
    return XZ_APP_OK;
}

std::shared_ptr<AppCameraPreview> AppCameraPreviewCreate() {
    return std::make_shared<AppCameraPreview>();
}

int AppCameraPreviewEnable(const std::shared_ptr<AppCameraPreview>& state, bool enabled, int width, int height) {
    if (!state || state->closed.load()) {
        return XZ_APP_ERR_NOT_READY;
    }
    if (enabled && (width < 1 || height < 1 || width > 1600 || height > 1600 ||
                    uint64_t(width) * height > 1024 * 1024)) {
        return XZ_APP_ERR_INVALID;
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed.load()) {
            return XZ_APP_ERR_NOT_READY;
        }
        const bool running = state->worker.load();
        if (enabled && running && (state->width != width || state->height != height)) {
            return XZ_APP_ERR_BUSY;
        }
        if (!running) {
            state->width = width;
            state->height = height;
        }
        if (state->enabled.exchange(enabled) != enabled) {
            ++state->generation;
            state->status.result = XZ_APP_OK;
            state->latest.reset();
        }
        if (!enabled) {
            state->latest.reset();
        }
    }
    if (!enabled) {
        return XZ_APP_OK;
    }
    return PreviewStart(state);
}

void AppCameraPreviewForeground(const std::shared_ptr<AppCameraPreview>& state, bool foreground) {
    if (state) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->foreground.exchange(foreground) != foreground) {
                ++state->generation;
                state->latest.reset();
            }
        }
        if (foreground && state->enabled.load()) {
            PreviewStart(state);
        }
    }
}

std::shared_ptr<RasterImage> AppCameraPreviewTake(const std::shared_ptr<AppCameraPreview>& state) {
    if (!state) {
        return {};
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    return std::exchange(state->latest, {});
}

int AppCameraPreviewStatus(const std::shared_ptr<AppCameraPreview>& state, xz_app_camera_status_t* result) {
    if (!state || !result || result->struct_size < sizeof(uint32_t)) {
        return XZ_APP_ERR_INVALID;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    std::memcpy(result, &state->status, std::min<size_t>(result->struct_size, sizeof(state->status)));
    return XZ_APP_OK;
}

void AppCameraPreviewClose(const std::shared_ptr<AppCameraPreview>& state) {
    if (state) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->closed.store(true);
        state->enabled.store(false);
        ++state->generation;
        state->latest.reset();
    }
}

bool AppCameraPreviewBusy() {
    return s_preview_workers.load() != 0;
}
