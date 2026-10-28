#include "wifi_config_storage.h"

#include "SdCardManager.hpp"
#include "sd_access.h"
#include "ssid_manager.h"
#include "usb_virtual_disk.h"

#include <cerrno>
#include <cstdio>
#include <unistd.h>
#include <esp_log.h>

#define TAG "WifiConfigStorage"

namespace {
constexpr const char* kTemp = SD_PATH_WIFI_CONFIG ".tmp";
constexpr const char* kBackup = SD_PATH_WIFI_CONFIG ".bak";
constexpr size_t kMaxBytes = 8192;

bool Fail(const char* direction, const char* stage) {
    ESP_LOGE(TAG, "%s failed: %s errno=%d", direction, stage, errno);
    return false;
}

bool CanAccess(const char* direction, bool admitted) {
    const auto& usb = UsbVirtualDisk::GetInstance();
    const char* reason = nullptr;
    if (!admitted) reason = "SD quiescing";
    else if (usb.IsBusy()) reason = "USB switching";
    else if (usb.IsGadgetActive()) reason = "USB storage enabled";
    else if (!SdCardManager::GetInstance().IsMounted()) reason = "SD not mounted";
    if (!reason) return true;
    ESP_LOGW(TAG, "%s deferred: %s", direction, reason);
    return false;
}

SsidManager::ReadResult Read(std::string& text) {
    using Result = SsidManager::ReadResult;
    sd_access::Transaction transaction;
    if (!CanAccess("SD->NVS", static_cast<bool>(transaction))) return Result::Error;
    struct stat st;
    if (stat(SD_PATH_WIFI_CONFIG, &st) != 0) {
        if (errno != ENOENT) {
            Fail("SD->NVS", "stat");
            return Result::Error;
        }
        if (stat(kBackup, &st) == 0) {
            if (!S_ISREG(st.st_mode)) {
                ESP_LOGE(TAG, "SD config backup invalid: not_regular_file");
                return Result::Error;
            }
            // 恢复在 FAT 两次 rename 之间断电留下的旧文件。
            if (rename(kBackup, SD_PATH_WIFI_CONFIG) != 0) {
                Fail("SD->NVS", "recover_backup");
                return Result::Error;
            }
            ESP_LOGW(TAG, "SD->NVS recovered interrupted replacement");
        } else {
            if (errno == ENOENT) return Result::Missing;
            Fail("SD->NVS", "stat_backup");
            return Result::Error;
        }
    }
    if (!S_ISREG(st.st_mode)) {
        ESP_LOGE(TAG, "SD config invalid: not_regular_file");
        return Result::Error;
    }
    if (st.st_size <= 0 || st.st_size > kMaxBytes) {
        ESP_LOGE(TAG, "SD config invalid: file_size");
        return Result::Invalid;
    }
    FILE* file = fopen(SD_PATH_WIFI_CONFIG, "rb");
    if (!file) {
        Fail("SD->NVS", "open");
        return Result::Error;
    }
    text.resize(static_cast<size_t>(st.st_size));
    const size_t count = fread(&text[0], 1, text.size(), file);
    const bool complete = count == text.size() && fgetc(file) == EOF && !ferror(file);
    const int read_errno = errno;
    const int closed = fclose(file);
    if (!complete) {
        errno = read_errno;
        Fail("SD->NVS", "read_or_size_changed");
        return Result::Error;
    }
    if (closed != 0) {
        Fail("SD->NVS", "close");
        return Result::Error;
    }
    return Result::Ok;
}

bool Write(const std::string& text) {
    sd_access::Transaction transaction;
    if (!CanAccess("NVS->SD", static_cast<bool>(transaction))) return false;
    if (!SdEnsureDir("/sdcard/metalio") || !SdEnsureDir(SD_APP_ROOT) ||
        !SdEnsureDir(SD_PATH_SETTINGS)) return Fail("NVS->SD", "create_directory");
    struct stat st;
    bool exists = stat(SD_PATH_WIFI_CONFIG, &st) == 0;
    if (!exists && errno != ENOENT) return Fail("NVS->SD", "stat");
    if (exists && !S_ISREG(st.st_mode)) {
        ESP_LOGE(TAG, "NVS->SD failed: destination_not_regular_file");
        return false;
    }
    FILE* file = fopen(kTemp, "wb");
    if (!file) return Fail("NVS->SD", "open_temp");
    const char* stage = nullptr;
    if (fwrite(text.data(), 1, text.size(), file) != text.size()) stage = "write_temp";
    else if (fflush(file) != 0) stage = "flush_temp";
    else if (fsync(fileno(file)) != 0) stage = "sync_temp";
    const int write_errno = errno;
    const int closed = fclose(file);
    if (stage) {
        errno = write_errno;
        return Fail("NVS->SD", stage);
    }
    if (closed != 0) return Fail("NVS->SD", "close_temp");
    if (exists) {
        if (unlink(kBackup) != 0 && errno != ENOENT) return Fail("NVS->SD", "remove_old_backup");
        if (rename(SD_PATH_WIFI_CONFIG, kBackup) != 0) return Fail("NVS->SD", "backup_previous");
    }
    if (rename(kTemp, SD_PATH_WIFI_CONFIG) != 0) {
        Fail("NVS->SD", "install_temp");
        if (exists && rename(kBackup, SD_PATH_WIFI_CONFIG) != 0) Fail("NVS->SD", "restore_backup");
        return false;
    }
    if (unlink(kBackup) != 0 && errno != ENOENT) return Fail("NVS->SD", "remove_backup");
    return true;
}
}

void WifiConfigStorage_Init() {
    SsidManager::GetInstance().InitializeStorage(Read, Write);
}
