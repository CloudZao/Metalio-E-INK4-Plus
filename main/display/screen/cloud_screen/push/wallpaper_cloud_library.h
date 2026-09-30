#pragma once

#include "book_screen/book_cloud_library.h"

#include <cstdint>
#include <string>
#include <vector>

namespace reader {

struct CloudWallpaperTask {
    std::string task_id;
    std::string badge_image_id;
    std::string image_name;          // 列表展示
    std::string original_filename;   // 原始文件名（用于组成落盘名）
    std::string download_url;
    uint64_t file_size = 0;
    std::string sha256;

    /** @brief 本地落盘路径 */
    std::string LocalPath() const;
    /** @brief 本地是否已存在对应文件 */
    bool IsDownloaded() const;
};

/** @brief 列表标题：imageName_originalFilename */
std::string FormatWallpaperItemTitle(const std::string& image_name,
                                     const std::string& original_filename);
/** @brief 读取壁纸旁路元数据 */
bool ReadWallpaperMeta(const char* wallpaper_path, std::string& image_name_out,
                       std::string& original_filename_out);
/** @brief 写入壁纸旁路元数据 */
bool WriteWallpaperMeta(const char* wallpaper_path, const std::string& image_name,
                        const std::string& original_filename);
/** @brief 删除壁纸旁路元数据文件 */
void DeleteWallpaperMeta(const char* wallpaper_path);

/** @brief 下载云壁纸到本地 */
bool DownloadCloudWallpaper(const CloudWallpaperTask& task, std::string& err_out,
                            CloudDownloadProgressFn on_progress = nullptr,
                            void* progress_user = nullptr, DownloadGate* gate = nullptr);
/** @brief 删除本地云壁纸文件及元数据 */
bool DeleteCloudWallpaperLocal(const CloudWallpaperTask& task, std::string& err_out);

}  // namespace reader
