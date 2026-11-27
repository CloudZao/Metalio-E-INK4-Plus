#pragma once
#include "reader_types.h"

/** @brief 从限额缓存复制匹配尺寸及文件版本的缩略图，未命中不读取 SD */
bool GetListThumbnail(const char* path, int width, int height, size_t bytes, int64_t mtime, RasterImage& out);

/** @brief 查询该文件版本是否已确认没有封面，不将临时读取失败视为无封面 */
bool ListThumbnailUnavailable(const char* path, size_t bytes, int64_t mtime);

/** @brief 缓存缩略图，空图表示已确认无封面；内存不足时淘汰或跳过 */
void CacheListThumbnail(const char* path, int width, int height, size_t bytes, int64_t mtime, const RasterImage& image);

/** @brief 离页、待机或文件变更时释放缩略图预取缓存 */
void ClearListThumbnailCache();
