#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <esp_log.h>
#include <unistd.h>
#include "book_session.h"
#include "book_storage_paths.h"
#include "infra/storage/sd_file_commit.h"
#ifndef EBOOK_EMULATOR
#include "infra/diagnostics/firmware_memory.h"
#endif

static constexpr const char* kStructureTag = "BookStructure";

#pragma pack(push, 1)
struct BookStructureHeader {
    char magic[4]; // 结构缓存标识
    uint32_t mapping_fingerprint; // 标题源位置映射指纹，与排版无关
    uint32_t version, parser_version, format; // 缓存版本、正文位置版本及书籍格式
    uint64_t file_size; // 原文件身份
    int64_t mtime; // 原文件修改时间
    uint32_t sources, toc, positions, chapters; // 各段记录数
};

struct BookStructureTocRecord {
    uint32_t byte_offset; // TXT 原始字节位置
    uint16_t depth, title_bytes; // 目录层级及 UTF-8 标题长度
};

struct BookStructureChapterRecord {
    BookSourcePosition first, end; // 原始正文半开区间
    uint64_t content_begin, content_end; // 累计正文长度范围
    int32_t toc_index; // 对应原生目录项，前言为负数
};
#pragma pack(pop)

static uint32_t StructureChecksum(uint32_t hash, const void* data, size_t bytes) {
    const auto* input = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) {
        hash = (hash ^ input[i]) * 16777619u;
    }
    return hash;
}

bool BookSession::LoadStructureCache(uint32_t generation) {
    const auto path = BookStoragePath(info_.path.c_str(), ".rsi");
    std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(path.c_str(), "rb"), &std::fclose);
    if (!file) {
        if (errno != ENOENT) {
            ESP_LOGW(kStructureTag, "cache unavailable stage=open errno=%d; scanning source", errno);
        }
        return false;
    }
    uint32_t checksum = 2166136261u;
    const auto read = [&](void* data, size_t bytes) {
        if (StructureScanInterrupted(generation) || std::fread(data, 1, bytes, file.get()) != bytes) {
            return false;
        }
        checksum = StructureChecksum(checksum, data, bytes);
        return true;
    };
    BookStructureHeader header{};
    if (!read(&header, sizeof(header)) || std::memcmp(header.magic, "RSI1", 4) != 0 || header.version != 1 ||
        header.mapping_fingerprint != StructureFingerprint() ||
        header.parser_version != kSourcePositionVersion || header.format != static_cast<uint32_t>(info_.format) ||
        header.file_size != info_.file_size || header.mtime != info_.mtime ||
        header.sources != static_cast<uint32_t>(source_chapter_count_)) {
        return false;
    }
    const bool txt = info_.format == BookFormat::kTxt;
    if ((txt && (header.toc > kMaxTxtTocEntries || header.positions != 0 ||
                 (header.toc == 0 ? header.chapters != 0 : header.chapters == 0))) ||
        (!txt && (header.toc != toc_.size() || header.positions != epub_toc_positions_.size())) ||
        header.chapters > std::max<uint64_t>(static_cast<uint64_t>(header.toc) + 1, header.sources)) {
        ESP_LOGW(kStructureTag, "cache invalid stage=counts; scanning source");
        return false;
    }
#ifndef EBOOK_EMULATOR
    const uint64_t bytes = (static_cast<uint64_t>(header.sources) + 1) * sizeof(uint64_t) +
        static_cast<uint64_t>(header.chapters) * sizeof(ReadingChapter) +
        static_cast<uint64_t>(header.positions) * sizeof(BookSourcePosition) +
        (txt ? static_cast<uint64_t>(header.toc) * (sizeof(TocEntry) + kMaxTxtChapterTitleBytes) : 0);
    if (bytes > SIZE_MAX || !FirmwarePsramCanAllocate(static_cast<size_t>(bytes))) {
        return false;
    }
#endif
    const auto invalid_payload = [&]() {
        if (Error() != BookReadError::kCancelled) {
            ESP_LOGW(kStructureTag, "cache invalid stage=payload; scanning source");
        }
        return false;
    };
    auto sizes = std::make_shared<std::vector<uint64_t>>(static_cast<size_t>(header.sources) + 1);
    auto chapters = std::make_shared<std::vector<ReadingChapter>>();
    std::vector<BookSourcePosition> positions(header.positions);
    std::vector<TocEntry> txt_toc;
    if (!read(sizes->data(), sizes->size() * sizeof(uint64_t)) || sizes->front() != 0 ||
        sizes->back() > std::numeric_limits<uint64_t>::max() / 1000 ||
        !std::is_sorted(sizes->begin(), sizes->end())) {
        return invalid_payload();
    }
    if (txt) {
        txt_toc.reserve(header.toc);
    }
    for (uint32_t i = 0; i < header.toc; ++i) {
        BookStructureTocRecord record{};
        if (!read(&record, sizeof(record)) ||
            (txt ? (record.title_bytes == 0 || record.title_bytes > kMaxTxtChapterTitleBytes) : record.title_bytes != toc_[i].title.size())) {
            return invalid_payload();
        }
        std::string title(record.title_bytes, '\0');
        if (!read(title.data(), title.size())) {
            return invalid_payload();
        }
        if (txt) {
            if (record.byte_offset >= info_.file_size ||
                (!txt_toc.empty() && record.byte_offset <= txt_toc.back().first_byte_off)) {
                return invalid_payload();
            }
            TocEntry entry;
            entry.title = std::move(title);
            entry.depth = record.depth;
            entry.first_byte_off = record.byte_offset;
            txt_toc.push_back(std::move(entry));
        } else if (title != toc_[i].title || record.depth != toc_[i].depth) {
            return invalid_payload();
        }
    }
    if (!read(positions.data(), positions.size() * sizeof(BookSourcePosition))) {
        return invalid_payload();
    }
    chapters->reserve(header.chapters);
    for (uint32_t i = 0; i < header.chapters; ++i) {
        BookStructureChapterRecord record{};
        if (!read(&record, sizeof(record)) || !SourceBefore(record.first, record.end) ||
            record.content_begin >= record.content_end || record.content_end > sizes->back() ||
            record.toc_index < -1 || record.toc_index >= static_cast<int64_t>(header.toc) ||
            (!chapters->empty() && (SourceBefore(record.first, chapters->back().end) ||
                                   record.content_begin < chapters->back().content_end))) {
            return invalid_payload();
        }
        chapters->push_back({record.first, record.end, record.content_begin, record.content_end, record.toc_index});
    }
    uint32_t expected = 0;
    if (std::fread(&expected, sizeof(expected), 1, file.get()) != 1 || expected != checksum ||
        std::fgetc(file.get()) != EOF || std::ferror(file.get()) ||
        (!txt && chapters->empty()) || (txt && sizes->back() != info_.file_size)) {
        return invalid_payload();
    }
    if (std::fclose(file.release()) != 0) {
        return invalid_payload();
    }
    if (txt) {
        toc_.swap(txt_toc);
    }
    epub_toc_positions_.swap(positions);
    source_sizes_ = std::move(sizes);
    reading_chapters_ = chapters->empty() ? nullptr : std::move(chapters);
    txt_mode_ = txt && toc_.empty();
    chapter_count_ = txt_mode_ ? 1 : static_cast<int>(reading_chapters_->size());
    ESP_LOGI(kStructureTag, "cache ready chapters=%d toc=%u", chapter_count_, static_cast<unsigned>(toc_.size()));
    return true;
}

bool BookSession::SaveStructureCache(uint32_t generation) const try {
    if (!source_sizes_ || StructureScanInterrupted(generation)) {
        return false;
    }
    const auto path = BookStoragePath(info_.path.c_str(), ".rsi");
    const auto temporary = path + ".tmp";
    const auto backup = path + ".backup";
    struct RemoveTemporary {
        const std::string& path; // 本次写入的临时缓存路径
        ~RemoveTemporary() {
            if (unlink(path.c_str()) != 0 && errno != ENOENT) {
                ESP_LOGW(kStructureTag, "cache cleanup failed errno=%d", errno);
            }
        }
    } cleanup{temporary};
    FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file) {
        ESP_LOGW(kStructureTag, "cache not saved stage=open errno=%d", errno);
        return false;
    }
    uint32_t checksum = 2166136261u;
    const auto write = [&](const void* data, size_t bytes) {
        if (StructureScanInterrupted(generation) || std::fwrite(data, 1, bytes, file) != bytes) {
            return false;
        }
        checksum = StructureChecksum(checksum, data, bytes);
        return true;
    };
    BookStructureHeader header{};
    std::memcpy(header.magic, "RSI1", 4);
    header.mapping_fingerprint = StructureFingerprint();
    header.version = 1;
    header.parser_version = kSourcePositionVersion;
    header.format = static_cast<uint32_t>(info_.format);
    header.file_size = info_.file_size;
    header.mtime = info_.mtime;
    header.sources = source_chapter_count_;
    header.toc = toc_.size();
    header.positions = epub_toc_positions_.size();
    header.chapters = reading_chapters_ ? reading_chapters_->size() : 0;
    bool ok = write(&header, sizeof(header)) &&
              write(source_sizes_->data(), source_sizes_->size() * sizeof(uint64_t));
    for (const auto& entry : toc_) {
        if (!ok) {
            break;
        }
        if (entry.title.size() > UINT16_MAX) {
            ok = false;
            break;
        }
        const BookStructureTocRecord record{entry.first_byte_off, entry.depth, static_cast<uint16_t>(entry.title.size())};
        ok = write(&record, sizeof(record)) && write(entry.title.data(), entry.title.size());
    }
    ok = ok && write(epub_toc_positions_.data(), epub_toc_positions_.size() * sizeof(BookSourcePosition));
    if (reading_chapters_) {
        for (const auto& chapter : *reading_chapters_) {
            if (!ok) {
                break;
            }
            const BookStructureChapterRecord record{chapter.first, chapter.end, chapter.content_begin,
                                                    chapter.content_end, chapter.toc_index};
            ok = write(&record, sizeof(record));
        }
    }
    if (ok) {
        ok = std::fwrite(&checksum, sizeof(checksum), 1, file) == 1 && std::fflush(file) == 0;
    }
    if (std::fclose(file) != 0) {
        ok = false;
    }
    const char* failure_stage = "write";
    int native_error = errno;
    if (ok && !StructureScanInterrupted(generation)) {
        failure_stage = "commit";
        StorageError error;
        ok = SdCommitFile(temporary, path, true, backup, &error) == SdCommitResult::Ok;
        native_error = error.system_error;
    } else {
        ok = false;
    }
    if (!ok) {
        if (Error() != BookReadError::kCancelled) {
            ESP_LOGW(kStructureTag, "cache not saved stage=%s errno=%d; keeping readable preview", failure_stage, native_error);
        }
    }
    return ok;
} catch (const std::bad_alloc&) {
    ESP_LOGW(kStructureTag, "cache not saved stage=allocate; keeping readable preview");
    return false;
}
