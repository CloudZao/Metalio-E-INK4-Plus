#include "application.h"

#include <array>
#include <deque>
#include <new>
#include <esp_log.h>

namespace {
constexpr const char* TAG = "AppTasks";
using Domain = Application::TaskDomain;

struct Worker {
    const char* name; // FreeRTOS 任务名称
    uint32_t stack_bytes; // 内部 RAM 栈大小，涵盖 NVS 和驱动调用
    size_t capacity; // 等待队列上限，不包含正在执行的任务
    std::mutex mutex; // 仅保护任务句柄与队列，不覆盖业务执行
    std::deque<std::function<void()>> jobs; // 拥有回调及其捕获数据
    TaskHandle_t task = nullptr; // 空闲退出前在锁内清空
};

std::array<Worker, 5>& Workers() {
    static std::array<Worker, 5> workers{{
        {"app_voice", 8192, 32},
        {"app_network", 8192, 4},
        {"app_storage", 4096, 16},
        {"app_audio", 8192, 16},
        {"app_cleanup", 4096, 24},
    }};
    return workers;
}

bool TakeJob(Worker& worker, std::function<void()>& job) {
    std::lock_guard<std::mutex> lock(worker.mutex);
    if (worker.jobs.empty()) return false;
    job = std::move(worker.jobs.front());
    worker.jobs.pop_front();
    return true;
}

void RunWorker(void* context) {
    auto& worker = *static_cast<Worker*>(context);
    for (;;) {
        {
            std::function<void()> job;
            if (TakeJob(worker, job)) {
                job();
                continue;
            }
        }
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) != 0) continue;
        const auto& workers = Workers();
        auto& app = Application::GetInstance();
        // 硬件会话仍在使用时保留控制任务，收尾不再依赖重新申请任务栈。
        if ((&worker == &workers[static_cast<size_t>(Domain::Voice)] && app.IsXiaozhiVoiceActive()) ||
            (&worker == &workers[static_cast<size_t>(Domain::Audio)] && app.GetAudioService().IsStarted())) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(worker.mutex);
            if (!worker.jobs.empty()) continue;
            // 队列与句柄一起交还，后续投递只能唤醒有效任务或创建新任务。
            worker.task = nullptr;
        }
        break;
    }
    vTaskDelete(nullptr);
}
}

bool Application::Schedule(std::function<void()> callback, TaskDomain domain) try {
    const size_t index = static_cast<size_t>(domain);
    if (!callback || index >= Workers().size()) return false;
    auto& worker = Workers()[index];
    bool full = false;
    {
        std::lock_guard<std::mutex> lock(worker.mutex);
        full = worker.jobs.size() >= worker.capacity;
        if (!full) {
            worker.jobs.emplace_back();
            worker.jobs.back().swap(callback);
            if (worker.task == nullptr &&
                xTaskCreate(RunWorker, worker.name, worker.stack_bytes, &worker, 3, &worker.task) != pdPASS) {
                // 捕获对象可能归还电源或音频占用，移出后在队列锁外析构。
                worker.jobs.back().swap(callback);
                worker.jobs.pop_back();
                worker.task = nullptr;
            } else {
                xTaskNotifyGive(worker.task);
                return true;
            }
        }
    }
    // 日志后端也可能持锁或访问存储，不在消息队列锁内调用。
    ESP_LOGW(TAG, "%s %s", worker.name, full ? "queue full" : "task allocation failed");
    return false;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "task message allocation failed");
    return false;
}

bool Application::IsVoiceTask() const {
    auto& worker = Workers()[static_cast<size_t>(Domain::Voice)];
    std::lock_guard<std::mutex> lock(worker.mutex);
    return worker.task != nullptr && worker.task == xTaskGetCurrentTaskHandle();
}
