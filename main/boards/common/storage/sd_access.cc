#include "sd_access.h"
#include "sd_paths.h"

#include <mutex>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ff.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <sys/reent.h>
#include <sys/stat.h>
#include <dirent.h>
#include <utime.h>
#include <cerrno>
#include <cstring>

namespace {
std::mutex s_mu;
bool s_quiescing = false;
unsigned s_pending = 0;
bool s_mounted = false;
TaskHandle_t s_transition_owner = nullptr;
unsigned s_transition_depth = 0;
struct PermitSlot {
    TaskHandle_t task = nullptr; // 持有权限的任务
    unsigned depth = 0; // 临时权限与事务的总嵌套层数
    unsigned transactions = 0; // 已接纳事务层数，允许在暂停访问后收尾
};
PermitSlot s_permits[8];

bool Admit() {
    std::lock_guard<std::mutex> lock(s_mu);
    const auto task = xTaskGetCurrentTaskHandle();
    bool permitted = s_transition_owner == task;
    if (!s_transition_owner && s_mounted && !s_quiescing) permitted = true;
    for (const auto& slot : s_permits) {
        if (slot.task != task || slot.depth == 0) continue;
        // 普通 Permit 不能绕过卸载；已接纳事务可完成后续 rename/cleanup。
        permitted |= s_transition_owner ? slot.transactions != 0 : s_mounted;
    }
    if (permitted) ++s_pending;
    return permitted;
}
class Completion {
public:
    Completion() {
        std::lock_guard<std::mutex> lock(s_mu);
        ++s_pending;
    }
    ~Completion();
};
void Finish() {
    std::lock_guard<std::mutex> lock(s_mu);
    configASSERT(s_pending != 0);
    --s_pending;
}
Completion::~Completion() { Finish(); }
bool AddPermit(bool transaction = false) {
    std::lock_guard<std::mutex> lock(s_mu);
    auto task = xTaskGetCurrentTaskHandle();
    for (auto& slot : s_permits) {
        if (slot.task == task && slot.depth) {
            ++slot.depth;
            if (transaction) ++slot.transactions;
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
void RemovePermit(bool transaction = false) {
    std::lock_guard<std::mutex> lock(s_mu);
    for (auto& slot : s_permits) {
        if (slot.task == xTaskGetCurrentTaskHandle() && slot.depth) {
            if (transaction) {
                configASSERT(slot.transactions != 0);
                --slot.transactions;
            }
            if (--slot.depth == 0) slot.task = nullptr;
            return;
        }
    }
    configASSERT(false);
}
}

namespace sd_access {
void Quiesce() {
    std::lock_guard<std::mutex> lock(s_mu);
    s_quiescing = true;
}
void Resume() {
    std::lock_guard<std::mutex> lock(s_mu);
    s_quiescing = false;
}
bool IsQuiescing() {
    std::lock_guard<std::mutex> lock(s_mu);
    return s_quiescing;
}
bool IsIdle() {
    std::lock_guard<std::mutex> lock(s_mu);
    return s_pending == 0;
}
unsigned Pending() {
    std::lock_guard<std::mutex> lock(s_mu);
    return s_pending;
}
void SetMounted(bool mounted) {
    std::lock_guard<std::mutex> lock(s_mu);
    s_mounted = mounted;
}
Transition::Transition(unsigned timeout_ms) {
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
        if (xTaskGetTickCount() - start >= timeout) break;
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
Transition::~Transition() {
    if (!acquired_) return;
    std::lock_guard<std::mutex> lock(s_mu);
    configASSERT(s_transition_owner == xTaskGetCurrentTaskHandle());
    if (--s_transition_depth == 0) s_transition_owner = nullptr;
}
Permit::Permit() : acquired_(AddPermit()) {
    if (!acquired_) ESP_LOGW("SdAccess", "permit unavailable: task slots exhausted");
}
Permit::~Permit() { if (acquired_) RemovePermit(); }
DeferredTransaction::DeferredTransaction() : admitted_(Admit()) {}
DeferredTransaction::~DeferredTransaction() { if (admitted_) Finish(); }
Transaction::Transaction(DeferredTransaction& deferred) : admitted_(false) {
    if (deferred.admitted_ && AddPermit(true)) {
        deferred.admitted_ = false;
        admitted_ = true;
    }
}
Transaction::Transaction() : admitted_(Admit()) {
    if (admitted_ && !AddPermit(true)) {
        Finish();
        admitted_ = false;
        ESP_LOGW("SdAccess", "transaction rejected: task slots exhausted");
    }
}
Transaction::~Transaction() {
    if (admitted_) {
        RemovePermit(true);
        Finish();
    }
}
}

// 链接包装器在 FIL/FF_DIR 的完整生命周期内保留访问计数，防止卸载使句柄失效。
extern "C" {
FRESULT __real_f_open(FIL*, const TCHAR*, BYTE);
FRESULT __real_f_close(FIL*);
FRESULT __real_f_opendir(FF_DIR*, const TCHAR*);
FRESULT __real_f_closedir(FF_DIR*);
FRESULT __wrap_f_open(FIL* file, const TCHAR* path, BYTE mode) {
    if (!Admit()) return FR_NOT_READY;
    FRESULT result = __real_f_open(file, path, mode);
    if (result != FR_OK) Finish();
    return result;
}
FRESULT __wrap_f_close(FIL* file) {
    FRESULT result = __real_f_close(file);
    if (result == FR_OK) Finish();
    return result;
}
FRESULT __wrap_f_opendir(FF_DIR* dir, const TCHAR* path) {
    if (!Admit()) return FR_NOT_READY;
    FRESULT result = __real_f_opendir(dir, path);
    if (result != FR_OK) Finish();
    return result;
}
FRESULT __wrap_f_closedir(FF_DIR* dir) {
    FRESULT result = __real_f_closedir(dir);
    if (result == FR_OK) Finish();
    return result;
}

#define SD_PATH_OPERATION(name, params, args) \
    FRESULT __real_##name params; \
    FRESULT __wrap_##name params { \
        if (!Admit()) return FR_NOT_READY; \
        FRESULT result = __real_##name args; \
        Finish(); \
        return result; \
    }
SD_PATH_OPERATION(f_stat, (const TCHAR* path, FILINFO* info), (path, info))
SD_PATH_OPERATION(f_mkdir, (const TCHAR* path), (path))
SD_PATH_OPERATION(f_unlink, (const TCHAR* path), (path))
SD_PATH_OPERATION(f_rename, (const TCHAR* from, const TCHAR* to), (from, to))
SD_PATH_OPERATION(f_utime, (const TCHAR* path, const FILINFO* info), (path, info))
SD_PATH_OPERATION(f_getfree, (const TCHAR* path, DWORD* count, FATFS** fs), (path, count, fs))
#undef SD_PATH_OPERATION
}

namespace {
bool IsSdPath(const char* path) {
    constexpr const char* root = SD_MOUNT_POINT;
    constexpr size_t length = sizeof(SD_MOUNT_POINT) - 1;
    return path && strncmp(path, root, length) == 0 && (path[length] == '/' || path[length] == '\0');
}
}

// 在 VFS 查找上下文之前接纳，计数覆盖 FatFS 返回后的上下文访问。
extern "C" {
// 对 SD 路径在 VFS 上下文查找前申请完整操作权限。
#define SD_VFS_PATH(name, params, path, args) \
    int __real_##name params; \
    int __wrap_##name params { \
        if (!IsSdPath(path)) return __real_##name args; \
        sd_access::Transaction access; \
        if (!access) { errno = EBUSY; return -1; } \
        return __real_##name args; \
    }
SD_VFS_PATH(_open_r, (struct _reent* r, const char* path, int flags, int mode), path, (r, path, flags, mode))
SD_VFS_PATH(_stat_r, (struct _reent* r, const char* path, struct stat* st), path, (r, path, st))
SD_VFS_PATH(_unlink_r, (struct _reent* r, const char* path), path, (r, path))
SD_VFS_PATH(_rename_r, (struct _reent* r, const char* from, const char* to), (IsSdPath(from) ? from : to), (r, from, to))
SD_VFS_PATH(_link_r, (struct _reent* r, const char* from, const char* to), (IsSdPath(from) ? from : to), (r, from, to))
SD_VFS_PATH(mkdir, (const char* path, mode_t mode), path, (path, mode))
SD_VFS_PATH(rmdir, (const char* path), path, (path))
SD_VFS_PATH(access, (const char* path, int mode), path, (path, mode))
SD_VFS_PATH(utime, (const char* path, const struct utimbuf* times), path, (path, times))
SD_VFS_PATH(truncate, (const char* path, off_t length), path, (path, length))
#undef SD_VFS_PATH
int __real__close_r(struct _reent*, int);
int __wrap__close_r(struct _reent* r, int fd) {
    Completion access;
    return __real__close_r(r, fd);
}
DIR* __real_opendir(const char*);
DIR* __wrap_opendir(const char* path) {
    if (!IsSdPath(path)) return __real_opendir(path);
    sd_access::Transaction access;
    if (!access) {
        errno = EBUSY;
        return nullptr;
    }
    return __real_opendir(path);
}
int __real_closedir(DIR*);
int __wrap_closedir(DIR* dir) {
    Completion access;
    return __real_closedir(dir);
}
esp_err_t __real_esp_vfs_fat_info(const char*, uint64_t*, uint64_t*);
esp_err_t __wrap_esp_vfs_fat_info(const char* path, uint64_t* total, uint64_t* free) {
    if (!IsSdPath(path)) return __real_esp_vfs_fat_info(path, total, free);
    sd_access::Transaction access;
    if (!access) return ESP_ERR_INVALID_STATE;
    return __real_esp_vfs_fat_info(path, total, free);
}
FRESULT __real_f_mount(FATFS*, const TCHAR*, BYTE);
FRESULT __wrap_f_mount(FATFS* fs, const TCHAR* path, BYTE option) {
    sd_access::Transition transition;
    if (!transition) return FR_TIMEOUT;
    sd_access::SetMounted(false);
    const auto result = __real_f_mount(fs, path, option);
    if (result == FR_OK && fs) sd_access::SetMounted(true);
    return result;
}
}
