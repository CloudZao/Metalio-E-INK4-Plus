#include "cloud_screen/push/wallpaper_cloud_library.h"
#include "cloud_screen/cloud_screen_priv.h"
#include "cloud_screen/push/push_resources_library.h"

#include "cloud_screen/push/wallpaper_cloud_library.h"
#include "reader/http_download_file.h"
#include "sd_paths.h"
#include "SdCardManager.hpp"
#include "wallpaper_screen/wallpaper_active.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <cJSON.h>
#include <esp_log.h>
#include <sys/stat.h>
#include <unistd.h>
#include "assets/lang_config.h"

namespace reader {

static constexpr const char* TAG = "WpCloud";
static constexpr size_t kMaxWallpaperDownloadBytes = 512 * 1024;

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
    return std::string(wallpaper_path) + ".meta.json";
}


std::string FormatWallpaperItemTitle(const std::string& image_name,
                                     const std::string& original_filename) {
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
    if (img == orig) {
        return img;
    }
    return img + "_" + orig;
}

bool ReadWallpaperMeta(const char* wallpaper_path, std::string& image_name_out,
                       std::string& original_filename_out) {
    image_name_out.clear();
    original_filename_out.clear();
    const std::string meta_path = MetaPathFor(wallpaper_path);
    if (meta_path.empty()) {
        return false;
    }
    FILE* fp = fopen(meta_path.c_str(), "rb");
    if (fp == nullptr) {
        return false;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    const long sz = ftell(fp);
    if (sz <= 0 || sz > 4096) {
        fclose(fp);
        return false;
    }
    std::rewind(fp);
    std::string body(static_cast<size_t>(sz), '\0');
    if (fread(body.data(), 1, body.size(), fp) != body.size()) {
        fclose(fp);
        return false;
    }
    fclose(fp);

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON* image_name = cJSON_GetObjectItemCaseSensitive(root, "imageName");
    const cJSON* original_filename =
        cJSON_GetObjectItemCaseSensitive(root, "originalFilename");
    if (cJSON_IsString(image_name) && image_name->valuestring != nullptr) {
        image_name_out = image_name->valuestring;
    }
    if (cJSON_IsString(original_filename) && original_filename->valuestring != nullptr) {
        original_filename_out = original_filename->valuestring;
    }
    cJSON_Delete(root);
    return !image_name_out.empty() || !original_filename_out.empty();
}

bool WriteWallpaperMeta(const char* wallpaper_path, const std::string& image_name,
                        const std::string& original_filename) {
    const std::string meta_path = MetaPathFor(wallpaper_path);
    if (meta_path.empty()) {
        return false;
    }
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return false;
    }
    if (!image_name.empty()) {
        cJSON_AddStringToObject(root, "imageName", image_name.c_str());
    }
    std::string orig = BasenameOfPath(original_filename);
    if (orig.empty()) {
        orig = original_filename;
    }
    if (!orig.empty()) {
        cJSON_AddStringToObject(root, "originalFilename", orig.c_str());
    }
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed == nullptr) {
        return false;
    }
    const std::string body(printed);
    cJSON_free(printed);

    FILE* fp = fopen(meta_path.c_str(), "wb");
    if (fp == nullptr) {
        return false;
    }
    const bool ok =
        fwrite(body.data(), 1, body.size(), fp) == body.size() && fflush(fp) == 0 &&
        fclose(fp) == 0;
    if (!ok) {
        fclose(fp);
        unlink(meta_path.c_str());
    }
    return ok;
}

void DeleteWallpaperMeta(const char* wallpaper_path) {
    const std::string meta_path = MetaPathFor(wallpaper_path);
    if (!meta_path.empty()) {
        unlink(meta_path.c_str());
    }
}

std::string BuildWallpaperSaveBasename(const std::string& image_name,
                                       const std::string& original_filename) {
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
    path.append(file);
    return path;
}

bool CloudWallpaperTask::IsDownloaded() const {
    struct stat st {};
    const std::string path = LocalPath();
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    return st.st_size > 0;
}

bool DownloadCloudWallpaper(const CloudWallpaperTask& task, std::string& err_out,
                            CloudDownloadProgressFn on_progress, void* progress_user,
                            DownloadGate* gate) {
    err_out.clear();
    if (gate != nullptr && gate->IsCancelled()) {
        err_out = Lang::Strings::CLOUD_CANCELLED;
        return false;
    }
    if (!SdCardManager::GetInstance().IsMounted()) {
        err_out = Lang::Strings::CLOUD_NO_SD;
        return false;
    }
    if (!SdEnsureAppLayout()) {
        err_out = Lang::Strings::CLOUD_STORAGE_UNAVAIL;
        return false;
    }
    if (task.download_url.empty()) {
        err_out = Lang::Strings::CLOUD_NO_DOWNLOAD_URL;
        return false;
    }

    const std::string final_path = task.LocalPath();
    const std::string wallpaper_prefix = std::string(SD_PATH_WALLPAPER) + "/";
    if (final_path.rfind(wallpaper_prefix, 0) != 0 && final_path != SD_PATH_WALLPAPER) {
        err_out = Lang::Strings::CLOUD_PATH_INVALID;
        return false;
    }
    const std::string tmp_path = final_path + ".tmp";

    if (!DownloadHttpToFile(task.download_url.c_str(), tmp_path.c_str(),
                            task.sha256.empty() ? nullptr : task.sha256.c_str(), task.file_size,
                            kMaxWallpaperDownloadBytes, on_progress, progress_user, err_out, gate)) {
        return false;
    }
    if (gate != nullptr && gate->IsCancelled()) {
        unlink(tmp_path.c_str());
        err_out = Lang::Strings::CLOUD_CANCELLED;
        return false;
    }

    unlink(final_path.c_str());
    if (rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        unlink(tmp_path.c_str());
        err_out = Lang::Strings::CLOUD_SAVE_FAIL;
        return false;
    }

    if (!WriteWallpaperMeta(final_path.c_str(), task.image_name, task.original_filename)) {
        ESP_LOGW(TAG, "write wallpaper meta failed path=%s", final_path.c_str());
    }

    ESP_LOGI(TAG, "saved cloud wallpaper -> %s", final_path.c_str());
    return true;
}

bool DeleteCloudWallpaperLocal(const CloudWallpaperTask& task, std::string& err_out) {
    err_out.clear();
    const std::string path = task.LocalPath();
    const std::string wallpaper_prefix = std::string(SD_PATH_WALLPAPER) + "/";
    if (path.rfind(wallpaper_prefix, 0) != 0) {
        err_out = Lang::Strings::CLOUD_PATH_INVALID;
        return false;
    }
    if (!task.IsDownloaded()) {
        err_out = Lang::Strings::CLOUD_NO_LOCAL_FILE;
        return false;
    }
    if (unlink(path.c_str()) != 0 && errno != ENOENT) {
        err_out = Lang::Strings::CLOUD_DELETE_FAIL;
        return false;
    }
    DeleteWallpaperMeta(path.c_str());
    unlink((path + ".tmp").c_str());
    const char* slash = std::strrchr(path.c_str(), '/');
    const char* base = slash != nullptr ? slash + 1 : path.c_str();
    Wallpaper_ClearActiveIfMatches(base);
    ESP_LOGI(TAG, "deleted local cloud wallpaper %s", path.c_str());
    return true;
}

}  // namespace reader
