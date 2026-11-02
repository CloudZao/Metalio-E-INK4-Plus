#include "sd_log_writer.h"
#include "sd_paths.h"
#include "settings.h"
#include "usb_virtual_disk.h"
#include "epd_flash_guard.h"
#include "sdkconfig.h"

#if CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS || CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS_GLOBAL
#include "metalio_i2c1.h"
#endif

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sys/stat.h>
#include <sys/time.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_core_dump.h>
#include <esp_partition.h>
#include <unistd.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

namespace {
constexpr size_t kRingCap = 4 * 1024;
constexpr size_t kMaxFile = 256 * 1024;
char s_ring[kRingCap];
size_t s_head = 0, s_tail = 0, s_used = 0;
portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;
std::mutex s_file_mu;
bool s_crash_checked = false;
bool s_crash_erase_pending = false; // 当前 SD 副本已持久化，等待日志任务安全擦除 Flash
size_t s_write_pos = 0;
bool s_ready = false; // all file operations/mount transitions share s_file_mu
std::atomic<bool> s_enabled{true};
std::atomic<bool> s_persist{false};
std::atomic<bool> s_started{false};
std::atomic<bool> s_clearing{false};
std::atomic<unsigned> s_dropped{0};
vprintf_like_t s_previous = nullptr;

// No logging, allocations, filesystem access, or blocking mutex in this hook.
int CaptureLog(const char* fmt, va_list args) {
    if (s_enabled.load(std::memory_order_relaxed) &&
        !s_clearing.load(std::memory_order_relaxed) && !xPortInIsrContext()) {
        char line[256];
        timeval tv{};
        gettimeofday(&tv, nullptr);
        tm time{};
        localtime_r(&tv.tv_sec, &time);
        int prefix = snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d.%03d ",
                              time.tm_year + 1900, time.tm_mon + 1, time.tm_mday,
                              time.tm_hour, time.tm_min, time.tm_sec, int(tv.tv_usec / 1000));
        if (prefix > 0 && static_cast<size_t>(prefix) < sizeof(line) - 2) {
            va_list copy;
            va_copy(copy, args);
            int n = vsnprintf(line + prefix, sizeof(line) - prefix, fmt, copy);
            va_end(copy);
            if (n > 0) {
                size_t len = strnlen(line, sizeof(line) - 2);
                if (!len || line[len - 1] != '\n') line[len++] = '\n';
                portENTER_CRITICAL(&s_ring_mux);
                if (s_clearing.load(std::memory_order_relaxed)) {
                    portEXIT_CRITICAL(&s_ring_mux);
                    return s_previous ? s_previous(fmt, args) : vprintf(fmt, args);
                }
                if (len > kRingCap - s_used) {
                    size_t drop = len - (kRingCap - s_used);
                    s_tail = (s_tail + drop) % kRingCap;
                    s_used -= drop;
                    s_dropped.fetch_add(drop, std::memory_order_relaxed);
                }
                for (size_t i = 0; i < len; ++i) {
                    s_ring[s_head] = line[i];
                    s_head = (s_head + 1) % kRingCap;
                }
                s_used += len;
                portEXIT_CRITICAL(&s_ring_mux);
            }
        }
    }
    return s_previous ? s_previous(fmt, args) : vprintf(fmt, args);
}

// Caller holds s_file_mu across ring drain and the entire fopen/write/close.
bool FlushLocked() {
    if (!s_ready) return false;
    portENTER_CRITICAL(&s_ring_mux);
    size_t remaining = s_used; // bounded batch; concurrent producers cannot prolong it
    portEXIT_CRITICAL(&s_ring_mux);
    if (!remaining) return true;
    if (!SdEnsureDir(SD_PATH_LOGS)) return false;
    struct stat st{};
    bool exists = stat(SD_PATH_RUNTIME_LOG, &st) == 0 && S_ISREG(st.st_mode);
    if (!exists) s_write_pos = 0;
    FILE* fp = fopen(SD_PATH_RUNTIME_LOG, exists ? "r+b" : "w+b");
    if (!fp) return false;
    bool ok = true;
    char chunk[512];
    while (remaining) {
        portENTER_CRITICAL(&s_ring_mux);
        size_t n = s_used < sizeof(chunk) ? s_used : sizeof(chunk);
        if (n > remaining) n = remaining;
        for (size_t i = 0; i < n; ++i) {
            chunk[i] = s_ring[s_tail];
            s_tail = (s_tail + 1) % kRingCap;
        }
        s_used -= n;
        portEXIT_CRITICAL(&s_ring_mux);
        if (!n) break;
        remaining -= n;
        size_t off = 0;
        while (off < n) {
            if (s_write_pos >= kMaxFile) s_write_pos = 0;
            size_t take = n - off < kMaxFile - s_write_pos ? n - off : kMaxFile - s_write_pos;
            if (fseek(fp, static_cast<long>(s_write_pos), SEEK_SET) != 0) { ok = false; break; }
            size_t written = fwrite(chunk + off, 1, take, fp);
            s_write_pos += written;
            off += written;
            if (written != take) { ok = false; break; }
        }
        if (!ok) { s_dropped.fetch_add(n - off, std::memory_order_relaxed); break; }
    }
    if (fflush(fp) != 0) ok = false;
    if (fclose(fp) != 0) ok = false;
    return ok;
}

// 调用方先持共享 Flash 门闩；只在 SDK 操作期间排空 I2C，不跨 SD I/O 持锁。
template <typename Operation>
esp_err_t WithCoredumpFlashOperation(Operation operation) {
    #if CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS || CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS_GLOBAL
    if (!metalio_i2c1_lock_for_flash(-1)) return ESP_FAIL;
    struct I2cGuard {
        ~I2cGuard() { metalio_i2c1_unlock(); }
    } i2c_guard;
    #endif
    return operation();
}

// 持 Flash 和文件锁发布同步关闭后的完整副本；擦除交给日志任务。
bool ExportCrashLocked() {
    #if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    if (!s_ready || s_crash_checked) return false;
    if (s_crash_erase_pending) return true;
    esp_err_t checked = WithCoredumpFlashOperation([]() { return esp_core_dump_image_check(); });
    if (checked != ESP_OK) {
        s_crash_checked = checked == ESP_ERR_NOT_FOUND || checked == ESP_ERR_INVALID_SIZE || checked == ESP_ERR_INVALID_CRC;
        return false;
    }
    size_t address = 0, size = 0;
    const auto* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                               ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
    if (!part || WithCoredumpFlashOperation([&]() { return esp_core_dump_image_get(&address, &size); }) != ESP_OK ||
        address < part->address || !size || size > part->size ||
        address - part->address > part->size - size || !SdEnsureDir(SD_PATH_LOGS)) return false;
    FILE* fp = fopen(SD_PATH_COREDUMP ".tmp", "wb");
    if (!fp) return false;
    bool ok = true;
    uint8_t chunk[1024];
    for (size_t off = 0; off < size; off += sizeof(chunk)) {
        size_t n = size - off < sizeof(chunk) ? size - off : sizeof(chunk);
        if (WithCoredumpFlashOperation([&]() {
                return esp_partition_read(part, address - part->address + off, chunk, n);
            }) != ESP_OK ||
            fwrite(chunk, 1, n, fp) != n) { ok = false; break; }
    }
    if (fflush(fp) != 0) ok = false;
    if (ok && fsync(fileno(fp)) != 0) ok = false;
    if (fclose(fp) != 0) ok = false;
    // FatFS rename cannot replace an existing destination. Keep its old image
    // until the new durable temporary file can be published; rollback on failure.
    bool backed_up = false;
    if (ok) {
        struct stat old{};
        if (stat(SD_PATH_COREDUMP, &old) == 0) {
            remove(SD_PATH_COREDUMP ".previous");
            backed_up = rename(SD_PATH_COREDUMP, SD_PATH_COREDUMP ".previous") == 0;
            if (!backed_up) ok = false;
        }
    }
    if (ok && rename(SD_PATH_COREDUMP ".tmp", SD_PATH_COREDUMP) != 0) {
        ok = false;
        if (backed_up) rename(SD_PATH_COREDUMP ".previous", SD_PATH_COREDUMP);
    }
    if (!ok) { remove(SD_PATH_COREDUMP ".tmp"); return false; }
    s_crash_erase_pending = true;
    ESP_LOGI("SdLog", "crash saved bytes=%u; flash erase pending", unsigned(size));
    return true;
    #else
    return false;
    #endif
}

bool ExportCrash() {
    #if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    {
        std::lock_guard<std::mutex> lock(s_file_mu);
        if (!s_ready || s_crash_checked) return false;
        if (s_crash_erase_pending) return true;
    }
    EpdFlashGuard flash_guard;
    std::lock_guard<std::mutex> lock(s_file_mu);
    return ExportCrashLocked();
    #else
    return ExportCrashLocked();
    #endif
}

void EraseExportedCrash() {
    #if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    esp_err_t erased;
    {
        EpdFlashGuard flash_guard;
        // 共享 Flash 门闩串行化 file/I2C 交叉；擦除期间阻止清空副本或切换 SD。
        std::lock_guard<std::mutex> lock(s_file_mu);
        if (!s_ready || !s_crash_erase_pending) return;
        erased = WithCoredumpFlashOperation([]() { return esp_core_dump_image_erase(); });
        if (erased == ESP_OK) {
            s_crash_erase_pending = false;
            s_crash_checked = true;
        }
    }
    ESP_LOGI("SdLog", "crash flash erase=%d", int(erased));
    #endif
}

void Writer(void*) {
    for (;;) {
        // NVS takes the Flash gate; acquire it before entering the file lock domain.
        if (s_persist.exchange(false)) {
            Settings settings("display", true);
            settings.SetBool("sd_log", s_enabled.load());
        }
        const bool erase_crash = ExportCrash();
        {
            std::lock_guard<std::mutex> lock(s_file_mu);
            if (s_enabled.load()) FlushLocked();
        }
        if (erase_crash) EraseExportedCrash();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
}

void SdLogWriter_Start() {
    bool expected = false;
    if (!s_started.compare_exchange_strong(expected, true)) return;
    {
        Settings settings("display", false);
        s_enabled.store(settings.GetBool("sd_log", true));
    }
    // 日志任务优先级高于缩略图解码，避免持有 s_file_mu 时延迟刷屏。
    if (xTaskCreatePinnedToCoreWithCaps(Writer, "sd_log", 4096, nullptr, 6, nullptr, 0,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        s_started.store(false);
        ESP_LOGE("SdLog", "writer task allocation failed");
        return;
    }
    s_previous = esp_log_set_vprintf(CaptureLog);
}

void SdLogWriter_LockForDisplayRefresh() { s_file_mu.lock(); }
void SdLogWriter_UnlockForDisplayRefresh() { s_file_mu.unlock(); }

bool SdLogWriter_IsEnabled() { return s_enabled.load(); }
void SdLogWriter_SetEnabled(bool enabled) {
    s_enabled.store(enabled);
    s_persist.store(true); // serial persistence avoids out-of-order NVS tasks
}
void SdLogWriter_OnSdMounted() {
    std::lock_guard<std::mutex> lock(s_file_mu);
    s_crash_erase_pending = false;
    // Keep logging suspended for the entire gadget lifetime, including APP mounts
    // caused by host eject. Resume only after the USB worker fully stops.
    auto& usb = UsbVirtualDisk::GetInstance();
    s_ready = !usb.IsGadgetActive() && !usb.IsBusy();
    if (s_ready) {
        struct stat st{};
        s_write_pos = stat(SD_PATH_RUNTIME_LOG, &st) == 0 && st.st_size > 0 &&
                      size_t(st.st_size) < kMaxFile ? size_t(st.st_size) : 0;
        remove(SD_PATH_RUNTIME_LOG ".previous"); // retire backup from earlier firmware
    }
}
void SdLogWriter_OnSdUnmounting() {
    std::lock_guard<std::mutex> lock(s_file_mu);
    // Waits for an in-progress flush/snapshot to close its FILE before VFS switches.
    s_ready = false;
    s_crash_erase_pending = false;
}

bool SdLogWriter_Clear() {
    // This lock also excludes flush, snapshot, crash export and USB unmount.
    std::lock_guard<std::mutex> lock(s_file_mu);
    auto& usb = UsbVirtualDisk::GetInstance();
    if (!s_ready || usb.IsGadgetActive() || usb.IsBusy()) return false;
    s_crash_erase_pending = false;
    s_clearing.store(true);
    portENTER_CRITICAL(&s_ring_mux);
    s_head = s_tail = s_used = 0;
    portEXIT_CRITICAL(&s_ring_mux);

    bool ok = SdEnsureDir("/sdcard/metalio") && SdEnsureDir(SD_APP_ROOT) && SdEnsureDir(SD_PATH_LOGS);
    if (ok) {
        FILE* file = fopen(SD_PATH_RUNTIME_LOG, "wb");
        if (file) {
            s_write_pos = 0;
            if (fclose(file) != 0) ok = false;
        } else {
            ok = false;
        }
        // Attempt every removal even if another file operation failed. Missing
        // files are already clear. Never touch the Flash coredump partition.
        for (const char* path : {SD_PATH_COREDUMP, SD_PATH_COREDUMP ".tmp",
                                 SD_PATH_COREDUMP ".previous", SD_PATH_RUNTIME_LOG ".previous"}) {
            if (unlink(path) != 0 && errno != ENOENT) ok = false;
        }
    }
    s_clearing.store(false);
    return ok;
}

SdLogSnapshot::~SdLogSnapshot() { heap_caps_free(data); }

SdLogSnapshotResult SdLogWriter_Snapshot(SdLogSnapshot& out) {
    std::lock_guard<std::mutex> lock(s_file_mu);
    if (!s_ready) return SdLogSnapshotResult::Unavailable;
    if (out.data) return SdLogSnapshotResult::IoError;
    if (!FlushLocked()) return SdLogSnapshotResult::IoError;
    struct stat st{};
    if (stat(SD_PATH_RUNTIME_LOG, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
        return SdLogSnapshotResult::NoFile;
    // Same physical file order as 397's single-file circular logger.
    size_t total = size_t(st.st_size) < kMaxFile ? size_t(st.st_size) : kMaxFile;
    auto* data = static_cast<uint8_t*>(heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!data) return SdLogSnapshotResult::NoMemory;
    FILE* fp = fopen(SD_PATH_RUNTIME_LOG, "rb");
    bool ok = fp && fread(data, 1, total, fp) == total && !ferror(fp);
    if (fp && fclose(fp) != 0) ok = false;
    if (!ok) {
        heap_caps_free(data);
        return SdLogSnapshotResult::IoError;
    }
    out.data = data;
    out.size = total;
    ESP_LOGI("SdLog", "snapshot bytes=%u ring_dropped_bytes=%u", static_cast<unsigned>(total), s_dropped.load());
    return SdLogSnapshotResult::Ok;
}

SdLogSnapshotResult SdLogWriter_CrashSnapshot(SdLogSnapshot& out) {
    ExportCrash();
    std::lock_guard<std::mutex> lock(s_file_mu);
    if (!s_ready) return SdLogSnapshotResult::Unavailable;
    if (out.data) return SdLogSnapshotResult::IoError;
    #if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    if (!s_crash_checked && !s_crash_erase_pending) return SdLogSnapshotResult::IoError;
    #endif
    struct stat st{};
    if (stat(SD_PATH_COREDUMP, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
        return SdLogSnapshotResult::NoFile;
    if (st.st_size > 64 * 1024) return SdLogSnapshotResult::IoError;
    auto* data = static_cast<uint8_t*>(heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!data) return SdLogSnapshotResult::NoMemory;
    FILE* fp = fopen(SD_PATH_COREDUMP, "rb");
    bool ok = fp && fread(data, 1, st.st_size, fp) == size_t(st.st_size) && !ferror(fp);
    if (fp && fclose(fp) != 0) ok = false;
    if (!ok) { heap_caps_free(data); return SdLogSnapshotResult::IoError; }
    out.data = data;
    out.size = st.st_size;
    return SdLogSnapshotResult::Ok;
}
