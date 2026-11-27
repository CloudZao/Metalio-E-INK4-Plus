#include <array>
#include <deque>
#include <mutex>
#include <new>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include "task_executor.h"
#include "infra/tasks/task_create.h"

static constexpr const char* TAG = "TaskExecutor";

struct TaskWorker {
    const char* name;                        // FreeRTOS 任务名称
    uint32_t stack_bytes;                    // 任务栈字节数，位置由stack_caps指定
    size_t capacity;                         // 等待队列上限，不包含正在执行的任务
    uint32_t stack_caps = 0;                 // 非零时由WithCaps分配及回收任务栈
    UBaseType_t priority = 3;                // 控制作业沿用原优先级
    std::mutex mutex;                        // 仅保护任务句柄与队列，不覆盖业务执行
    std::deque<std::function<void()>> jobs;  // 拥有回调及其捕获数据
    TaskHandle_t task = nullptr;             // 空闲退出前在锁内清空
    bool retained = false;                   // 资源所有者要求保留控制线程
};

static std::array<TaskWorker, 8>& Workers() {
    static std::array<TaskWorker, 8> workers{{
        {"app_voice", 8192, 32},
        {"app_network", 8192, 4},
        {"app_storage", 8192, 16},
        {"app_audio", 8192, 16},
        {"app_cleanup", 4096, 24, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT},
        {"app_network_io", 16384, 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, 2},
        {"app_image", 16384, 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, 2},
        {"app_transfer", 8192, 4},
    }};
    return workers;
}

static bool TakeJob(TaskWorker& worker, std::function<void()>& job) {
    std::lock_guard<std::mutex> lock(worker.mutex);
    if (worker.jobs.empty()) {
        return false;
    }
    job = std::move(worker.jobs.front());
    worker.jobs.pop_front();
    return true;
}

static void RunWorker(void* context) {
    auto& worker = *static_cast<TaskWorker*>(context);
    for (;;) {
        {
            std::function<void()> job;
            if (TakeJob(worker, job)) {
                try {
                    job();
                } catch (const std::bad_alloc&) {
                    ESP_LOGE(TAG, "operation=run_task worker=%s result=no_memory", worker.name);
                }
                continue;
            }
        }
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) != 0) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(worker.mutex);
            if (!worker.jobs.empty() || worker.retained) {
                continue;
            }
            // 队列与句柄一起交还，后续投递只能唤醒有效任务或创建新任务。
            worker.task = nullptr;
        }
        break;
    }
    if (worker.stack_caps != 0) {
        vTaskDeleteWithCaps(nullptr);
    } else {
        vTaskDelete(nullptr);
    }
}

bool TaskExecutor::Schedule(std::function<void()> callback, TaskDomain domain) try {
    const size_t index = static_cast<size_t>(domain);
    if (!callback || index >= Workers().size()) {
        return false;
    }
    auto& worker = Workers()[index];
    bool full = false;
    {
        std::lock_guard<std::mutex> lock(worker.mutex);
        full = worker.jobs.size() >= worker.capacity;
        if (!full) {
            worker.jobs.emplace_back();
            worker.jobs.back().swap(callback);
            BaseType_t created = pdPASS;
            if (worker.task == nullptr) {
                created = FirmwareCreateTask(RunWorker, worker.name, worker.stack_bytes, &worker,
                                              worker.priority, &worker.task,
                                              worker.stack_caps != 0 ? 0 : tskNO_AFFINITY, worker.stack_caps);
            }
            if (created != pdPASS) {
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
    ESP_LOGW(TAG, "operation=submit worker=%s result=%s", worker.name,
             full ? "queue_full" : "task_unavailable");
    return false;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "task message allocation failed");
    return false;
}

bool TaskExecutor::IsCurrent(TaskDomain domain) {
    const auto index = static_cast<size_t>(domain);
    if (index >= Workers().size()) {
        return false;
    }
    auto& worker = Workers()[index];
    std::lock_guard<std::mutex> lock(worker.mutex);
    return worker.task != nullptr && worker.task == xTaskGetCurrentTaskHandle();
}

void TaskExecutor::SetRetained(TaskDomain domain, bool retained) {
    const auto index = static_cast<size_t>(domain);
    if (index >= Workers().size()) {
        return;
    }
    auto& worker = Workers()[index];
    std::lock_guard<std::mutex> lock(worker.mutex);
    worker.retained = retained;
}
