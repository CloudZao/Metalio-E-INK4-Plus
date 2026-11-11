#include "image_file_write.h"
#include "sd_access.h"
#include "sd_paths.h"
#include "sd_data_layout.h"
#include "wallpaper_screen/wallpaper_active.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <esp_log.h>
#include <esp_random.h>

namespace reader {
namespace {
std::mutex s_file_mutex; // 保存、删除与保存工作文件回收共用

const char* WallpaperName(const char* path, bool nested = false) {
    constexpr char prefix[] = SD_PATH_WALLPAPER "/";
    if (!path || std::strncmp(path, prefix, sizeof(prefix) - 1)) return nullptr;
    const char* name = path + sizeof(prefix) - 1;
    if (!nested) return SdIsValidFileName(name) ? name : nullptr;
    const std::string relative = name;
    size_t start = 0;
    do {
        const size_t end = relative.find('/', start);
        const auto part = relative.substr(start, end - start);
        if (part == "." || part == ".." || !SdIsValidFileName(part.c_str())) return nullptr;
        if (end == std::string::npos) return name;
        start = end + 1;
    } while (true);
}

bool BackupPath(const char* name, char* path, size_t capacity) {
    const int count = std::snprintf(path, capacity, "%s/backup/%s", SD_PATH_WALLPAPER_TEMP, name);
    return count > 0 && size_t(count) < capacity;
}

bool RemoveIfPresent(const char* path) {
    if (unlink(path) == 0 || errno == ENOENT) return true;
    ESP_LOGW("WallpaperWrite", "file cleanup failed errno=%d", errno);
    return false;
}

struct WorkFile {
    FILE* file = nullptr; // 本次写入独占的文件
    char path[SD_FILE_PATH_CAPACITY]{}; // 空路径表示所有权已转移
    ~WorkFile() {
        if (file && std::fclose(file)) ESP_LOGW("WallpaperWrite", "work close failed: %s", path);
        if (path[0] && unlink(path) && errno != ENOENT) ESP_LOGW("WallpaperWrite", "work cleanup failed: %s", path);
    }
    bool Open() {
        for (unsigned n = 0; n < 8; ++n) {
            std::snprintf(path, sizeof(path), "%s/ws-%08lx.tmp", SD_PATH_WALLPAPER_TEMP,
                          static_cast<unsigned long>(esp_random()));
            const int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
            if (fd < 0) { path[0] = '\0'; if (errno == EEXIST) continue; return false; }
            file = fdopen(fd, "wb");
            if (!file) close(fd);
            return file != nullptr;
        }
        return false;
    }
    bool Close() {
        const bool flushed = std::fflush(file) == 0;
        const bool closed = std::fclose(file) == 0;
        file = nullptr;
        return flushed && closed;
    }
};
} // namespace

bool WriteWallpaperFileAtomic(const char* destination, const uint8_t* data, size_t length,
                               bool overwrite, ImageDecodeCancelled cancelled, void* context,
                               ImageDecodeError* error) {
    auto fail = [error](ImageDecodeError reason) { if (error) *error = reason; return false; };
    if (error) *error = ImageDecodeError::kNone;
    const char* name = WallpaperName(destination);
    if (!name || !data || !length) return fail(ImageDecodeError::kInvalidArgument);
    if (cancelled && cancelled(context)) return fail(ImageDecodeError::kCancelled);
    std::lock_guard<std::mutex> lock(s_file_mutex);
    sd_access::Transaction transaction;
    if (!transaction || !SdEnsureDir(SD_PATH_TEMP) || !SdEnsureDir(SD_PATH_WALLPAPER_APP_TEMP) || !SdEnsureDir(SD_PATH_WALLPAPER_TEMP))
        return fail(ImageDecodeError::kFileIo);
    sd_data::CleanWorkFiles(SD_PATH_WALLPAPER_TEMP, "ws-");
    WorkFile work;
    if (!work.Open()) return fail(ImageDecodeError::kFileIo);
    for (size_t offset = 0; offset < length;) {
        if (cancelled && cancelled(context)) return fail(ImageDecodeError::kCancelled);
        const size_t count = std::min<size_t>(16 * 1024, length - offset);
        if (std::fwrite(data + offset, 1, count, work.file) != count) return fail(ImageDecodeError::kFileIo);
        offset += count;
    }
    if (!work.Close()) return fail(ImageDecodeError::kFileIo);
    if (cancelled && cancelled(context)) return fail(ImageDecodeError::kCancelled);
    struct stat info{};
    if (stat(destination, &info) != 0) {
        if (errno != ENOENT || rename(work.path, destination)) return fail(ImageDecodeError::kFileIo);
    } else {
        if (!overwrite || !S_ISREG(info.st_mode)) return fail(ImageDecodeError::kFileIo);
        char backup_path[SD_FILE_PATH_CAPACITY];
        if (!BackupPath(name, backup_path, sizeof(backup_path)) || !SdEnsureDir(SD_PATH_WALLPAPER_TEMP "/backup"))
            return fail(ImageDecodeError::kFileIo);
        if (stat(backup_path, &info) == 0 || errno != ENOENT) return fail(ImageDecodeError::kFileIo);
        WorkFile backup;
        if (rename(destination, backup_path)) return fail(ImageDecodeError::kFileIo);
        std::strcpy(backup.path, backup_path);
        if (rename(work.path, destination)) {
            if (rename(backup.path, destination)) {
                ESP_LOGE("WallpaperWrite", "restore failed; original retained: %s", backup.path);
                backup.path[0] = '\0';
            }
            return fail(ImageDecodeError::kFileIo);
        }
    }
    work.path[0] = '\0';
    return true;
}

bool DeleteWallpaperFile(const char* path) {
    const char* name = WallpaperName(path, true);
    if (!name) return false;
    std::lock_guard<std::mutex> lock(s_file_mutex);
    sd_access::Transaction transaction;
    if (!transaction) return false;
    sd_data::CleanWorkFiles(SD_PATH_WALLPAPER_TEMP, "ws-");
    char temporary[SD_FILE_PATH_CAPACITY];
    if (!BackupPath(name, temporary, sizeof(temporary)) || !RemoveIfPresent(temporary)) return false;
    const int count = std::snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (count <= 0 || size_t(count) >= sizeof(temporary) || !RemoveIfPresent(temporary)) return false;
    if (!RemoveIfPresent(path)) return false;
    const bool clean = sd_data::DeleteWallpaperData(path);
    if (!std::strchr(name, '/')) Wallpaper_ClearActiveIfMatches(name);
    ESP_LOGI("WallpaperWrite", "wallpaper delete done auxiliary_ok=%d", clean);
    return clean;
}
} // namespace reader
