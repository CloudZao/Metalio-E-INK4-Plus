#pragma once

#include <atomic>
#include "zip_reader.h"
#include "infra/image/image_decode.h"

/**
 * @brief 从 ZIP 条目流式解码 JPEG 或 PNG，保留受限缓冲回退及取消处理。
 * @note 负责条目的打开、重开和关闭，不接管 ZIP 文件生命周期。
 */
bool DecodeZipEntryImageToL8(ZipReader& zip, const char* entry_name, int max_w, int max_h, RasterImage& out,
                             const std::atomic<bool>* abort = nullptr);
