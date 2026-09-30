#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <zlib.h>

namespace reader {

// 轻量 ZIP 读取器（store / deflate），面向 EPUB。
// 不预加载整包；按需查找 central directory 条目并 inflate / 流式读取。
class ZipReader {
public:
    ZipReader() = default;
    ~ZipReader() { Close(); }

    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    bool Open(const char* path);
    void Close();
    /** @brief 已解析过中央目录（句柄可暂卸，见 DetachFile） */
    bool IsOpen() const { return cd_loaded_ && !path_.empty(); }

    /** @brief 浅睡卸 SD 前关 FILE*，保留条目表 */
    void DetachFile();
    /** @brief SD 重挂后按 path_ 再 fopen */
    bool ReattachFile();

    // 整包解压（HTML/OPF 等小文件）；图片请用 OpenEntryStream。
    bool ReadEntry(const char* name, std::vector<uint8_t>& out, size_t max_bytes = 8 * 1024 * 1024);

    bool EntryExists(const char* name);
    bool GetUncompressedSize(const char* name, size_t* size);

    void ListEntries(std::vector<std::string>& names);

    /**
     * @brief 打开条目流式读取（解压后字节流）；同时仅允许一个流。
     * @note 工作区在 PSRAM；不把整条目装入内存。
     */
    bool OpenEntryStream(const char* name);
    /** @brief 读出解压后数据；返回实际字节数，0 表示 EOF 或错误（结合 StreamEof/StreamError） */
    size_t EntryStreamRead(uint8_t* dst, size_t n);
    bool EntryStreamEof() const { return stream_eof_; }
    bool EntryStreamError() const { return stream_error_; }
    uint32_t EntryStreamUncompSize() const { return stream_uncomp_size_; }
    void CloseEntryStream();

private:
    struct Entry {
        uint16_t method = 0;
        uint32_t comp_size = 0;
        uint32_t uncomp_size = 0;
        uint32_t local_header_off = 0;
    };

    bool LoadCentralDirectory();
    bool FindEntry(const char* name, Entry* out);
    bool InflateEntry(const Entry& e, std::vector<uint8_t>& out, size_t max_bytes);
    bool SeekEntryPayload(const Entry& e, uint32_t* comp_remain);
    void ReleaseStreamLock();

    FILE* fp_ = nullptr;
    std::string path_;
    std::unordered_map<std::string, Entry> entries_;
    bool cd_loaded_ = false;

    // 整包读与流式互斥（流式持锁直至 CloseEntryStream）
    std::mutex io_mutex_;
    bool stream_holding_mutex_ = false;

    // 流式状态
    bool stream_open_ = false;
    bool stream_eof_ = false;
    bool stream_error_ = false;
    bool stream_store_ = false;
    uint32_t stream_comp_remain_ = 0;
    uint32_t stream_uncomp_size_ = 0;
    z_stream stream_z_{};
    bool stream_z_init_ = false;
    uint8_t* stream_inbuf_ = nullptr;
    size_t stream_inbuf_cap_ = 0;
    size_t stream_inbuf_len_ = 0;
    size_t stream_inbuf_pos_ = 0;
};

}  // namespace reader
