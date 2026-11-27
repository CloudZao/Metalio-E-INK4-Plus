#include <cerrno>
#include <esp_log.h>
#include "storage_error.h"

StorageError StorageErrorFromErrno(StorageStage stage, int error) {
    StorageErrorCode code;
    switch (error) {
        case ENODEV: code = StorageErrorCode::NotReady; break;
        case EBUSY: case EAGAIN: code = StorageErrorCode::Busy; break;
        case ETIMEDOUT: code = StorageErrorCode::Timeout; break;
        case ENOENT: code = StorageErrorCode::NotFound; break;
        case EACCES: case EPERM: code = StorageErrorCode::AccessDenied; break;
        case ENOSPC: code = StorageErrorCode::NoSpace; break;
        case EROFS: code = StorageErrorCode::ReadOnly; break;
        case ENOMEM: code = StorageErrorCode::NoMemory; break;
        case EINVAL: case ENAMETOOLONG: code = StorageErrorCode::InvalidArgument; break;
        case EEXIST: code = StorageErrorCode::Conflict; break;
        default: code = StorageErrorCode::Io; break;
    }
    return {code, stage, error};
}

const char* StorageErrorName(StorageErrorCode code) {
    switch (code) {
        case StorageErrorCode::None: return "ok";
        case StorageErrorCode::NotReady: return "not_ready";
        case StorageErrorCode::Busy: return "busy";
        case StorageErrorCode::UsbOwned: return "usb_owned";
        case StorageErrorCode::Timeout: return "timeout";
        case StorageErrorCode::NotFound: return "not_found";
        case StorageErrorCode::AccessDenied: return "access_denied";
        case StorageErrorCode::NoSpace: return "no_space";
        case StorageErrorCode::ReadOnly: return "read_only";
        case StorageErrorCode::NoMemory: return "no_memory";
        case StorageErrorCode::InvalidArgument: return "invalid_argument";
        case StorageErrorCode::Io: return "io";
        case StorageErrorCode::Conflict: return "conflict";
        case StorageErrorCode::Rejected: return "rejected";
        case StorageErrorCode::Cancelled: return "cancelled";
    }
    return "unknown";
}

const char* StorageStageName(StorageStage stage) {
    switch (stage) {
        case StorageStage::None: return "none";
        case StorageStage::Access: return "access";
        case StorageStage::Mount: return "mount";
        case StorageStage::Submit: return "submit";
        case StorageStage::DirectoryOpen: return "directory_open";
        case StorageStage::DirectoryRead: return "directory_read";
        case StorageStage::Stat: return "stat";
        case StorageStage::Open: return "open";
        case StorageStage::Read: return "read";
        case StorageStage::Seek: return "seek";
        case StorageStage::Write: return "write";
        case StorageStage::Flush: return "flush";
        case StorageStage::Close: return "close";
        case StorageStage::CreateDirectory: return "mkdir";
        case StorageStage::Backup: return "backup";
        case StorageStage::Replace: return "replace";
        case StorageStage::Restore: return "restore";
        case StorageStage::Remove: return "remove";
        case StorageStage::Capacity: return "capacity";
    }
    return "unknown";
}

void LogStorageError(const char* tag, const StorageError& error) {
    if (!error || error.code == StorageErrorCode::Cancelled) {
        return;
    }
    const int saved_errno = errno;
    if (error.code == StorageErrorCode::Busy || error.code == StorageErrorCode::UsbOwned ||
        error.code == StorageErrorCode::NotReady || error.code == StorageErrorCode::NotFound ||
        error.code == StorageErrorCode::Timeout || error.code == StorageErrorCode::Rejected) {
        ESP_LOGW(tag, "storage stage=%s code=%u reason=%s errno=%d sdk=0x%x", StorageStageName(error.stage),
                 unsigned(error.code), StorageErrorName(error.code), error.system_error, unsigned(error.platform_error));
    } else {
        ESP_LOGE(tag, "storage stage=%s code=%u reason=%s errno=%d sdk=0x%x", StorageStageName(error.stage),
                 unsigned(error.code), StorageErrorName(error.code), error.system_error, unsigned(error.platform_error));
    }
    errno = saved_errno;
}

void CaptureStorageError(StorageError* output, StorageStage stage, int error) {
    if (output && !*output) {
        *output = StorageErrorFromErrno(stage, error);
    }
}
