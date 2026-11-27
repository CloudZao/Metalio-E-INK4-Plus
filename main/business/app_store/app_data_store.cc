#include <cerrno>
#include <cstdio>
#include <string>
#include <dirent.h>
#include <esp_log.h>
#include <sys/stat.h>
#include <unistd.h>
#include "business/app_store/app_data_store.h"
#include "business/app_store/app_api.h"
#include "boards/common/storage/sd_access.h"
#include "infra/storage/sd_file_commit.h"
#include "infra/storage/sd_paths.h"

static constexpr const char* TAG = "AppData";
static constexpr uint32_t kDataLimit = 16 * 1024;
static constexpr unsigned kKeyLimit = 32;
static const std::string kDataRoot = SD_PATH_APP_STATE;

static bool ValidKey(const char* key) {
    if (!key || !key[0]) {
        return false;
    }
    unsigned length = 0;
    for (; key[length]; ++length) {
        if (length >= 48) {
            return false;
        }
        const char c = key[length];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static int IoError(const char* operation, const char* stage) {
    ESP_LOGE(TAG, "operation=%s stage=%s result=failed errno=%d", operation, stage, errno);
    return XZ_APP_ERR_IO;
}

static bool Directory(const std::string& path) {
    if (mkdir(path.c_str(), 0755) == 0) {
        return true;
    }
    struct stat st{};
    return errno == EEXIST && stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static std::string DataDirectory(const std::string& storage_key) {
    return kDataRoot + "/" + storage_key;
}

static std::string DataPath(const std::string& storage_key, const char* key) {
    return DataDirectory(storage_key) + "/" + key + ".bin";
}

static std::string WorkDirectory(const std::string& storage_key) {
    return std::string(SD_PATH_APP_STORE_TEMP) + "/" + storage_key;
}

static std::string PendingPath(const std::string& storage_key, const char* key) {
    return WorkDirectory(storage_key) + "/data-" + key + ".bin.part";
}

static void CleanWorkDirectory(const std::string& storage_key) {
    if (rmdir(WorkDirectory(storage_key).c_str()) != 0 && errno != ENOENT && errno != ENOTEMPTY && errno != EEXIST) {
        ESP_LOGW(TAG, "operation=data_cleanup stage=work_directory result=failed errno=%d", errno);
    }
}

static void CleanPending(const std::string& storage_key, const std::string& pending) {
    if (unlink(pending.c_str()) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "operation=data_write stage=pending_cleanup result=failed errno=%d", errno);
    }
    CleanWorkDirectory(storage_key);
}

int AppReadData(const std::string& storage_key, const char* key, void* buffer, uint32_t capacity) {
    if (storage_key.empty() || !ValidKey(key)) {
        return XZ_APP_ERR_INVALID;
    }
    SdAccessTransaction transaction;
    if (!transaction) {
        ESP_LOGW(TAG, "operation=data_read stage=sd_access result=unavailable");
        return XZ_APP_ERR_NOT_READY;
    }
    const auto path = DataPath(storage_key, key);
    FILE* file = fopen(path.c_str(), "rb");
    if (!file && errno == ENOENT) {
        file = fopen((path + ".cloud-backup").c_str(), "rb");
    }
    if (!file) {
        return errno == ENOENT ? XZ_APP_ERR_NOT_FOUND : IoError("data_read", "open");
    }
    int result = XZ_APP_ERR_IO;
    struct stat st{};
    if (fstat(fileno(file), &st) != 0) {
        result = IoError("data_read", "stat");
    } else if (!S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > kDataLimit) {
        ESP_LOGE(TAG, "operation=data_read stage=size result=invalid");
        result = XZ_APP_ERR_LIMIT;
    } else if (!buffer) {
        result = static_cast<int>(st.st_size);
    } else if (capacity < static_cast<uint32_t>(st.st_size)) {
        result = XZ_APP_ERR_LIMIT;
    } else if (fread(buffer, 1, st.st_size, file) != static_cast<size_t>(st.st_size)) {
        result = IoError("data_read", "read");
    } else {
        result = static_cast<int>(st.st_size);
    }
    if (fclose(file) != 0 && result >= 0) {
        result = IoError("data_read", "close");
    }
    return result;
}

static int CountKeys(const std::string& directory) {
    DIR* dir = opendir(directory.c_str());
    if (!dir) {
        return IoError("data_write", "open_directory");
    }
    int count = 0;
    while (auto* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".bin") == 0) {
            ++count;
        } else if (name.size() > 17 && name.compare(name.size() - 17, 17, ".bin.cloud-backup") == 0) {
            struct stat st{};
            if (stat((directory + "/" + name.substr(0, name.size() - 13)).c_str(), &st) != 0) {
                if (errno != ENOENT) {
                    const int result = IoError("data_write", "count_stat");
                    closedir(dir);
                    return result;
                }
                ++count;
            }
        }
    }
    closedir(dir);
    return count;
}

int AppWriteData(const std::string& storage_key, const char* key, const void* data, uint32_t length) {
    if (storage_key.empty() || !ValidKey(key) || (!data && length)) {
        return XZ_APP_ERR_INVALID;
    }
    if (length > kDataLimit) {
        return XZ_APP_ERR_LIMIT;
    }
    SdAccessTransaction transaction;
    if (!transaction) {
        ESP_LOGW(TAG, "operation=data_write stage=sd_access result=unavailable");
        return XZ_APP_ERR_NOT_READY;
    }
    const std::string directory = DataDirectory(storage_key);
    if (!Directory(SD_PATH_SETTINGS) || !Directory(kDataRoot) || !Directory(directory)) {
        return IoError("data_write", "directory");
    }
    const auto path = DataPath(storage_key, key);
    const auto backup = path + ".cloud-backup";
    struct stat st{};
    if (stat(backup.c_str(), &st) == 0) {
        if (stat(path.c_str(), &st) == 0) {
            if (unlink(backup.c_str()) != 0) {
                return IoError("data_write", "clean_backup");
            }
        } else if (errno != ENOENT || rename(backup.c_str(), path.c_str()) != 0) {
            return IoError("data_write", "restore_backup");
        }
    } else if (errno != ENOENT) {
        return IoError("data_write", "stat_backup");
    }
    if (stat(path.c_str(), &st) != 0) {
        if (errno != ENOENT) {
            return IoError("data_write", "stat");
        }
        const int count = CountKeys(directory);
        if (count < 0) {
            return count;
        }
        if (count >= static_cast<int>(kKeyLimit)) {
            return XZ_APP_ERR_LIMIT;
        }
    }
    if (!Directory(SD_PATH_TEMP) || !Directory(SD_PATH_APP_STORE_TEMP) || !Directory(WorkDirectory(storage_key))) {
        return IoError("data_write", "work_directory");
    }
    const auto pending = PendingPath(storage_key, key);
    FILE* file = fopen(pending.c_str(), "wb");
    if (!file) {
        const int result = IoError("data_write", "open");
        CleanPending(storage_key, pending);
        return result;
    }
    bool ok = !length || fwrite(data, 1, length, file) == length;
    int failure = ok ? 0 : errno;
    if (fflush(file) != 0 && ok) {
        ok = false;
        failure = errno;
    }
    if (ok && fsync(fileno(file)) != 0) {
        ok = false;
        failure = errno;
    }
    if (fclose(file) != 0 && ok) {
        ok = false;
        failure = errno;
    }
    if (!ok) {
        ESP_LOGE(TAG, "operation=data_write stage=write result=failed errno=%d", failure);
        CleanPending(storage_key, pending);
        return XZ_APP_ERR_IO;
    }
    if (!(SdCommitFile(pending, path, true) == SdCommitResult::Ok)) {
        const int result = IoError("data_write", "commit");
        CleanPending(storage_key, pending);
        return result;
    }
    CleanWorkDirectory(storage_key);
    ESP_LOGD(TAG, "operation=data_write result=success bytes=%lu", (unsigned long)length);
    return XZ_APP_OK;
}

int AppRemoveData(const std::string& storage_key, const char* key) {
    if (storage_key.empty() || !ValidKey(key)) {
        return XZ_APP_ERR_INVALID;
    }
    SdAccessTransaction transaction;
    if (!transaction) {
        ESP_LOGW(TAG, "operation=data_remove stage=sd_access result=unavailable");
        return XZ_APP_ERR_NOT_READY;
    }
    const auto path = DataPath(storage_key, key);
    bool found = false;
    for (const auto& suffix : {".cloud-backup", ""}) {
        if (unlink((path + suffix).c_str()) == 0) {
            found = true;
        } else if (errno != ENOENT) {
            return IoError("data_remove", "unlink");
        }
    }
    const auto pending = PendingPath(storage_key, key);
    if (unlink(pending.c_str()) == 0) {
        found = true;
    } else if (errno != ENOENT) {
        return IoError("data_remove", "pending_unlink");
    }
    CleanWorkDirectory(storage_key);
    return found ? XZ_APP_OK : XZ_APP_ERR_NOT_FOUND;
}


std::string AppStorageKey(const AppStoreItem& item) {
    return ValidKey(item.app_id.c_str()) && item.installed_name.size() <= 72 &&
                                SdIsValidFileName(item.installed_name.c_str()) && item.installed_name.front() != '.'
                            ? item.installed_name + "--" + item.app_id
                            : "";
}
