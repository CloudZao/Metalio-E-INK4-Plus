#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <dirent.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <ff.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/reent.h>
#include <sys/stat.h>
#include <utime.h>
#include "sd_access.h"
#include "sd_directory_page.h"
#include "infra/storage/sd_paths.h"

static std::mutex s_mu;
static std::atomic<void (*)()> s_release_caches{nullptr};
static bool s_quiescing = false;
static unsigned s_pending = 0;
static bool s_mounted = false;
static StorageError s_unavailable{StorageErrorCode::NotReady, StorageStage::Access, ENODEV};
static unsigned s_mount_generation = 0;  // 每次更新挂载状态时作废旧介质缓存
static TaskHandle_t s_transition_owner = nullptr;
static unsigned s_transition_depth = 0;

struct SdAccessPermitSlot {
    TaskHandle_t task = nullptr;  // 持有权限的任务
    unsigned depth = 0;           // 临时权限与事务的总嵌套层数
    unsigned transactions = 0;    // 已接纳事务层数，允许在暂停访问后收尾
};

static SdAccessPermitSlot s_permits[8];

static StorageError AccessStatusLocked() {
    if (s_transition_owner || s_quiescing) {
        return {StorageErrorCode::Busy, StorageStage::Access, EBUSY};
    }
    return s_mounted ? StorageError{} : s_unavailable;
}

StorageError SdAccessStatus() {
    std::lock_guard<std::mutex> lock(s_mu);
    return AccessStatusLocked();
}

static bool Admit(StorageError* error = nullptr) {
    std::lock_guard<std::mutex> lock(s_mu);
    const auto task = xTaskGetCurrentTaskHandle();
    bool permitted = s_transition_owner == task;
    if (!s_transition_owner && s_mounted && !s_quiescing) {
        permitted = true;
    }
    for (const auto& slot : s_permits) {
        if (slot.task != task || slot.depth == 0) {
            continue;
        }
        // 普通 Permit 不能绕过卸载；已接纳事务可完成后续 rename/cleanup。
        permitted |= s_transition_owner ? slot.transactions != 0 : s_mounted;
    }
    if (permitted) {
        ++s_pending;
    } else if (error) {
        *error = AccessStatusLocked();
    }
    return permitted;
}

class SdAccessCompletion {
public:
    SdAccessCompletion() {
        std::lock_guard<std::mutex> lock(s_mu);
        ++s_pending;
    }

    ~SdAccessCompletion();
};

static void Finish() {
    std::lock_guard<std::mutex> lock(s_mu);
    configASSERT(s_pending != 0);
    --s_pending;
}

SdAccessCompletion::~SdAccessCompletion() {
    Finish();
}

static bool AddPermit(bool transaction = false) {
    std::lock_guard<std::mutex> lock(s_mu);
    auto task = xTaskGetCurrentTaskHandle();
    for (auto& slot : s_permits) {
        if (slot.task == task && slot.depth) {
            ++slot.depth;
            if (transaction) {
                ++slot.transactions;
            }
            return true;
        }
    }
    for (auto& slot : s_permits) {
        if (slot.depth == 0) {
            slot.task = task;
            slot.depth = 1;
            slot.transactions = transaction ? 1 : 0;
            return true;
        }
    }
    return false;
}

static void RemovePermit(bool transaction = false) {
    std::lock_guard<std::mutex> lock(s_mu);
    for (auto& slot : s_permits) {
        if (slot.task == xTaskGetCurrentTaskHandle() && slot.depth) {
            if (transaction) {
                configASSERT(slot.transactions != 0);
                --slot.transactions;
            }
            if (--slot.depth == 0) {
                slot.task = nullptr;
            }
            return;
        }
    }
    configASSERT(false);
}

// 链接包装器在 FIL/FF_DIR 的完整生命周期内保留访问计数，防止卸载使句柄失效。
extern "C" {
FRESULT __real_f_open(FIL*, const TCHAR*, BYTE);
FRESULT __real_f_close(FIL*);
FRESULT __real_f_opendir(FF_DIR*, const TCHAR*);
FRESULT __real_f_closedir(FF_DIR*);

FRESULT __wrap_f_open(FIL* file, const TCHAR* path, BYTE mode) {
    if (!Admit()) {
        return FR_NOT_READY;
    }
    FRESULT result = __real_f_open(file, path, mode);
    if (result != FR_OK) {
        Finish();
    }
    return result;
}

FRESULT __wrap_f_close(FIL* file) {
    FRESULT result = __real_f_close(file);
    if (result == FR_OK) {
        Finish();
    }
    return result;
}

FRESULT __wrap_f_opendir(FF_DIR* dir, const TCHAR* path) {
    if (!Admit()) {
        return FR_NOT_READY;
    }
    FRESULT result = __real_f_opendir(dir, path);
    if (result != FR_OK) {
        Finish();
    }
    return result;
}

FRESULT __wrap_f_closedir(FF_DIR* dir) {
    FRESULT result = __real_f_closedir(dir);
    if (result == FR_OK) {
        Finish();
    }
    return result;
}

#define SD_PATH_OPERATION(name, params, args) \
    FRESULT __real_##name params;             \
    FRESULT __wrap_##name params {            \
        if (!Admit())                         \
            return FR_NOT_READY;              \
        FRESULT result = __real_##name args;  \
        Finish();                             \
        return result;                        \
    }
SD_PATH_OPERATION(f_stat, (const TCHAR* path, FILINFO* info), (path, info))
SD_PATH_OPERATION(f_mkdir, (const TCHAR* path), (path))
SD_PATH_OPERATION(f_unlink, (const TCHAR* path), (path))
SD_PATH_OPERATION(f_rename, (const TCHAR* from, const TCHAR* to), (from, to))
SD_PATH_OPERATION(f_utime, (const TCHAR* path, const FILINFO* info), (path, info))
SD_PATH_OPERATION(f_getfree, (const TCHAR* path, DWORD* count, FATFS** fs), (path, count, fs))
#undef SD_PATH_OPERATION
}

static int AccessErrno(const StorageError& error) {
    if (error.system_error) {
        return error.system_error;
    }
    switch (error.code) {
        case StorageErrorCode::NotReady: return ENODEV;
        case StorageErrorCode::Timeout: return ETIMEDOUT;
        case StorageErrorCode::NoMemory: return ENOMEM;
        case StorageErrorCode::Busy:
        case StorageErrorCode::UsbOwned: return EBUSY;
        default: return EIO;
    }
}

static bool IsSdPath(const char* path) {
    constexpr const char* root = SD_MOUNT_POINT;
    constexpr size_t length = sizeof(SD_MOUNT_POINT) - 1;
    return path && strncmp(path, root, length) == 0 && (path[length] == '/' || path[length] == '\0');
}

// 在 VFS 查找上下文之前接纳，计数覆盖 FatFS 返回后的上下文访问。
extern "C" {
// 对 SD 路径在 VFS 上下文查找前申请完整操作权限。
#define SD_VFS_PATH(name, params, path, args) \
    int __real_##name params;                 \
    int __wrap_##name params {                \
        if (!IsSdPath(path))                  \
            return __real_##name args;        \
        SdAccessTransaction access;           \
        if (!access) {                        \
            errno = AccessErrno(access.Error()); \
            return -1;                        \
        }                                     \
        return __real_##name args;            \
    }
SD_VFS_PATH(_open_r, (struct _reent * r, const char* path, int flags, int mode), path, (r, path, flags, mode))
SD_VFS_PATH(_stat_r, (struct _reent * r, const char* path, struct stat* st), path, (r, path, st))
SD_VFS_PATH(_unlink_r, (struct _reent * r, const char* path), path, (r, path))
SD_VFS_PATH(_rename_r, (struct _reent * r, const char* from, const char* to), (IsSdPath(from) ? from : to),
            (r, from, to))
SD_VFS_PATH(_link_r, (struct _reent * r, const char* from, const char* to), (IsSdPath(from) ? from : to), (r, from, to))
SD_VFS_PATH(mkdir, (const char* path, mode_t mode), path, (path, mode))
SD_VFS_PATH(rmdir, (const char* path), path, (path))
SD_VFS_PATH(access, (const char* path, int mode), path, (path, mode))
SD_VFS_PATH(utime, (const char* path, const struct utimbuf* times), path, (path, times))
SD_VFS_PATH(truncate, (const char* path, off_t length), path, (path, length))
#undef SD_VFS_PATH
int __real__close_r(struct _reent*, int);

int __wrap__close_r(struct _reent* r, int fd) {
    SdAccessCompletion access;
    return __real__close_r(r, fd);
}

DIR* __real_opendir(const char*);

DIR* __wrap_opendir(const char* path) {
    if (!IsSdPath(path)) {
        return __real_opendir(path);
    }
    SdAccessTransaction access;
    if (!access) {
        errno = AccessErrno(access.Error());
        return nullptr;
    }
    return __real_opendir(path);
}

int __real_closedir(DIR*);

int __wrap_closedir(DIR* dir) {
    SdAccessCompletion access;
    return __real_closedir(dir);
}

esp_err_t __real_esp_vfs_fat_info(const char*, uint64_t*, uint64_t*);

esp_err_t __wrap_esp_vfs_fat_info(const char* path, uint64_t* total, uint64_t* free) {
    if (!IsSdPath(path)) {
        return __real_esp_vfs_fat_info(path, total, free);
    }
    SdAccessTransaction access;
    if (!access) {
        return ESP_ERR_INVALID_STATE;
    }
    return __real_esp_vfs_fat_info(path, total, free);
}

FRESULT __real_f_mount(FATFS*, const TCHAR*, BYTE);

FRESULT __wrap_f_mount(FATFS* fs, const TCHAR* path, BYTE option) {
    SdAccessTransition transition;
    if (!transition) {
        return FR_TIMEOUT;
    }
    SdAccessSetMounted(false);
    const auto result = __real_f_mount(fs, path, option);
    if (result == FR_OK && fs) {
        SdAccessSetMounted(true);
    }
    return result;
}
}

void SdAccessConfigureCacheRelease(void (*release)()) {
    s_release_caches.store(release, std::memory_order_release);
}

void SdAccessQuiesce() {
    {
        std::lock_guard<std::mutex> lock(s_mu);
        s_quiescing = true;
    }
    SdCancelDirectoryPages();
    if (auto release = s_release_caches.load(std::memory_order_acquire)) {
        release();
    }
}

void SdAccessResume() {
    std::lock_guard<std::mutex> lock(s_mu);
    s_quiescing = false;
}

bool SdAccessIsQuiescing() {
    std::lock_guard<std::mutex> lock(s_mu);
    return s_quiescing;
}

bool SdAccessIsIdle() {
    std::lock_guard<std::mutex> lock(s_mu);
    return s_pending == 0;
}

unsigned SdAccessPending() {
    std::lock_guard<std::mutex> lock(s_mu);
    return s_pending;
}

void SdAccessSetMounted(bool mounted, StorageError unavailable) {
    std::lock_guard<std::mutex> lock(s_mu);
    s_mounted = mounted;
    s_unavailable = unavailable ? unavailable : StorageError{StorageErrorCode::NotReady, StorageStage::Access, ENODEV};
    ++s_mount_generation;
}

unsigned SdAccessMountGeneration() {
    std::lock_guard<std::mutex> lock(s_mu);
    return s_mount_generation;
}

SdAccessTransition::SdAccessTransition(unsigned timeout_ms) {
    SdCancelDirectoryPages();
    if (auto release = s_release_caches.load(std::memory_order_acquire)) {
        release();
    }
    const auto task = xTaskGetCurrentTaskHandle();
    const auto start = xTaskGetTickCount();
    const auto timeout = pdMS_TO_TICKS(timeout_ms);
    bool claimed = false;
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(s_mu);
            for (const auto& slot : s_permits) {
                if (slot.task == task && slot.transactions) {
                    ESP_LOGE("SdAccess", "transition rejected: caller owns an active transaction");
                    return;
                }
            }
            if (s_transition_owner == task && !claimed) {
                ++s_transition_depth;
                acquired_ = true;
                return;
            }
            if (!s_transition_owner) {
                s_transition_owner = task;
                s_transition_depth = 1;
                claimed = true;
            }
            if (claimed && s_pending == 0) {
                acquired_ = true;
                return;
            }
        }
        if (xTaskGetTickCount() - start >= timeout) {
            break;
        }
        vTaskDelay(1);
    }
    unsigned pending;
    {
        std::lock_guard<std::mutex> lock(s_mu);
        pending = s_pending;
        if (claimed) {
            s_transition_owner = nullptr;
            s_transition_depth = 0;
        }
    }
    ESP_LOGW("SdAccess", "transition timed out: reason=%s pending=%u; filesystem retained",
             claimed ? "inflight_access" : "owner_busy", pending);
}

SdAccessTransition::~SdAccessTransition() {
    if (!acquired_) {
        return;
    }
    std::lock_guard<std::mutex> lock(s_mu);
    configASSERT(s_transition_owner == xTaskGetCurrentTaskHandle());
    if (--s_transition_depth == 0) {
        s_transition_owner = nullptr;
    }
}

SdAccessPermit::SdAccessPermit() : acquired_(AddPermit()) {
    if (!acquired_) {
        ESP_LOGW("SdAccess", "permit unavailable: task slots exhausted");
    }
}

SdAccessPermit::~SdAccessPermit() {
    if (acquired_) {
        RemovePermit();
    }
}

SdAccessDeferredTransaction::SdAccessDeferredTransaction() : admitted_(Admit()) {}

SdAccessDeferredTransaction::~SdAccessDeferredTransaction() {
    if (admitted_) {
        Finish();
    }
}

SdAccessTransaction::SdAccessTransaction(SdAccessDeferredTransaction& deferred) : admitted_(false) {
    if (!deferred.admitted_) {
        error_ = {StorageErrorCode::Busy, StorageStage::Access, EBUSY};
    } else if (AddPermit(true)) {
        deferred.admitted_ = false;
        admitted_ = true;
    } else {
        error_ = {StorageErrorCode::NoMemory, StorageStage::Access, ENOMEM};
    }
}

SdAccessTransaction::SdAccessTransaction() : admitted_(Admit(&error_)) {
    if (admitted_ && !AddPermit(true)) {
        Finish();
        admitted_ = false;
        error_ = {StorageErrorCode::NoMemory, StorageStage::Access, ENOMEM};
    }
}

SdAccessTransaction::~SdAccessTransaction() {
    if (admitted_) {
        RemovePermit(true);
        Finish();
    }
}
