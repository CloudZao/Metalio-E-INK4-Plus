#include <mutex>
#include <new>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "infra/tasks/task_create.h"
#include "boards/common/camera/camera_capture.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "camera_diagnostic.h"

static std::mutex s_mutex;
static bool s_running = false;
static constexpr unsigned kStackBytes = 10 * 1024; // USB主机与硬件采集保留独立内部栈
struct CameraDiagnosticWork {
    CameraDiagnosticRequest result;
    int max_width = 0, max_height = 0;
};

static void RunCamera(void* context) {
    CameraDiagnosticRequest result;
    {
        std::unique_ptr<CameraDiagnosticWork> work(static_cast<CameraDiagnosticWork*>(context));
        result = work->result;
        PowerNeedHold hold(PowerNeed::StandbyInhibit);
        CameraCaptureRequest request;
        request.max_width = work->max_width;
        request.max_height = work->max_height;
        request.timeout_ms = result->capture ? 20000u : 8000u;
        request.context = result.get();
        request.cancelled = [](void* value) { return static_cast<CameraDiagnosticResult*>(value)->cancelled.load(); };
        try {
            result->error = result->capture ? CameraCapturePreview(result->raster, request) : CameraDetect(request);
        } catch (const std::bad_alloc&) {
            result->error = ESP_ERR_NO_MEM;
        }
        if (!result->cancelled.load() && result->error != ESP_OK) {
            ESP_LOGW("CameraDiagnostic", "operation=%s error=%d", result->capture ? "capture" : "detect", int(result->error));
        }
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_running = false;
    }
    result->completed.store(true, std::memory_order_release);
    result.reset();
    vTaskDelete(nullptr);
}

CameraDiagnosticRequest CameraDiagnosticStart(bool capture, int max_width, int max_height) try {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_running || (capture && (max_width <= 0 || max_height <= 0))) {
        return {};
    }
    auto result = std::make_shared<CameraDiagnosticResult>();
    result->capture = capture;
    auto work = std::make_unique<CameraDiagnosticWork>();
    work->result = result;
    work->max_width = max_width;
    work->max_height = max_height;
    s_running = true;
    if (FirmwareCreateTask(RunCamera, "camera_diagnostic", kStackBytes, work.get(), 5, nullptr, tskNO_AFFINITY, 0) != pdPASS) {
        s_running = false;
        ESP_LOGW("CameraDiagnostic", "operation=submit stage=create_task result=unavailable");
        return {};
    }
    work.release();
    return result;
} catch (const std::bad_alloc&) {
    ESP_LOGE("CameraDiagnostic", "operation=submit stage=allocate result=no_memory");
    return {};
}

void CameraDiagnosticCancel(const CameraDiagnosticRequest& request) {
    if (request) {
        request->cancelled.store(true);
    }
}
