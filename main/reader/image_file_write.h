#pragma once

#include "jpeg/progressive_jpeg.h"

namespace reader {
/**
 * @brief 完整写入壁纸后再提交目标，失败或取消时保留原文件
 * @param overwrite false 时拒绝已有目标；恢复失败的原文件保留在临时目录的 backup 子目录
 */
bool WriteWallpaperFileAtomic(const char* destination, const uint8_t* data, size_t length,
                               bool overwrite, ImageDecodeCancelled cancelled, void* context,
                               ImageDecodeError* error);
/** @brief 删除壁纸及对应下载临时文件、恢复备份，并回收废弃保存工作文件 */
bool DeleteWallpaperFile(const char* path);
} // namespace reader
