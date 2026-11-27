#pragma once

#include <cstddef>
#include <cstdint>
#include "storage_error.h"

/**
 * @brief 串行写入 .tmp 暂存并备份替换目标；提交失败恢复旧文件，保留暂存供恢复。
 * @note 恢复失败时保留 .backup，下一次提交重试恢复；不保证 FAT 断电原子性。
 */
bool WriteFileAtomic(const char* path, const uint8_t* data, size_t len, StorageError* error = nullptr);
