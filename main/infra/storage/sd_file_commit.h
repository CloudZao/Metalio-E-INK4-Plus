#pragma once
#include <string>
#include "storage_error.h"

enum class SdCommitResult {
    Ok,        // 新文件已成为目标文件
    Conflict,  // 目标存在且不允许覆盖
    IoError,   // 提交或恢复失败，备份保留
};

/**
 * @brief 提交已刷盘并关闭的临时文件；失败时恢复原文件。
 * @param backup 非空时使用指定备份路径；空时使用兼容的 .cloud-backup 后缀。
 * @note 调用方须保护同一目标的完整事务；不保证 FAT 断电原子性或多文件原子性。
 */
SdCommitResult SdCommitFile(const std::string& temporary, const std::string& target, bool overwrite,
                            const std::string& backup = {}, StorageError* error = nullptr);
