#pragma once

#include "download_gate.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <memory>

namespace reader {

using HttpDownloadCancelledFn = bool (*)(void* user);

/** @brief 共享 HTTP 磁盘缓存，最后一个引用释放时删除文件 */
struct HttpTempFile {
    std::string path; // 本次缓存的独占路径
    ~HttpTempFile();
    HttpTempFile() = default;
    HttpTempFile(const HttpTempFile&) = delete;
    HttpTempFile& operator=(const HttpTempFile&) = delete;
};

/** @brief 分块下载到临时缓存，不在内存中保留完整响应 */
std::shared_ptr<HttpTempFile> DownloadHttpToCache(const char* url, std::string& err_out,
    DownloadGate* gate = nullptr, HttpDownloadCancelledFn cancelled = nullptr, void* context = nullptr);

using HttpDownloadProgressFn = void (*)(int percent, void* user);

/**
 * @brief HTTP 落盘下载（边收边写 SD；PSRAM 大块缓冲）
 * @param expected_sha256 空或 nullptr 则跳过比较；哈希值不一致仅警告，不阻止保存
 * @param expected_total 推送长度，仅用于进度与差异警告；HTTP Content-Length 用于长度校验并优先用于进度
 */
bool DownloadHttpToFile(const char* url, const char* tmp_path, const char* expected_sha256,
                        uint64_t expected_total,
                        HttpDownloadProgressFn on_progress, void* progress_user,
                        std::string& err_out, DownloadGate* gate,
                        HttpDownloadCancelledFn cancelled = nullptr, void* context = nullptr);

/** @brief 提交下载临时文件；覆盖失败时恢复原文件，未授权覆盖则拒绝重名目标 */
bool CommitDownloadedFile(const std::string& tmp_path, const std::string& final_path,
                          bool overwrite, std::string& err_out);

}  // namespace reader
