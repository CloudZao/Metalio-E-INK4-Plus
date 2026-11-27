#include <new>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "infra/tasks/task_create.h"
#include "assets/lang_app_store.h"
#include "app_executable.h"

void AppStoreExecutable::Close() {
    if (initialized) {
        esp_elf_deinit(&elf);
        initialized = false;
    }
    loaded = false;
}

AppStoreExecutable::~AppStoreExecutable() { Close(); }

static void LoadExecutable(void* context) {
    {
        std::unique_ptr<std::shared_ptr<AppStoreExecutable>> owner(
            static_cast<std::shared_ptr<AppStoreExecutable>*>(context));
        auto& executable = **owner;
        try {
            if (!executable.cancelled.load()) {
                executable.initialized = esp_elf_init(&executable.elf) == 0;
                executable.loaded = executable.initialized &&
                    AppStoreLoad(executable.item, executable.elf, executable.error);
                if (!executable.initialized) {
                    executable.error = LangString_STORE_MEMORY_ERROR;
                    ESP_LOGE("AppExecutable", "operation=load stage=elf_init result=failed");
                }
                if (executable.loaded) {
                    executable.asset_size = AppStoreAssetSize(executable.item);
                }
            }
        } catch (const std::bad_alloc&) {
            executable.loaded = false;
            executable.error = LangString_STORE_MEMORY_ERROR;
            ESP_LOGE("AppExecutable", "operation=load stage=execute result=no_memory");
        }
        executable.ready.store(true, std::memory_order_release);
    }
    vTaskDelete(nullptr);
}

std::shared_ptr<AppStoreExecutable> AppStoreOpenExecutable(const AppStoreItem& item) try {
    auto executable = std::make_shared<AppStoreExecutable>();
    executable->item = item;
    executable->error.reserve(128);
    auto owner = std::make_unique<std::shared_ptr<AppStoreExecutable>>(executable);
    constexpr unsigned stack_bytes = 12 * 1024; // 重定位同步PSRAM代码缓存，不能使用外部RAM栈
    if (FirmwareCreateTask(LoadExecutable, "app_load", stack_bytes, owner.get(), 2, nullptr, 0, 0) != pdPASS) {
        executable->error = LangString_STORE_MEMORY_ERROR;
        executable->ready.store(true, std::memory_order_release);
        ESP_LOGW("AppExecutable", "operation=load stage=create_task result=unavailable");
    } else {
        owner.release();
    }
    return executable;
} catch (const std::bad_alloc&) {
    ESP_LOGE("AppExecutable", "operation=load stage=submit result=no_memory");
    return {};
}
