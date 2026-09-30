#pragma once

#include <atomic>

#include "reader_types.h"
#include "zip_reader.h"

namespace reader {

/**
 * @brief 从 ZIP 条目流式解码图片到 L8（JPEG/PNG；工作区 PSRAM）
 * @param abort 非空且为 true 时尽快中止（翻页作废）
 * @note  不整包装入内存；按 max_w×max_h 缩放出图。日志带 decode=stream。
 */
bool DecodeZipEntryImageToL8(ZipReader& zip, const char* entry_name, int max_w, int max_h,
                             RasterImage& out, const std::atomic<bool>* abort = nullptr);

}  // namespace reader
