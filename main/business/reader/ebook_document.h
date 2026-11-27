#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "reader_types.h"

// `.ebook` v1（见 tools/ebook/SPEC.md）：分块 zlib、章节可 fseek。
class EbookDocument {
public:
    /** @brief 创建尚未打开文件的 EBOOK 文档 */
    EbookDocument() = default;

    /** @brief 关闭文件并释放元数据、章节索引和解压缓冲 */
    ~EbookDocument() {
        Close();
    }

    EbookDocument(const EbookDocument&) = delete;
    EbookDocument& operator=(const EbookDocument&) = delete;

    /** @brief 打开并校验 EBOOK，加载元数据和章节索引，失败原因通过 Error 获取 */
    bool Open(const char* path);

    /** @brief 关闭文件并清空当前文档的元数据、索引和解压缓冲 */
    void Close();

    /** @brief 元数据已在内存（句柄可暂卸） */
    bool IsOpen() const {
        return !path_.empty() && !chapters_.empty();
    }

    /** @brief 卸卡前关闭文件句柄，保留重新打开所需的路径与索引 */
    void DetachSd();

    /** @brief 按保留路径恢复文件句柄，已经打开时直接返回成功 */
    bool ReattachSd();

    /** @brief 借用当前文档的文件路径 */
    const std::string& Path() const {
        return path_;
    }

    /** @brief 借用元数据中的书名 */
    const std::string& Title() const {
        return title_;
    }

    /** @brief 借用元数据中的作者信息 */
    const std::string& Author() const {
        return author_;
    }

    /** @brief 借用 Metadata.extra 中的 book_id，旧书或未声明时为空串 */
    const std::string& BookId() const {
        return book_id_;
    }

    /** @brief 获取文件头声明的单块最大解压字节数 */
    uint16_t ChunkMaxUncomp() const {
        return chunk_max_;
    }

    /** @brief 获取文件头声明的默认字体像素尺寸 */
    uint16_t DefaultFontPx() const {
        return font_px_;
    }

    /** @brief 获取已加载索引中的章节数 */
    int ChapterCount() const {
        return static_cast<int>(chapters_.size());
    }

    /** @brief 借用指定章节标题，序号无效时返回空串 */
    const std::string& ChapterTitle(int index) const;

    /** @brief 获取章节声明的文本及图元数据解压总量，序号无效时返回零 */
    uint32_t ChapterUncompressedTotal(int index) const;

    /** @brief 查询转换文件是否声明原生目录，例如 PDF 书签或 EPUB 导航目录 */
    bool HasToc() const {
        return (flags_ & 0x0008) != 0;
    }

    /** @brief 查询文件是否声明整页图片模式，供阅读端选择页面布局 */
    bool HasPageImages() const {
        return (flags_ & 0x0010) != 0;
    }

    /** @brief 仅检查文件头中的整页图片标记，无需加载章节索引 */
    static bool PeekPageImages(const char* path);

    /** @brief 仅从文件头和元数据读取 book_id，缺失或读取失败时返回 false 并清空输出 */
    static bool PeekBookId(const char* path, std::string& out);

    /**
     * @brief 按块同步读取正文，消费后释放，不累计整章
     * @param image_refs_only 仅枚举图片引用，不解压内容，供预取定位使用
     */
    bool StreamChapterBlocks(int chapter_index, const ContentBlockSink& sink, bool image_refs_only = false);

    /** @brief 获取最近一次读取失败的原因。 */
    BookReadError Error() const {
        return fp_ && std::ferror(fp_) ? BookReadError::kFileIo : error_;
    }

    /** @brief 根据章节索引判断是否可能含正文或图片，不解压内容块 */
    bool ChapterLikelyHasContent(int chapter_index) const;

    /** @brief 将封面载荷转换为可交给通用图像解码器的完整字节序列 */
    bool LoadCoverBytes(std::vector<uint8_t>& out);

    /** @brief 仅读取文件头和封面区提取封面字节，不加载章节索引 */
    static bool PeekCoverImageBytes(const char* path, std::vector<uint8_t>& out);

    /** @brief 按数据块文件偏移读取并还原图像字节，供正文插图解码 */
    bool LoadImageBlockBytes(uint32_t block_file_offset, std::vector<uint8_t>& out);

    /** @brief 解析 ebook:blk 引用中的文件偏移，格式或数值无效时返回 false */
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
    bool ReadOneBlockAt(uint32_t offset, uint8_t& type, std::vector<uint8_t>& raw_out, uint32_t* next_offset,
                        bool read_payload = true);
    static bool ImagePayloadToBytes(const uint8_t* raw, size_t raw_len, std::vector<uint8_t>& out);

    BookReadError error_ = BookReadError::kInvalidData;  // 最近一次读取失败原因
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
