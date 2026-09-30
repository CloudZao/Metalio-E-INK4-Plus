#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "reader_types.h"

namespace reader {

// `.ebook` v1（见 tools/ebook/SPEC.md）：分块 zlib、章节可 fseek。
class EbookDocument {
public:
    EbookDocument() = default;
    ~EbookDocument() { Close(); }

    EbookDocument(const EbookDocument&) = delete;
    EbookDocument& operator=(const EbookDocument&) = delete;

    bool Open(const char* path);
    void Close();
    /** @brief 元数据已在内存（句柄可暂卸） */
    bool IsOpen() const { return !path_.empty() && !chapters_.empty(); }

    void DetachSd();
    bool ReattachSd();

    const std::string& Path() const { return path_; }
    const std::string& Title() const { return title_; }
    const std::string& Author() const { return author_; }
    // Metadata.extra JSON 的 book_id；旧书或缺失为空串（不视为打开失败）
    const std::string& BookId() const { return book_id_; }
    uint16_t ChunkMaxUncomp() const { return chunk_max_; }
    uint16_t DefaultFontPx() const { return font_px_; }

    int ChapterCount() const { return static_cast<int>(chapters_.size()); }
    const std::string& ChapterTitle(int index) const;
    // 章索引 uncompressed_total（文本+图元数据）；越界返回 0
    uint32_t ChapterUncompressedTotal(int index) const;

    // Header.flags bit3：转换时写入的原生目录标记（PDF Outline / EPUB nav·NCX）
    bool HasToc() const { return (flags_ & 0x0008) != 0; }

    // Header.flags bit4：整页转图书；阅读端全屏铺满显示区
    bool HasPageImages() const { return (flags_ & 0x0010) != 0; }

    // 仅读 Header flags（打开前 peek，供阅读页去边距 / 设全宽视口）
    static bool PeekPageImages(const char* path);

    // 轻量读 Metadata.extra.book_id（Header + Meta，不读章节索引）；缺失/非 .ebook → false 且 out 空
    static bool PeekBookId(const char* path, std::string& out);

    // 按章流式读块 → ContentBlock（图用 ebook:blk:<file_offset> 引用）。
    // 空章返回 true 且 out 为空（不再填「本章无内容」占位）。
    bool LoadChapterBlocks(int chapter_index, std::vector<ContentBlock>& out);

    // 轻量判断章是否有内容（读索引 flags/uncompressed_total，不解压块）。
    bool ChapterLikelyHasContent(int chapter_index) const;

    // 封面 ImagePayload → 可交给 DecodeImageToL8 / A2I1 解码的字节（JPEG/PNG/A2I1 整文件，或 gray 包装）。
    bool LoadCoverBytes(std::vector<uint8_t>& out);

    // 轻量读封面：仅 64B Header + cover 区，不读章节索引（供旁路 .a2i1 提取）。
    static bool PeekCoverImageBytes(const char* path, std::vector<uint8_t>& out);

    // 从数据区某块偏移解出图像原始字节（供 LoadPageImage）。
    bool LoadImageBlockBytes(uint32_t block_file_offset, std::vector<uint8_t>& out);

    // 解析 "ebook:blk:12345" → offset；失败返回 false。
    static bool ParseImageHref(const std::string& href, uint32_t& out_offset);

private:
    struct ChapterRec {
        uint32_t data_offset = 0;
        uint32_t data_length = 0;
        uint32_t uncompressed_total = 0;
        uint16_t block_count = 0;
        uint32_t flags = 0;
        std::string title;
    };

    bool ReadHeader();
    bool ReadMetadata();
    bool ReadChapterIndex();
    bool ValidateFileLayout();
    static bool RegionWithinFile(uint32_t offset, uint32_t size, uint64_t file_size);
    bool InflateBlock(const uint8_t* comp, size_t comp_len, uint8_t* raw, size_t raw_len);
    bool ReadOneBlockAt(uint32_t offset, uint8_t& type, std::vector<uint8_t>& raw_out,
                        uint32_t* next_offset);
    static void TextPayloadToBlocks(const uint8_t* data, size_t len, std::vector<ContentBlock>& out);
    static bool ImagePayloadToBytes(const uint8_t* raw, size_t raw_len, std::vector<uint8_t>& out);

    FILE* fp_ = nullptr;
    std::string path_;
    std::string title_;
    std::string author_;
    std::string book_id_;
    uint16_t version_ = 0;
    uint16_t flags_ = 0;
    uint16_t font_px_ = 25;
    uint16_t chunk_max_ = 4096;
    uint32_t meta_offset_ = 0;
    uint32_t meta_size_ = 0;
    uint32_t index_offset_ = 0;
    uint32_t index_size_ = 0;
    uint32_t title_blob_offset_ = 0;
    uint32_t title_blob_size_ = 0;
    uint32_t data_offset_ = 0;
    uint32_t data_size_ = 0;
    uint32_t cover_offset_ = 0;
    uint32_t cover_size_ = 0;
    uint64_t file_size_ = 0;
    std::vector<ChapterRec> chapters_;
    std::vector<uint8_t> comp_buf_;
    std::vector<uint8_t> raw_buf_;
};

}  // namespace reader
