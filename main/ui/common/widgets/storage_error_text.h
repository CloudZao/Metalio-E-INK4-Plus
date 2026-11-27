#pragma once
#include "infra/storage/storage_error.h"

/** @brief 将共享存储错误转换为当前语言的用户提示，不执行存储操作。 */
const char* StorageErrorText(const StorageError& error);
