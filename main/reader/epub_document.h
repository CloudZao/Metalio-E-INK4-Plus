#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>

#include "reader_types.h"
#include "zip_reader.h"

namespace reader {

struct EpubSpineItem {
    std::string href;       // 相对 EPUB 根
    std::string media_type;
    std::string id;
};

// EPUB 2/3 元数据 + spine + 封面/章节抽取（借鉴 CrossPoint 流程，POSIX/ZIP 实现）。
class EpubDocument {
public:
    EpubDocument() = default;

    bool Open(const char* path);
    void Close();

    bool IsOpen() const { return zip_.IsOpen(); }

    void DetachSd() { zip_.DetachFile(); }
    bool ReattachSd() { return zip_.ReattachFile(); }
    const std::string& Path() const { return path_; }
    const std::string& Title() const { return title_; }
    const std::string& Author() const { return author_; }
    const std::string& CoverHref() const { return cover_href_; }

    int SpineCount() const { return static_cast<int>(spine_.size()); }
    const EpubSpineItem* SpineItem(int index) const;

    // 解压封面图到 bytes（PNG/JPEG）；小资源/调试用。大图请走 DecodeCoverImageToL8。
    bool LoadCoverBytes(std::vector<uint8_t>& out, size_t max_bytes = 8 * 1024 * 1024);

    // 读取 spine 章节 HTML，并解析为 ContentBlock 列表。
    bool LoadChapterBlocks(int spine_index, std::vector<ContentBlock>& out, size_t max_html_bytes = 1024 * 1024);

    // 读取 EPUB 内任意资源（HTML/CSS 等小文件）；图片请走 DecodeItemImageToL8。
    bool LoadItemBytes(const char* href, std::vector<uint8_t>& out, size_t max_bytes = 8 * 1024 * 1024);

    /** @brief 窥探条目解压后大小；不存在返回 false */
    bool GetItemUncompressedSize(const char* href, size_t* size);

    /**
     * @brief 流式解码条目图片到 L8（JPEG/PNG；PSRAM 工作区，不整包装入）
     * @param abort 非空且为 true 时尽快中止
     */
    bool DecodeItemImageToL8(const char* href, int max_w, int max_h, RasterImage& out,
                             const std::atomic<bool>* abort = nullptr);

    /** @brief 流式解码封面到 L8 */
    bool DecodeCoverImageToL8(int max_w, int max_h, RasterImage& out,
                              const std::atomic<bool>* abort = nullptr);

private:
    bool ParseContainer();
    bool ParseOpf(const std::string& opf_path);
    static std::string DirOf(const std::string& path);

    ZipReader zip_;
    std::string path_;
    std::string content_base_;  // OPF 所在目录
    std::string title_;
    std::string author_;
    std::string cover_href_;
    std::vector<EpubSpineItem> spine_;
    // id -> href
    std::unordered_map<std::string, std::string> manifest_;
    std::unordered_map<std::string, std::string> manifest_props_;  // id -> properties
};

}  // namespace reader
