#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <cJSON.h>
#include <esp_log.h>
#include <sys/stat.h>
#include <unistd.h>
#include "infra/storage/file_write.h"
#include "business/cloud/wallpaper_cloud_library.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "infra/storage/sd_file_commit.h"
#include "boards/common/storage/SdCardManager.hpp"
#include "infra/storage/sd_data_layout.h"
#include "infra/storage/sd_paths.h"
#include "business/cloud/push_resources_library.h"
#include "infra/image/image_decode.h"
#include "business/wallpaper/image_file_write.h"
#include "infra/network/http/http_download_file.h"

static constexpr const char* TAG = "WpCloud";

static std::string SanitizeFilename(std::string file) {
    if (!file.empty()) {
        const size_t slash = file.find_last_of("/\\");
        if (slash != std::string::npos) {
            file = file.substr(slash + 1);
        }
    }
    if (file.empty() || file == "." || file == "..") {
        return {};
    }
    return file;
}

std::string BasenameOfPath(const std::string& path) {
    return SanitizeFilename(path);
}

std::string StemWithoutExt(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot == 0 || dot == std::string::npos) {
        return name;
    }
    return name.substr(0, dot);
}

std::string MetaPathFor(const char* wallpaper_path) {
    if (wallpaper_path == nullptr || wallpaper_path[0] == '\0') {
        return {};
    }
    return SdDataWallpaperMetaPath(wallpaper_path);
}

std::string BuildWallpaperSaveBasename(const std::string& image_name, const std::string& original_filename) {
    std::string file = FormatWallpaperItemTitle(image_name, original_filename);
    file = SanitizeFilename(file);
    for (char& c : file) {
        if (c == '/' || c == '\\' || c == ':') {
            c = '_';
        }
    }
    return file;
}

std::string CloudWallpaperTask::LocalPath() const {
    std::string path = SD_PATH_WALLPAPER;
    if (!path.empty() && path.back() != '/') {
        path.push_back('/');
    }
    std::string file = BuildWallpaperSaveBasename(image_name, original_filename);
    if (file.empty()) {
        if (!badge_image_id.empty()) {
            file = "badge-" + badge_image_id + ".a2i1";
        } else if (!task_id.empty()) {
            file = "badge-task-" + task_id + ".a2i1";
        } else {
            file = "wallpaper-unnamed.a2i1";
        }
    }
    if (!SdIsValidFileName(file.c_str())) {
        return {};
    }
    path.append(file);
    return path;
}

bool CloudWallpaperTask::IsDownloaded() const {
    struct stat st{};
    const std::string path = LocalPath();
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    return st.st_size > 0;
}

std::string FormatWallpaperItemTitle(const std::string& image_name, const std::string& original_filename) {
    std::string orig = BasenameOfPath(original_filename);
    std::string img = image_name;
    if (img.empty() && !orig.empty()) {
        img = StemWithoutExt(orig);
    }
    if (orig.empty() && !img.empty()) {
        orig = img;
    }
    if (img.empty()) {
        return orig;
    }
    if (orig.empty()) {
        return img;
    }
    if (img == orig || img == StemWithoutExt(orig)) {
        return img;
    }
    return img + "_" + orig;
}

bool ReadWallpaperMeta(const char* wallpaper_path, std::string& image_name_out,
                       std::string& original_filename_out) try {
    image_name_out.clear();
    original_filename_out.clear();
    const std::string meta_path = MetaPathFor(wallpaper_path);
    if (meta_path.empty()) {
        return false;
    }
    std::unique_ptr<FILE, decltype(&fclose)> file(fopen(meta_path.c_str(), "rb"), fclose);
    if (!file) {
        if (errno != ENOENT) {
            ESP_LOGW(TAG, "metadata read failed stage=open errno=%d", errno);
        }
        return false;
    }
    if (fseek(file.get(), 0, SEEK_END) != 0) {
        ESP_LOGW(TAG, "metadata read failed stage=seek errno=%d", errno);
        return false;
    }
    const long size = ftell(file.get());
    if (size <= 0 || size > 4096) {
        return false;
    }
    std::rewind(file.get());
    std::string body(static_cast<size_t>(size), '\0');
    if (fread(body.data(), 1, body.size(), file.get()) != body.size()) {
        ESP_LOGW(TAG, "metadata read failed stage=read errno=%d", errno);
        return false;
    }
    file.reset();
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(body.c_str()), cJSON_Delete);
    if (!cJSON_IsObject(root.get())) {
        return false;
    }
    const auto* image_name = cJSON_GetObjectItemCaseSensitive(root.get(), "imageName");
    const auto* original_filename = cJSON_GetObjectItemCaseSensitive(root.get(), "originalFilename");
    if (cJSON_IsString(image_name) && image_name->valuestring != nullptr) {
        image_name_out = image_name->valuestring;
    }
    if (cJSON_IsString(original_filename) && original_filename->valuestring != nullptr) {
        original_filename_out = original_filename->valuestring;
    }
    return !image_name_out.empty() || !original_filename_out.empty();
} catch (const std::bad_alloc&) {
    image_name_out.clear();
    original_filename_out.clear();
    ESP_LOGW(TAG, "metadata read failed stage=allocate; using filename");
    return false;
}

bool WriteWallpaperMeta(const char* wallpaper_path, const std::string& image_name,
                        const std::string& original_filename) try {
    const std::string meta_path = MetaPathFor(wallpaper_path);
    if (meta_path.empty()) {
        return false;
    }
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_CreateObject(), cJSON_Delete);
    if (!root || (!image_name.empty() && !cJSON_AddStringToObject(root.get(), "imageName", image_name.c_str()))) {
        ESP_LOGE(TAG, "metadata write failed stage=create_json");
        return false;
    }
    std::string original = BasenameOfPath(original_filename);
    if (original.empty()) {
        original = original_filename;
    }
    if (!original.empty() && !cJSON_AddStringToObject(root.get(), "originalFilename", original.c_str())) {
        ESP_LOGE(TAG, "metadata write failed stage=add_filename");
        return false;
    }
    std::unique_ptr<char, decltype(&cJSON_free)> printed(cJSON_PrintUnformatted(root.get()), cJSON_free);
    if (!printed) {
        ESP_LOGE(TAG, "metadata write failed stage=serialize");
        return false;
    }
    return WriteFileAtomic(meta_path.c_str(), reinterpret_cast<const uint8_t*>(printed.get()),
                           std::strlen(printed.get()));
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "metadata write failed stage=allocate");
    return false;
}

bool DownloadCloudWallpaper(const CloudWallpaperTask& task, std::string& err_out, CloudDownloadProgressFn on_progress,
                            void* progress_user, DownloadGate* gate, const char* save_path, bool overwrite) {
    err_out.clear();
    if (gate != nullptr && gate->IsCancelled()) {
        err_out = LangString_CLOUD_CANCELLED;
        return false;
    }
    if (!SdCardManager::GetInstance().IsMounted()) {
        err_out = LangString_CLOUD_NO_SD;
        return false;
    }
    if (!SdEnsureAppLayout()) {
        err_out = LangString_CLOUD_STORAGE_UNAVAIL;
        return false;
    }
    if (task.download_url.empty()) {
        err_out = LangString_CLOUD_NO_DOWNLOAD_URL;
        return false;
    }

    const std::string final_path = save_path != nullptr ? save_path : task.LocalPath();
    const std::string wallpaper_prefix = std::string(SD_PATH_WALLPAPER) + "/";
    if (final_path.rfind(wallpaper_prefix, 0) != 0 && final_path != SD_PATH_WALLPAPER) {
        err_out = LangString_CLOUD_PATH_INVALID;
        return false;
    }
    const std::string tmp_path = final_path + ".tmp";

    if (!DownloadHttpToFile(task.download_url.c_str(), tmp_path.c_str(),
                            task.sha256.empty() ? nullptr : task.sha256.c_str(), task.file_size, on_progress,
                            progress_user, err_out, gate)) {
        return false;
    }
    if (gate != nullptr && gate->IsCancelled()) {
        unlink(tmp_path.c_str());
        err_out = LangString_CLOUD_CANCELLED;
        return false;
    }

    const auto commit = SdCommitFile(tmp_path, final_path, overwrite);
    if (commit != SdCommitResult::Ok) {
        err_out = commit == SdCommitResult::Conflict ? LangString_CLOUD_FILE_EXISTS : LangString_CLOUD_SAVE_FAIL;
        unlink(tmp_path.c_str());
        return false;
    }

    // 元数据保存失败时使用文件名，不改变已保存图片的成功结果。
    WriteWallpaperMeta(final_path.c_str(), task.image_name, task.original_filename);

    ESP_LOGI(TAG, "operation=save_wallpaper result=success");
    return true;
}

bool DeleteCloudWallpaperLocal(const CloudWallpaperTask& task, std::string& err_out) {
    err_out.clear();
    const std::string path = task.LocalPath();
    const std::string wallpaper_prefix = std::string(SD_PATH_WALLPAPER) + "/";
    if (path.rfind(wallpaper_prefix, 0) != 0) {
        err_out = LangString_CLOUD_PATH_INVALID;
        return false;
    }
    if (!task.IsDownloaded()) {
        err_out = LangString_CLOUD_NO_LOCAL_FILE;
        return false;
    }
    if (!WallpaperDeleteWallpaperFile(path.c_str())) {
        err_out = LangString_CLOUD_DELETE_FAIL;
        return false;
    }
    return true;
}
