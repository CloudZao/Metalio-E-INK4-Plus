#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <zlib.h>
#include "book_read_error.h"

// 轻量 ZIP 读取器（store / deflate），面向 EPUB。
// 不预加载整包；按需查找 central directory 条目并 inflate / 流式读取。
class ZipReader {
public:
    /** @brief 创建尚未打开归档的读取器 */
    ZipReader() = default;

    /** @brief 关闭条目流和归档文件，释放条目索引 */
    ~ZipReader() {
        Close();
    }

    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    /** @brief 关闭旧归档并加载指定 ZIP 的中央目录，失败返回 false */
    bool Open(const char* path);

    /** @brief 最近一次源文件或解压读取错误。 */
    BookReadError Error() const {
        return fp_ && std::ferror(fp_) ? BookReadError::kFileIo : error_;
    }

    /** @brief 关闭条目流和文件句柄，清空归档路径与目录索引 */
    void Close();

    /** @brief 已解析过中央目录（句柄可暂卸，见 DetachFile） */
    bool IsOpen() const {
        return cd_loaded_ && !path_.empty();
    }

    /** @brief 浅睡卸 SD 前关 FILE*，保留条目表 */
    void DetachFile();

    /** @brief SD 重挂后按 path_ 再 fopen */
    bool ReattachFile();

    /**
     * @brief 将指定条目完整解压到输出缓冲，适用于 HTML、OPF 等有界内容
     * @param max_bytes 允许的最大解压字节数，图片宜改用条目流。
     * @note 调用前须关闭当前条目流，避免在持锁期间重入归档读取。
     */
    bool ReadEntry(const char* name, std::vector<uint8_t>& out, size_t max_bytes = 8 * 1024 * 1024);

    /** @brief 查询目录索引中是否存在指定条目 */
    bool EntryExists(const char* name);

    /** @brief 读取指定条目的声明解压长度，不存在或输出指针为空时返回 false */
    bool GetUncompressedSize(const char* name, size_t* size);

    /** @brief 用归档中的文件条目名替换输出列表，不保证排列顺序 */
    void ListEntries(std::vector<std::string>& names);

    /**
     * @brief 打开条目流式读取（解压后字节流）；同时仅允许一个流。
     * @note 不把整条目装入内存，输入工作区优先使用 PSRAM；打开、读取和关闭须在同一线程配对。
     */
    bool OpenEntryStream(const char* name);

    /** @brief 读出解压后数据；返回实际字节数，0 表示 EOF 或错误（结合 StreamEof/StreamError） */
    size_t EntryStreamRead(uint8_t* dst, size_t n);

    /** @brief 查询当前条目流是否已到达结尾 */
    bool EntryStreamEof() const {
        return stream_eof_;
    }

    /** @brief 查询当前条目流是否发生读取、解压或完整性错误 */
    bool EntryStreamError() const {
        return stream_error_;
    }

    /** @brief 获取当前条目流在归档中声明的解压字节数 */
    uint32_t EntryStreamUncompSize() const {
        return stream_uncomp_size_;
    }

    /** @brief 释放条目流工作区并归还归档读取锁，允许重复关闭 */
    void CloseEntryStream();

private:
    struct Entry {
        uint16_t flags = 0;  // ZIP 加密等标志
        uint16_t method = 0;
        uint32_t crc = 0;  // 解压数据校验值
        uint32_t comp_size = 0;
        uint32_t uncomp_size = 0;
        uint32_t local_header_off = 0;
    };

    bool LoadCentralDirectory();
    bool FindEntry(const char* name, Entry* out);
    bool InflateEntry(const Entry& e, std::vector<uint8_t>& out, size_t max_bytes);
    bool SeekEntryPayload(const Entry& e, uint32_t* comp_remain);
    void ReleaseStreamLock();
    size_t ValidateStreamChunk(const uint8_t* bytes, size_t size);

    BookReadError error_ = BookReadError::kInvalidData;  // 最近一次错误分类
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
    uint32_t stream_expected_crc_ = 0;  // 条目预期校验值
    uint32_t stream_crc_ = 0;           // 已读数据校验值
    uint64_t stream_total_ = 0;         // 已读解压字节数
    z_stream stream_z_{};
    bool stream_z_init_ = false;
    uint8_t* stream_inbuf_ = nullptr;
    size_t stream_inbuf_cap_ = 0;
    size_t stream_inbuf_len_ = 0;
    size_t stream_inbuf_pos_ = 0;
};
