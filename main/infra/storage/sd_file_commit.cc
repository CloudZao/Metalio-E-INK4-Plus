#include <cerrno>
#include <cstdio>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <esp_log.h>
#include "sd_file_commit.h"
#ifndef EBOOK_EMULATOR
#include "boards/common/storage/sd_access.h"
#endif

static std::mutex s_commit_mutex;

static SdCommitResult SdCommitFailure(StorageStage stage, StorageError* output) {
    const int native_error = errno;
    CaptureStorageError(output, stage, native_error);
    LogStorageError("SdCommit", StorageErrorFromErrno(stage, native_error));
    errno = native_error;
    return SdCommitResult::IoError;
}

SdCommitResult SdCommitFile(const std::string& temporary, const std::string& target, bool overwrite,
                            const std::string& backup_path, StorageError* error) {
    if (error) {
        *error = {};
    }
    #ifndef EBOOK_EMULATOR
    SdAccessTransaction transaction;
    if (!transaction) {
        if (error) {
            *error = transaction.Error();
        }
        LogStorageError("SdCommit", transaction.Error());
        return SdCommitResult::IoError;
    }
    #endif
    const std::string backup = backup_path.empty() ? target + ".cloud-backup" : backup_path;
    if (temporary.empty() || target.empty() || temporary == target || backup == target || backup == temporary) {
        errno = EINVAL;
        return SdCommitFailure(StorageStage::Open, error);
    }
    std::lock_guard<std::mutex> lock(s_commit_mutex);
    struct stat st{};
    // 目标存在表示替换已完成；目标缺失表示替换中断，恢复旧备份。
    if (stat(backup.c_str(), &st) == 0) {
        if (!S_ISREG(st.st_mode)) {
            errno = EINVAL;
            return SdCommitFailure(StorageStage::Backup, error);
        }
        if (stat(target.c_str(), &st) == 0) {
            if (!S_ISREG(st.st_mode)) {
                errno = EINVAL;
                return SdCommitFailure(StorageStage::Restore, error);
            }
            if (!overwrite) {
                if (error) {
                    *error = {StorageErrorCode::Conflict, StorageStage::Replace, EEXIST};
                }
                errno = EEXIST;
                return SdCommitResult::Conflict;
            }
            if (unlink(backup.c_str()) != 0) {
                return SdCommitFailure(StorageStage::Remove, error);
            }
        } else if (errno != ENOENT || rename(backup.c_str(), target.c_str()) != 0) {
            return SdCommitFailure(StorageStage::Restore, error);
        }
    } else if (errno != ENOENT) {
        return SdCommitFailure(StorageStage::Stat, error);
    }
    if (stat(temporary.c_str(), &st) != 0) {
        return SdCommitFailure(StorageStage::Stat, error);
    }
    if (!S_ISREG(st.st_mode)) {
        errno = EINVAL;
        return SdCommitFailure(StorageStage::Stat, error);
    }
    const int result = stat(target.c_str(), &st);
    const bool exists = result == 0;
    if (result != 0 && errno != ENOENT) {
        return SdCommitFailure(StorageStage::Stat, error);
    }
    if (exists && (!overwrite || !S_ISREG(st.st_mode))) {
        if (error) {
            *error = {StorageErrorCode::Conflict, StorageStage::Replace, EEXIST};
        }
        errno = EEXIST;
        return SdCommitResult::Conflict;
    }
    if (exists && rename(target.c_str(), backup.c_str()) != 0) {
        return SdCommitFailure(StorageStage::Backup, error);
    }
    if (rename(temporary.c_str(), target.c_str()) != 0) {
        const int replace_error = errno;
        CaptureStorageError(error, StorageStage::Replace, replace_error);
        int restore_error = 0;
        if (exists && rename(backup.c_str(), target.c_str()) != 0) {
            restore_error = errno;
        }
        ESP_LOGE("SdCommit", "operation=commit stage=replace errno=%d restore=%s restore_errno=%d",
                 replace_error, exists ? (restore_error ? "failed" : "ok") : "not_needed", restore_error);
        errno = replace_error;
        return SdCommitResult::IoError;
    }
    if (exists && unlink(backup.c_str()) != 0) {
        ESP_LOGW("SdCommit", "operation=commit stage=remove_backup errno=%d; backup retained", errno);
    }
    return SdCommitResult::Ok;
}
