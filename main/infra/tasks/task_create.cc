#include <mutex>
#include <freertos/idf_additions.h>
#include "task_create.h"
#include "infra/diagnostics/firmware_memory.h"

BaseType_t FirmwareCreateTask(TaskFunction_t entry, const char* name, uint32_t stack_bytes, void* argument,
                              UBaseType_t priority, TaskHandle_t* handle, BaseType_t core, uint32_t stack_caps) {
    BaseType_t result = pdFAIL;
    bool admitted = false;
    {
        std::lock_guard<std::mutex> lock(firmware_internal_budget_mutex);
        admitted = FirmwareTaskMemoryAvailable(stack_bytes, stack_caps);
        if (admitted) {
            if (stack_caps != 0) {
                result = xTaskCreatePinnedToCoreWithCaps(entry, name, stack_bytes, argument, priority,
                                                        handle, core, stack_caps);
            } else {
                result = xTaskCreatePinnedToCore(entry, name, stack_bytes, argument, priority, handle, core);
            }
        }
    }
    if (result != pdPASS) {
        ESP_LOGW("TaskCreate", "operation=create task=%s result=%s stack=%lu caps=0x%lx", name,
                 admitted ? "allocation_failed" : "internal_floor", static_cast<unsigned long>(stack_bytes),
                 static_cast<unsigned long>(stack_caps));
    }
    return result;
}
