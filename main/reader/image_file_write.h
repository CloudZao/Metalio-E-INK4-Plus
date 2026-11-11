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
/** @brief 删除壁纸、元数据和保存备份并清理启用状态；须在非 LVGL 的内部 RAM 栈任务调用。 */
bool DeleteWallpaperFile(const char* path);
} // namespace reader
