#include <cerrno>
#include <cstdio>
#include <mutex>
#include <string>
#include <unistd.h>
#include <esp_log.h>
#include "file_write.h"
#include "sd_file_commit.h"
#ifndef EBOOK_EMULATOR
#include "boards/common/storage/sd_access.h"
#endif

static std::mutex s_file_write_mutex;


bool WriteFileAtomic(const char* path, const uint8_t* data, size_t len, StorageError* error) {
    if (error) {
        *error = {};
    }
    #ifndef EBOOK_EMULATOR
    SdAccessTransaction transaction;
    if (!transaction) {
        if (error) {
            *error = transaction.Error();
        }
        LogStorageError("FileWrite", transaction.Error());
        return false;
    }
    #endif
    if (path == nullptr || path[0] == '\0' || data == nullptr || len == 0) {
        CaptureStorageError(error, StorageStage::Open, EINVAL);
        return false;
    }
    // 固定暂存路径的写入、提交和恢复属于同一事务；不允许其他保存者截断它。
    std::lock_guard<std::mutex> lock(s_file_write_mutex);
    std::string tmp(path);
    tmp.append(".tmp");
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) {
        const auto failure = StorageErrorFromErrno(StorageStage::Open, errno);
        if (error) {
            *error = failure;
        }
        LogStorageError("FileWrite", failure);
        return false;
    }
    StorageStage failure = StorageStage::None;
    if (std::fwrite(data, 1, len, f) != len) {
        failure = StorageStage::Write;
    } else if (std::fflush(f) != 0) {
        failure = StorageStage::Flush;
    }
    int native_error = errno;
    if (std::fclose(f) != 0 && failure == StorageStage::None) {
        failure = StorageStage::Close;
        native_error = errno;
    }
    if (failure != StorageStage::None) {
        const auto detail = StorageErrorFromErrno(failure, native_error);
        if (error) {
            *error = detail;
        }
        LogStorageError("FileWrite", detail);
        if (unlink(tmp.c_str()) != 0 && errno != ENOENT) {
            ESP_LOGW("FileWrite", "operation=replace stage=cleanup errno=%d", errno);
        }
        return false;
    }
    return SdCommitFile(tmp, path, true, std::string(path) + ".backup", error) == SdCommitResult::Ok;
}
