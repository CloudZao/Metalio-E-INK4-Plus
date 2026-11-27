#include "storage_error_text.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"

const char* StorageErrorText(const StorageError& error) {
    switch (error.code) {
        case StorageErrorCode::None: return "";
        case StorageErrorCode::NotReady: return LangString_STORAGE_NOT_READY;
        case StorageErrorCode::Busy: return LangString_STORAGE_BUSY;
        case StorageErrorCode::UsbOwned: return LangString_STORAGE_USB_OWNED;
        case StorageErrorCode::Timeout: return LangString_STORAGE_TIMEOUT;
        case StorageErrorCode::NotFound: return LangString_STORAGE_NOT_FOUND;
        case StorageErrorCode::AccessDenied: return LangString_STORAGE_ACCESS_DENIED;
        case StorageErrorCode::NoSpace: return LangString_STORAGE_FULL;
        case StorageErrorCode::ReadOnly: return LangString_STORAGE_READ_ONLY;
        case StorageErrorCode::NoMemory: return LangString_STORAGE_MEMORY;
        case StorageErrorCode::Rejected: return LangString_STORAGE_BUSY;
        case StorageErrorCode::Conflict: return LangString_STORAGE_CONFLICT;
        case StorageErrorCode::Cancelled: return LangString_COMMON_CANCEL;
        case StorageErrorCode::InvalidArgument: return LangString_BOOK_BAD_ARG;
        default: return error.stage == StorageStage::Mount ? LangString_STORAGE_MOUNT_FAILED
                                                         : LangString_STORAGE_IO_ERROR;
    }
}
