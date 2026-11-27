#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <esp_elf.h>
#include "business/app_store/app_store.h"

struct AppStoreExecutable {
    AppStoreItem item;
    esp_elf_t elf{};
    bool initialized = false;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> ready{false};
    bool loaded = false;
    uint32_t asset_size = 0;
    std::string error;

    /** @brief 释放加载器；加载任务持有最后引用时也能完成回收。 */
    ~AppStoreExecutable();

    /** @brief 在加载完成后关闭可执行资源，不修改已发布应用ABI。 */
    void Close();
};

/** @brief 在独立内部RAM栈加载及重定位ELF，页面离开不转移回收责任。 */
std::shared_ptr<AppStoreExecutable> AppStoreOpenExecutable(const AppStoreItem& item);
