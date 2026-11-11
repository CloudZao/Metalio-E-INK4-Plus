#include "ap_request_worker.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/idf_additions.h>
#include <lwip/sockets.h>

namespace {
constexpr const char* TAG = "ApWorker";
#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
constexpr uint32_t kMemoryCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
constexpr uint32_t kMemoryCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
}

bool ApRequestWorker::Start(const char* name, bool internal_stack) {
    if (queue_) return true;
    if (!mutex_) mutex_ = xSemaphoreCreateMutexStatic(&mutex_storage_);
    if (!done_) done_ = xSemaphoreCreateBinaryStatic(&done_storage_);
    if (!mutex_ || !done_) return false;
    xSemaphoreTake(done_, 0);
    buffer_ = static_cast<uint8_t*>(heap_caps_malloc(kDepth * sizeof(Job), kMemoryCaps));
    if (!buffer_) return false;
    queue_ = xQueueCreateStatic(kDepth, sizeof(Job), buffer_, &queue_storage_);
    for (auto& fd : fds_) fd = -1;
    if (!queue_) { heap_caps_free(buffer_); buffer_ = nullptr; return false; }
    accepting_ = true;
    const uint32_t stack_caps = internal_stack ? MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT : kMemoryCaps;
    if (xTaskCreateWithCaps(Run, name, 16384, this, 5, nullptr, stack_caps) != pdPASS) {
        accepting_ = false;
        vQueueDelete(queue_); queue_ = nullptr;
        heap_caps_free(buffer_); buffer_ = nullptr;
        return false;
    }
    return true;
}

esp_err_t ApRequestWorker::Submit(httpd_req_t* req, esp_err_t (*handler)(httpd_req_t*)) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    int* slot = nullptr;
    for (auto& fd : fds_) if (fd < 0) { slot = &fd; break; }
    if (!accepting_ || !slot || uxQueueSpacesAvailable(queue_) == 0) {
        xSemaphoreGive(mutex_);
        return ESP_ERR_NO_MEM;
    }
    Job job;
    const esp_err_t err = httpd_req_async_handler_begin(req, &job.req);
    if (err != ESP_OK) { xSemaphoreGive(mutex_); return err; }
    job.handler = handler;
    *slot = httpd_req_to_sockfd(job.req);
    if (xQueueSend(queue_, &job, 0) != pdTRUE) {
        *slot = -1;
        httpd_req_async_handler_complete(job.req);
        xSemaphoreGive(mutex_);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(mutex_);
    return ESP_OK;
}

bool ApRequestWorker::Cleanup(void (*cleanup)(void*), void* context) {
    if (!queue_) return false;
    Job job;
    job.kind = Kind::Cleanup; job.cleanup = cleanup; job.context = context;
    return xQueueSend(queue_, &job, 0) == pdTRUE;
}

void ApRequestWorker::RemoveFd(int fd) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (auto& pending : fds_) if (pending == fd) { pending = -1; break; }
    xSemaphoreGive(mutex_);
}

void ApRequestWorker::CancelRequests() {
    if (!queue_) return;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    accepting_ = false;
    // 持锁关闭读写方向，工作任务移除登记后才允许 SDK 回收和复用描述符。
    for (const auto fd : fds_) if (fd >= 0) shutdown(fd, SHUT_RDWR);
    xSemaphoreGive(mutex_);
}

void ApRequestWorker::Drain() {
    if (!queue_) return;
    xSemaphoreTake(done_, 0);
    Job job; job.kind = Kind::Barrier;
    xQueueSend(queue_, &job, portMAX_DELAY);
    xSemaphoreTake(done_, portMAX_DELAY);
}

void ApRequestWorker::Stop() {
    if (!queue_) return;
    CancelRequests();
    xSemaphoreTake(done_, 0);
    Job job; job.kind = Kind::Stop;
    xQueueSend(queue_, &job, portMAX_DELAY);
    xSemaphoreTake(done_, portMAX_DELAY);
    vQueueDelete(queue_); queue_ = nullptr;
    heap_caps_free(buffer_); buffer_ = nullptr;
}

void ApRequestWorker::Run(void* arg) {
    auto& worker = *static_cast<ApRequestWorker*>(arg);
    Job job;
    while (xQueueReceive(worker.queue_, &job, portMAX_DELAY) == pdTRUE) {
        if (job.kind == Kind::Stop) break;
        if (job.kind == Kind::Barrier) { xSemaphoreGive(worker.done_); continue; }
        if (job.kind == Kind::Cleanup) { job.cleanup(job.context); continue; }
        const int fd = httpd_req_to_sockfd(job.req);
        const esp_err_t err = job.handler(job.req);
        if (err != ESP_OK) shutdown(fd, SHUT_RDWR);
        worker.RemoveFd(fd);
        if (httpd_req_async_handler_complete(job.req) != ESP_OK) {
            ESP_LOGE(TAG, "async request completion failed");
        }
    }
    xSemaphoreGive(worker.done_);
    vTaskDeleteWithCaps(nullptr);
}
