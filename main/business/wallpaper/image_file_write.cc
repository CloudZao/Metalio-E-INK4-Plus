#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <esp_log.h>
#include <sys/stat.h>
#include <unistd.h>
#include "image_file_write.h"
#include "infra/storage/sd_file_commit.h"
#include "boards/common/storage/sd_access.h"
#include "infra/storage/sd_data_layout.h"
#include "infra/storage/sd_paths.h"
#include "infra/storage/sd_work_file.h"
#include "business/wallpaper/wallpaper_active.h"

static std::mutex s_file_mutex;  // 保存、删除与保存工作文件回收共用

static const char* WallpaperName(const char* path, bool nested = false) {
    constexpr char prefix[] = SD_PATH_WALLPAPER "/";
    if (!path || std::strncmp(path, prefix, sizeof(prefix) - 1)) {
        return nullptr;
    }
    const char* name = path + sizeof(prefix) - 1;
    if (!nested) {
        return SdIsValidFileName(name) ? name : nullptr;
    }
    const std::string relative = name;
    size_t start = 0;
    do {
        const size_t end = relative.find('/', start);
        const auto part = relative.substr(start, end - start);
        if (part == "." || part == ".." || !SdIsValidFileName(part.c_str())) {
            return nullptr;
        }
        if (end == std::string::npos) {
            return name;
        }
        start = end + 1;
    } while (true);
}

static bool BackupPath(const char* name, char* path, size_t capacity) {
    const int count = std::snprintf(path, capacity, "%s/%s", SD_PATH_WALLPAPER_BACKUP, name);
    return count > 0 && size_t(count) < capacity;
}

static bool RemoveIfPresent(const char* path) {
    if (unlink(path) == 0 || errno == ENOENT) {
        return true;
    }
    ESP_LOGW("WallpaperWrite", "file cleanup failed errno=%d", errno);
    return false;
}

struct ImageFileWriteWorkFile {
    FILE* file = nullptr;                // 本次写入独占的文件
    char path[SD_FILE_PATH_CAPACITY]{};  // 空路径表示所有权已转移

    ~ImageFileWriteWorkFile() {
        if (file && std::fclose(file)) {
            ESP_LOGW("WallpaperWrite", "work close failed: ");
        }
        if (path[0] && unlink(path) && errno != ENOENT) {
            ESP_LOGW("WallpaperWrite", "work cleanup failed: ");
        }
    }

    bool Open() {
        const int fd = SdCreateWorkFile(SdWorkArea::Wallpaper, "ws-", path, sizeof(path));
        if (fd < 0) {
            return false;
        }
        file = fdopen(fd, "wb");
        if (!file) {
            close(fd);
        }
        return file != nullptr;
    }

    bool Close() {
        const bool flushed = std::fflush(file) == 0 && fsync(fileno(file)) == 0;
        const bool closed = std::fclose(file) == 0;
        file = nullptr;
        return flushed && closed;
    }
};

bool WallpaperWriteWallpaperFileAtomic(const char* destination, const uint8_t* data, size_t length, bool overwrite,
                                       ImageDecodeCancelled cancelled, void* context, ImageDecodeError* error) {
    auto fail = [error](ImageDecodeError reason) {
        if (error) {
            *error = reason;
        }
        return false;
    };
    if (error) {
        *error = ImageDecodeError::kNone;
    }
    const char* name = WallpaperName(destination);
    if (!name || !data || !length) {
        return fail(ImageDecodeError::kInvalidArgument);
    }
    if (cancelled && cancelled(context)) {
        return fail(ImageDecodeError::kCancelled);
    }
    std::lock_guard<std::mutex> lock(s_file_mutex);
    SdAccessTransaction transaction;
    if (!transaction || !SdEnsureDirectory(SD_PATH_WALLPAPER_TEMP)) {
        return fail(ImageDecodeError::kFileIo);
    }
    SdDataCleanWorkFiles(SD_PATH_WALLPAPER_TEMP, "ws-");
    ImageFileWriteWorkFile work;
    if (!work.Open()) {
        return fail(ImageDecodeError::kFileIo);
    }
    for (size_t offset = 0; offset < length;) {
        if (cancelled && cancelled(context)) {
            return fail(ImageDecodeError::kCancelled);
        }
        const size_t count = std::min<size_t>(16 * 1024, length - offset);
        if (std::fwrite(data + offset, 1, count, work.file) != count) {
            return fail(ImageDecodeError::kFileIo);
        }
        offset += count;
    }
    if (!work.Close()) {
        return fail(ImageDecodeError::kFileIo);
    }
    if (cancelled && cancelled(context)) {
        return fail(ImageDecodeError::kCancelled);
    }
    char backup_path[SD_FILE_PATH_CAPACITY];
    if (!BackupPath(name, backup_path, sizeof(backup_path)) || !SdEnsureDirectory(SD_PATH_WALLPAPER_BACKUP) ||
        SdCommitFile(work.path, destination, overwrite, backup_path) != SdCommitResult::Ok) {
        return fail(ImageDecodeError::kFileIo);
    }
    work.path[0] = '\0';
    return true;
}

bool WallpaperDeleteWallpaperFile(const char* path) {
    const char* name = WallpaperName(path, true);
    if (!name) {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_file_mutex);
    SdAccessTransaction transaction;
    if (!transaction) {
        return false;
    }
    SdDataCleanWorkFiles(SD_PATH_WALLPAPER_TEMP, "ws-");
    char temporary[SD_FILE_PATH_CAPACITY];
    if (!BackupPath(name, temporary, sizeof(temporary)) || !RemoveIfPresent(temporary)) {
        return false;
    }
    const int count = std::snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (count <= 0 || size_t(count) >= sizeof(temporary) || !RemoveIfPresent(temporary)) {
        return false;
    }
    if (!RemoveIfPresent(path)) {
        return false;
    }
    const bool clean = SdDataDeleteWallpaperData(path);
    if (!std::strchr(name, '/')) {
        Wallpaper_ClearActiveIfMatches(name);
    }
    ESP_LOGI("WallpaperWrite", "wallpaper delete done auxiliary_ok=%d", clean);
    return clean;
}
