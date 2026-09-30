#include "ebook_document.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>

#include <esp_log.h>
#include <zlib.h>
#ifndef EBOOK_EMULATOR
#include "assets/lang_config.h"
#endif

namespace reader {
namespace {

#ifndef EBOOK_EMULATOR
const char* UiChapterFmt() { return Lang::Strings::BOOK_CHAPTER_FMT; }
#else
const char* UiChapterFmt() { return "Ch.%d"; }
#endif

constexpr const char* TAG = "EbookDoc";

constexpr char kMagic[4] = {'E', 'B', 'O', 'K'};
constexpr uint16_t kVersion = 1;
constexpr size_t kHeaderSize = 64;
constexpr size_t kChapterEntrySize = 32;
constexpr size_t kBlockHeaderSize = 8;
constexpr size_t kImagePayloadHeader = 12;

bool BadChapterTitle(const std::string& t) {
    return t.empty() || t == "未知" || t == "未命名" || t == "Unknown" || t == "Untitled";
}

constexpr uint8_t kBlockText = 0x01;
constexpr uint8_t kBlockImage = 0x02;
constexpr uint8_t kBfZlib = 0x01;

constexpr uint8_t kImgJpeg = 1;
constexpr uint8_t kImgPng = 2;
constexpr uint8_t kImgA2i1 = 3;
constexpr uint8_t kImgGray = 4;

constexpr uint8_t kCtrlPara = 0x1E;

constexpr size_t kMaxChapters = 4096;
constexpr size_t kMaxChunk = 49152;  // 正文块 raw 硬上限；对齐 480×720 A2I1 ImagePayload
constexpr size_t kMaxCoverPayload = 2 * 1024 * 1024;  // 独立封面；允许 JPEG/PNG，解压后进 PSRAM
constexpr size_t kMaxTitle = 256;
constexpr size_t kMaxCompSize = kMaxChunk;  // comp_size 硬上限，防坏文件大分配
constexpr size_t kMaxMetaStr = 1024;
constexpr const char* kImgHrefPrefix = "ebook:blk:";
constexpr uint16_t kFlagPageImages = 0x0010;  // Header flags bit4：整页转图全屏阅读

uint16_t RdU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t RdU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool ReadExact(FILE* fp, void* buf, size_t n) {
    return std::fread(buf, 1, n, fp) == n;
}

bool ReadU16Str(FILE* fp, std::string& out, size_t max_len = kMaxMetaStr) {
    uint8_t lb[2];
    if (!ReadExact(fp, lb, 2)) {
        return false;
    }
    const uint16_t n = RdU16(lb);
    if (n > max_len) {
        return false;
    }
    out.assign(static_cast<size_t>(n), '\0');
    if (n == 0) {
        return true;
    }
    return ReadExact(fp, out.data(), n);
}

// 从 Metadata.extra JSON 取 book_id（约定键）；非完整 JSON 解析，避免依赖。
// 仅接受 [0-9A-Za-z_-]，最长 64；缺失/非法 → 空串。
std::string ExtractBookIdFromExtra(const std::string& extra) {
    constexpr size_t kMaxBookId = 64;
    const char* key = "\"book_id\"";
    const size_t key_len = 9;  // strlen("\"book_id\"")
    size_t pos = extra.find(key);
    if (pos == std::string::npos) {
        return {};
    }
    pos += key_len;
    while (pos < extra.size() &&
           (extra[pos] == ' ' || extra[pos] == '\t' || extra[pos] == '\n' || extra[pos] == '\r')) {
        ++pos;
    }
    if (pos >= extra.size() || extra[pos] != ':') {
        return {};
    }
    ++pos;
    while (pos < extra.size() &&
           (extra[pos] == ' ' || extra[pos] == '\t' || extra[pos] == '\n' || extra[pos] == '\r')) {
        ++pos;
    }
    if (pos >= extra.size() || extra[pos] != '"') {
        return {};
    }
    ++pos;
    size_t end = pos;
    while (end < extra.size() && extra[end] != '"') {
        const unsigned char c = static_cast<unsigned char>(extra[end]);
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        c == '_' || c == '-';
        if (!ok) {
            return {};
        }
        ++end;
        if (end - pos > kMaxBookId) {
            return {};
        }
    }
    if (end >= extra.size() || extra[end] != '"') {
        return {};
    }
    if (end == pos) {
        return {};
    }
    return extra.substr(pos, end - pos);
}

}  // namespace

void EbookDocument::DetachSd() {
    if (fp_ != nullptr) {
        std::fclose(fp_);
        fp_ = nullptr;
    }
}

bool EbookDocument::ReattachSd() {
    if (fp_ != nullptr) {
        return true;
    }
    if (path_.empty()) {
        return false;
    }
    fp_ = std::fopen(path_.c_str(), "rb");
    if (fp_ == nullptr) {
        ESP_LOGW(TAG, "reattach fail: %s", path_.c_str());
        return false;
    }
    return true;
}

void EbookDocument::Close() {
    if (fp_ != nullptr) {
        std::fclose(fp_);
        fp_ = nullptr;
    }
    path_.clear();
    title_.clear();
    author_.clear();
    book_id_.clear();
    chapters_.clear();
    version_ = 0;
    flags_ = 0;
    cover_offset_ = 0;
    cover_size_ = 0;
    file_size_ = 0;
    data_offset_ = 0;
    data_size_ = 0;
    comp_buf_.clear();
    raw_buf_.clear();
}

bool EbookDocument::Open(const char* path) {
    Close();
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    fp_ = std::fopen(path, "rb");
    if (fp_ == nullptr) {
        ESP_LOGE(TAG, "fopen fail: %s", path);
        return false;
    }
    path_ = path;
    if (std::fseek(fp_, 0, SEEK_END) != 0) {
        ESP_LOGE(TAG, "fseek end fail");
        Close();
        return false;
    }
    const long fs = std::ftell(fp_);
    if (fs <= static_cast<long>(kHeaderSize)) {
        ESP_LOGE(TAG, "file too small %ld", fs);
        Close();
        return false;
    }
    file_size_ = static_cast<uint64_t>(fs);
    if (!ReadHeader() || !ReadMetadata() || !ReadChapterIndex() || !ValidateFileLayout()) {
        Close();
        return false;
    }
    ESP_LOGI(TAG, "open %s chapters=%d chunk_max=%u title=%s book_id=%s", path,
             ChapterCount(), static_cast<unsigned>(chunk_max_), title_.c_str(),
             book_id_.empty() ? "-" : book_id_.c_str());
    return true;
}

bool EbookDocument::PeekPageImages(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    FILE* fp = std::fopen(path, "rb");
    if (fp == nullptr) {
        return false;
    }
    uint8_t hdr[kHeaderSize];
    const bool ok = ReadExact(fp, hdr, kHeaderSize);
    std::fclose(fp);
    if (!ok) {
        return false;
    }
    if (std::memcmp(hdr, kMagic, 4) != 0) {
        return false;
    }
    const uint16_t ver = RdU16(hdr + 4);
    if (ver != kVersion) {
        return false;
    }
    const uint16_t flags = RdU16(hdr + 6);
    return (flags & kFlagPageImages) != 0;
}

bool EbookDocument::PeekBookId(const char* path, std::string& out) {
    out.clear();
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    FILE* fp = std::fopen(path, "rb");
    if (fp == nullptr) {
        return false;
    }
    struct FpCloser {
        FILE* f = nullptr;
        ~FpCloser() {
            if (f != nullptr) {
                std::fclose(f);
            }
        }
    } guard{fp};

    if (std::fseek(fp, 0, SEEK_END) != 0) {
        return false;
    }
    const long fs = std::ftell(fp);
    if (fs <= static_cast<long>(kHeaderSize)) {
        return false;
    }
    const uint64_t file_size = static_cast<uint64_t>(fs);

    uint8_t hdr[kHeaderSize];
    if (std::fseek(fp, 0, SEEK_SET) != 0 || !ReadExact(fp, hdr, kHeaderSize)) {
        return false;
    }
    if (std::memcmp(hdr, kMagic, 4) != 0 || RdU16(hdr + 4) != kVersion) {
        return false;
    }
    const uint32_t header_crc = RdU32(hdr + 56);
    const uint32_t computed_crc = static_cast<uint32_t>(crc32(0L, hdr, 56));
    if (header_crc != computed_crc) {
        return false;
    }

    const uint32_t meta_offset = RdU32(hdr + 16);
    const uint32_t meta_size = RdU32(hdr + 20);
    if (meta_size == 0 || !RegionWithinFile(meta_offset, meta_size, file_size)) {
        return false;
    }
    if (std::fseek(fp, static_cast<long>(meta_offset), SEEK_SET) != 0) {
        return false;
    }
    const long meta_start = std::ftell(fp);
    std::string title;
    std::string author;
    std::string lang;
    std::string extra;
    if (!ReadU16Str(fp, title) || !ReadU16Str(fp, author) || !ReadU16Str(fp, lang) ||
        !ReadU16Str(fp, extra)) {
        return false;
    }
    const long meta_end = std::ftell(fp);
    if (meta_end < 0 || static_cast<uint64_t>(meta_end - meta_start) > meta_size) {
        return false;
    }
    out = ExtractBookIdFromExtra(extra);
    return !out.empty();
}

bool EbookDocument::ReadHeader() {
    uint8_t hdr[kHeaderSize];
    if (std::fseek(fp_, 0, SEEK_SET) != 0 || !ReadExact(fp_, hdr, kHeaderSize)) {
        ESP_LOGE(TAG, "header read fail");
        return false;
    }
    if (std::memcmp(hdr, kMagic, 4) != 0) {
        ESP_LOGE(TAG, "bad magic");
        return false;
    }
    version_ = RdU16(hdr + 4);
    if (version_ != kVersion) {
        ESP_LOGE(TAG, "unsupported version %u", static_cast<unsigned>(version_));
        return false;
    }
    flags_ = RdU16(hdr + 6);
    const uint32_t chapter_count = RdU32(hdr + 8);
    font_px_ = RdU16(hdr + 12);
    chunk_max_ = RdU16(hdr + 14);
    meta_offset_ = RdU32(hdr + 16);
    meta_size_ = RdU32(hdr + 20);
    index_offset_ = RdU32(hdr + 24);
    index_size_ = RdU32(hdr + 28);
    title_blob_offset_ = RdU32(hdr + 32);
    title_blob_size_ = RdU32(hdr + 36);
    data_offset_ = RdU32(hdr + 40);
    data_size_ = RdU32(hdr + 44);
    cover_offset_ = RdU32(hdr + 48);
    cover_size_ = RdU32(hdr + 52);
    const uint32_t header_crc = RdU32(hdr + 56);
    const uint32_t computed_crc = static_cast<uint32_t>(crc32(0L, hdr, 56));
    if (header_crc != computed_crc) {
        ESP_LOGE(TAG, "header crc mismatch got=%08x want=%08x", static_cast<unsigned>(header_crc),
                 static_cast<unsigned>(computed_crc));
        return false;
    }

    if (chapter_count == 0 || chapter_count > kMaxChapters) {
        ESP_LOGE(TAG, "bad chapter_count %u", static_cast<unsigned>(chapter_count));
        return false;
    }
    if (chunk_max_ < 64 || chunk_max_ > kMaxChunk) {
        ESP_LOGE(TAG, "bad chunk_max %u", static_cast<unsigned>(chunk_max_));
        return false;
    }
    if (index_size_ != chapter_count * kChapterEntrySize) {
        ESP_LOGE(TAG, "index_size mismatch");
        return false;
    }
    chapters_.resize(chapter_count);
    return true;
}

bool EbookDocument::ReadMetadata() {
    if (!RegionWithinFile(meta_offset_, meta_size_, file_size_)) {
        ESP_LOGE(TAG, "metadata region out of range");
        return false;
    }
    if (std::fseek(fp_, static_cast<long>(meta_offset_), SEEK_SET) != 0) {
        return false;
    }
    const long meta_start = std::ftell(fp_);
    std::string lang;
    std::string extra;
    if (!ReadU16Str(fp_, title_) || !ReadU16Str(fp_, author_) || !ReadU16Str(fp_, lang) ||
        !ReadU16Str(fp_, extra)) {
        ESP_LOGE(TAG, "metadata read fail");
        return false;
    }
    const long meta_end = std::ftell(fp_);
    if (meta_end < 0 || static_cast<uint64_t>(meta_end - meta_start) > meta_size_) {
        ESP_LOGE(TAG, "metadata exceeds meta_size");
        return false;
    }
    book_id_ = ExtractBookIdFromExtra(extra);
    return true;
}

bool EbookDocument::ReadChapterIndex() {
    if (std::fseek(fp_, static_cast<long>(index_offset_), SEEK_SET) != 0) {
        return false;
    }
    uint8_t ent[kChapterEntrySize];
    for (size_t i = 0; i < chapters_.size(); ++i) {
        if (!ReadExact(fp_, ent, kChapterEntrySize)) {
            ESP_LOGE(TAG, "chapter index read fail @%u", static_cast<unsigned>(i));
            return false;
        }
        ChapterRec& c = chapters_[i];
        c.data_offset = RdU32(ent + 0);
        c.data_length = RdU32(ent + 4);
        c.uncompressed_total = RdU32(ent + 8);
        c.block_count = RdU16(ent + 12);
        const uint16_t title_len = RdU16(ent + 14);
        const uint32_t title_off = RdU32(ent + 16);
        c.flags = RdU32(ent + 20);
        if (c.block_count == 0 || title_len > kMaxTitle) {
            ESP_LOGE(TAG, "bad chapter[%u] blocks=%u tlen=%u", static_cast<unsigned>(i),
                     static_cast<unsigned>(c.block_count), static_cast<unsigned>(title_len));
            return false;
        }
        if (!RegionWithinFile(title_off, title_len, file_size_)) {
            ESP_LOGE(TAG, "chapter[%u] title out of range", static_cast<unsigned>(i));
            return false;
        }
        if (!RegionWithinFile(c.data_offset, c.data_length, file_size_)) {
            ESP_LOGE(TAG, "chapter[%u] data out of range", static_cast<unsigned>(i));
            return false;
        }
        if (c.data_offset < data_offset_ ||
            static_cast<uint64_t>(c.data_offset) + c.data_length >
                static_cast<uint64_t>(data_offset_) + data_size_) {
            ESP_LOGE(TAG, "chapter[%u] data outside data region", static_cast<unsigned>(i));
            return false;
        }
        const long cur = std::ftell(fp_);
        if (cur < 0) {
            return false;
        }
        c.title.assign(title_len, '\0');
        if (title_len > 0) {
            if (std::fseek(fp_, static_cast<long>(title_off), SEEK_SET) != 0 ||
                !ReadExact(fp_, c.title.data(), title_len)) {
                return false;
            }
        }
        if (BadChapterTitle(c.title)) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), UiChapterFmt(), static_cast<int>(i + 1));
            c.title = buf;
        }
        if (std::fseek(fp_, cur, SEEK_SET) != 0) {
            return false;
        }
    }
    return true;
}

bool EbookDocument::RegionWithinFile(uint32_t offset, uint32_t size, uint64_t file_size) {
    if (size == 0) {
        return offset <= file_size;
    }
    const uint64_t end = static_cast<uint64_t>(offset) + static_cast<uint64_t>(size);
    return end <= file_size;
}

bool EbookDocument::ValidateFileLayout() {
    if (!RegionWithinFile(meta_offset_, meta_size_, file_size_) ||
        !RegionWithinFile(index_offset_, index_size_, file_size_) ||
        !RegionWithinFile(title_blob_offset_, title_blob_size_, file_size_) ||
        !RegionWithinFile(data_offset_, data_size_, file_size_)) {
        ESP_LOGE(TAG, "layout region out of file bounds");
        return false;
    }
    if (cover_offset_ != 0 &&
        !RegionWithinFile(cover_offset_, cover_size_, file_size_)) {
        ESP_LOGE(TAG, "cover region out of file bounds");
        return false;
    }
    if (cover_size_ > kMaxCoverPayload) {
        ESP_LOGE(TAG, "cover_size too large %u", static_cast<unsigned>(cover_size_));
        return false;
    }
    return true;
}

const std::string& EbookDocument::ChapterTitle(int index) const {
    static const std::string kEmpty;
    if (index < 0 || static_cast<size_t>(index) >= chapters_.size()) {
        return kEmpty;
    }
    return chapters_[static_cast<size_t>(index)].title;
}

uint32_t EbookDocument::ChapterUncompressedTotal(int index) const {
    if (index < 0 || static_cast<size_t>(index) >= chapters_.size()) {
        return 0;
    }
    return chapters_[static_cast<size_t>(index)].uncompressed_total;
}

bool EbookDocument::InflateBlock(const uint8_t* comp, size_t comp_len, uint8_t* raw, size_t raw_len) {
    z_stream strm{};
    strm.next_in = const_cast<Bytef*>(comp);
    strm.avail_in = static_cast<uInt>(comp_len);
    strm.next_out = raw;
    strm.avail_out = static_cast<uInt>(raw_len);
    if (inflateInit(&strm) != Z_OK) {
        return false;
    }
    const int zret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);
    if (zret != Z_STREAM_END) {
        ESP_LOGW(TAG, "inflate fail z=%d", zret);
        return false;
    }
    return strm.total_out == raw_len;
}

bool EbookDocument::ReadOneBlockAt(uint32_t offset, uint8_t& type, std::vector<uint8_t>& raw_out,
                                   uint32_t* next_offset) {
    if (fp_ == nullptr) {
        return false;
    }
    if (!RegionWithinFile(offset, static_cast<uint32_t>(kBlockHeaderSize), file_size_)) {
        return false;
    }
    uint8_t hdr[kBlockHeaderSize];
    if (std::fseek(fp_, static_cast<long>(offset), SEEK_SET) != 0 ||
        !ReadExact(fp_, hdr, kBlockHeaderSize)) {
        return false;
    }
    type = hdr[0];
    const uint8_t bflags = hdr[1];
    const uint16_t comp_size = RdU16(hdr + 2);
    const uint16_t raw_size = RdU16(hdr + 4);
    if (raw_size == 0 || raw_size > chunk_max_ || comp_size == 0 || comp_size > kMaxCompSize) {
        ESP_LOGW(TAG, "bad block @%u raw=%u comp=%u", static_cast<unsigned>(offset),
                 static_cast<unsigned>(raw_size), static_cast<unsigned>(comp_size));
        return false;
    }
    const uint32_t payload_end = offset + static_cast<uint32_t>(kBlockHeaderSize + comp_size);
    if (!RegionWithinFile(offset, static_cast<uint32_t>(kBlockHeaderSize + comp_size), file_size_)) {
        ESP_LOGW(TAG, "block payload out of file @%u", static_cast<unsigned>(offset));
        return false;
    }
    if (comp_buf_.size() < comp_size) {
        comp_buf_.resize(comp_size);
    }
    if (!ReadExact(fp_, comp_buf_.data(), comp_size)) {
        return false;
    }
    if (raw_buf_.size() < raw_size) {
        raw_buf_.resize(raw_size);
    }
    if (bflags & kBfZlib) {
        if (!InflateBlock(comp_buf_.data(), comp_size, raw_buf_.data(), raw_size)) {
            return false;
        }
    } else {
        if (comp_size != raw_size) {
            return false;
        }
        std::memcpy(raw_buf_.data(), comp_buf_.data(), raw_size);
    }
    raw_out.assign(raw_buf_.data(), raw_buf_.data() + raw_size);
    if (next_offset) {
        *next_offset = payload_end;
    }
    return true;
}

void EbookDocument::TextPayloadToBlocks(const uint8_t* data, size_t len,
                                        std::vector<ContentBlock>& out) {
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && data[j] != kCtrlPara) {
            ++j;
        }
        if (j > i) {
            ContentBlock b;
            b.kind = ContentKind::kText;
            b.text.assign(reinterpret_cast<const char*>(data + i), j - i);
            // 软换行保留；渲染侧按 \\n 折行
            out.push_back(std::move(b));
        }
        i = (j < len) ? j + 1 : j;
    }
}

bool EbookDocument::ImagePayloadToBytes(const uint8_t* raw, size_t raw_len,
                                        std::vector<uint8_t>& out) {
    out.clear();
    if (raw_len < kImagePayloadHeader) {
        return false;
    }
    const uint8_t fmt = raw[0];
    const uint16_t width = RdU16(raw + 2);
    const uint16_t height = RdU16(raw + 4);
    const uint16_t stride = RdU16(raw + 6);
    const uint32_t data_len = RdU32(raw + 8);
    if (width == 0 || height == 0 || data_len == 0 ||
        kImagePayloadHeader + data_len > raw_len) {
        return false;
    }
    const uint8_t* img = raw + kImagePayloadHeader;
    if (fmt == kImgJpeg || fmt == kImgPng || fmt == kImgA2i1) {
        out.assign(img, img + data_len);
        return true;
    }
    if (fmt == kImgGray) {
        // 简易包装：魔数 "EBGR" + w/h/stride + 像素，供 image_util 识别
        out.resize(8 + data_len);
        out[0] = 'E';
        out[1] = 'B';
        out[2] = 'G';
        out[3] = 'R';
        out[4] = static_cast<uint8_t>(width & 0xFF);
        out[5] = static_cast<uint8_t>((width >> 8) & 0xFF);
        out[6] = static_cast<uint8_t>(height & 0xFF);
        out[7] = static_cast<uint8_t>((height >> 8) & 0xFF);
        (void)stride;
        std::memcpy(out.data() + 8, img, data_len);
        return true;
    }
    ESP_LOGW(TAG, "unknown img fmt %u", static_cast<unsigned>(fmt));
    return false;
}

bool EbookDocument::LoadChapterBlocks(int chapter_index, std::vector<ContentBlock>& out) {
    out.clear();
    if (!IsOpen() || chapter_index < 0 ||
        static_cast<size_t>(chapter_index) >= chapters_.size()) {
        return false;
    }
    const ChapterRec& ch = chapters_[static_cast<size_t>(chapter_index)];
    uint32_t cursor = ch.data_offset;
    const uint32_t end = ch.data_offset + ch.data_length;
    for (uint16_t bi = 0; bi < ch.block_count; ++bi) {
        if (cursor >= end) {
            ESP_LOGW(TAG, "chapter %d truncated at block %u", chapter_index,
                     static_cast<unsigned>(bi));
            out.clear();
            return false;
        }
        uint8_t type = 0;
        std::vector<uint8_t> raw;
        uint32_t next = 0;
        const uint32_t block_off = cursor;
        if (!ReadOneBlockAt(cursor, type, raw, &next)) {
            ESP_LOGW(TAG, "block read fail ch=%d bi=%u", chapter_index, static_cast<unsigned>(bi));
            out.clear();
            return false;
        }
        cursor = next;
        if (type == kBlockText) {
            TextPayloadToBlocks(raw.data(), raw.size(), out);
        } else if (type == kBlockImage) {
            ContentBlock b;
            b.kind = ContentKind::kImage;
            b.image_href = std::string(kImgHrefPrefix) + std::to_string(block_off);
            out.push_back(std::move(b));
        } else {
            ESP_LOGD(TAG, "skip unknown block type 0x%02x", type);
        }
    }
    return true;
}

bool EbookDocument::ChapterLikelyHasContent(int chapter_index) const {
    if (chapter_index < 0 || static_cast<size_t>(chapter_index) >= chapters_.size()) {
        return false;
    }
    const ChapterRec& c = chapters_[static_cast<size_t>(chapter_index)];
    if ((c.flags & 0x01) != 0) {
        return true;  // 索引标记含图
    }
    if (c.uncompressed_total == 0) {
        return false;
    }
    return c.block_count > 0;
}

bool EbookDocument::PeekCoverImageBytes(const char* path, std::vector<uint8_t>& out) {
    out.clear();
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    FILE* fp = std::fopen(path, "rb");
    if (fp == nullptr) {
        return false;
    }
    struct FpCloser {
        FILE* f = nullptr;
        ~FpCloser() {
            if (f != nullptr) {
                std::fclose(f);
            }
        }
    } guard{fp};

    if (std::fseek(fp, 0, SEEK_END) != 0) {
        return false;
    }
    const long fs = std::ftell(fp);
    if (fs <= static_cast<long>(kHeaderSize)) {
        return false;
    }
    const uint64_t file_size = static_cast<uint64_t>(fs);

    uint8_t hdr[kHeaderSize];
    if (std::fseek(fp, 0, SEEK_SET) != 0 || !ReadExact(fp, hdr, kHeaderSize)) {
        return false;
    }
    if (std::memcmp(hdr, kMagic, 4) != 0) {
        return false;
    }
    if (RdU16(hdr + 4) != kVersion) {
        return false;
    }
    const uint32_t header_crc = RdU32(hdr + 56);
    const uint32_t computed_crc = static_cast<uint32_t>(crc32(0L, hdr, 56));
    if (header_crc != computed_crc) {
        return false;
    }

    const uint32_t cover_offset = RdU32(hdr + 48);
    const uint32_t cover_size = RdU32(hdr + 52);
    if (cover_offset == 0 || cover_size == 0) {
        return false;
    }
    if (cover_size < kImagePayloadHeader || cover_size > kMaxCoverPayload) {
        return false;
    }
    if (!RegionWithinFile(cover_offset, cover_size, file_size)) {
        return false;
    }

    std::vector<uint8_t> raw(cover_size);
    if (std::fseek(fp, static_cast<long>(cover_offset), SEEK_SET) != 0 ||
        !ReadExact(fp, raw.data(), cover_size)) {
        return false;
    }
    return ImagePayloadToBytes(raw.data(), raw.size(), out) && !out.empty();
}

bool EbookDocument::LoadCoverBytes(std::vector<uint8_t>& out) {
    out.clear();
    if (!IsOpen() || cover_offset_ == 0 || cover_size_ == 0) {
        return false;
    }
    // 封面不走翻页解压缓冲：独立预算，避免与 chunk_max 混淆
    if (cover_size_ < kImagePayloadHeader || cover_size_ > kMaxCoverPayload) {
        ESP_LOGW(TAG, "cover size out of range %u (max %u)", static_cast<unsigned>(cover_size_),
                 static_cast<unsigned>(kMaxCoverPayload));
        return false;
    }
    std::vector<uint8_t> raw(cover_size_);
    if (std::fseek(fp_, static_cast<long>(cover_offset_), SEEK_SET) != 0 ||
        !ReadExact(fp_, raw.data(), cover_size_)) {
        ESP_LOGW(TAG, "cover read fail @%u", static_cast<unsigned>(cover_offset_));
        return false;
    }
    if (!ImagePayloadToBytes(raw.data(), raw.size(), out) || out.empty()) {
        ESP_LOGW(TAG, "cover payload parse fail");
        out.clear();
        return false;
    }
    return true;
}

bool EbookDocument::LoadImageBlockBytes(uint32_t block_file_offset, std::vector<uint8_t>& out) {
    out.clear();
    if (!IsOpen()) {
        return false;
    }
    uint8_t type = 0;
    std::vector<uint8_t> raw;
    if (!ReadOneBlockAt(block_file_offset, type, raw, nullptr) || type != kBlockImage) {
        return false;
    }
    return ImagePayloadToBytes(raw.data(), raw.size(), out);
}

bool EbookDocument::ParseImageHref(const std::string& href, uint32_t& out_offset) {
    constexpr size_t kPrefLen = 10;  // strlen("ebook:blk:")
    if (href.size() <= kPrefLen || href.compare(0, kPrefLen, kImgHrefPrefix) != 0) {
        return false;
    }
    char* end = nullptr;
    const unsigned long v = std::strtoul(href.c_str() + kPrefLen, &end, 10);
    if (end == href.c_str() + kPrefLen || (end && *end != '\0')) {
        return false;
    }
    out_offset = static_cast<uint32_t>(v);
    return true;
}

}  // namespace reader
