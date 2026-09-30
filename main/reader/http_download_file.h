#pragma once

#include "download_gate.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace reader {

using HttpDownloadProgressFn = void (*)(int percent, void* user);

/**
 * @brief HTTP 落盘下载（边收边写 SD；PSRAM 大块缓冲）
 * @param expected_sha256 空或 nullptr 则跳过校验
 * @param expected_total 进度分母；若响应带 Content-Length 则优先用后者
 * @param max_bytes 超过则失败并删临时文件
 */
bool DownloadHttpToFile(const char* url, const char* tmp_path, const char* expected_sha256,
                        uint64_t expected_total, size_t max_bytes,
                        HttpDownloadProgressFn on_progress, void* progress_user,
                        std::string& err_out, DownloadGate* gate);

}  // namespace reader
