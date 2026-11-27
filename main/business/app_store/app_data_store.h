#pragma once
#include <cstdint>
#include <string>
#include "business/app_store/app_store.h"

/** @brief 为已安装应用生成兼容既有持久化目录的存储标识。 */
std::string AppStorageKey(const AppStoreItem& item);

/** @brief 读取应用数据，空缓冲区时返回所需字节数。 */
int AppReadData(const std::string& storage_key, const char* key, void* buffer, uint32_t capacity);

/** @brief 事务写入应用数据，保留容量、键数量和备份恢复约束。 */
int AppWriteData(const std::string& storage_key, const char* key, const void* data, uint32_t length);

/** @brief 删除应用数据及其恢复副本。 */
int AppRemoveData(const std::string& storage_key, const char* key);
