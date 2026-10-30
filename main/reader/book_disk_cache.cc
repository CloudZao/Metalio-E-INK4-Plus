#include "book_disk_cache.h"

#include <atomic>
#include <climits>
#include <new>

namespace reader {
namespace {
std::atomic<uint32_t> s_cache_id{0};
struct ItemRecord {
    uint32_t source_block, source_begin, source_end; // 稳定正文位置
    uint32_t text_bytes, href_bytes; // 变长字段长度
    int32_t width, height; // 图片排版尺寸
    uint8_t kind, flags; // 内容类型及缩进、段距、章名标志
};
}

bool BookPageStore::Open(const std::string& book_path, bool count_only) {
    clear(); count_only_ = count_only;
    if (count_only) return true;
    path_ = book_path + ".rpages-" + std::to_string(s_cache_id.fetch_add(1)) + ".tmp";
    fp_ = std::fopen(path_.c_str(), "w+b");
    good_ = fp_ && offsets_.Reserve(path_ + ".idx", 1);
    return good_;
}

bool BookPageStore::EnsureFile() const {
    if (!fp_ && !path_.empty()) fp_ = std::fopen(path_.c_str(), read_only_ ? "rb" : "r+b");
    return fp_ != nullptr;
}

bool BookPageStore::Push(Page&& page) {
    if (!Good() || count_ >= INT_MAX) return false;
    if (count_only_) { ++count_; return true; }
    if (!EnsureFile() || std::fseek(fp_, 0, SEEK_END) != 0) return good_ = false;
    const long off = std::ftell(fp_);
    if (off < 0 || !offsets_.Reserve(path_ + ".idx", count_ + 1)) return good_ = false;
    offsets_[count_] = static_cast<uint32_t>(off);
    const uint32_t n = page.items.size();
    if (std::fwrite(&n, sizeof(n), 1, fp_) != 1) return good_ = false;
    for (const auto& item : page.items) {
        ItemRecord rec{item.source_block, item.source_begin, item.source_end,
            static_cast<uint32_t>(item.text.size()), static_cast<uint32_t>(item.image_href.size()),
            item.image_w, item.image_h, static_cast<uint8_t>(item.kind),
            static_cast<uint8_t>(item.para_indent | (item.para_gap_before << 1) | (item.chapter_title << 2))};
        if (std::fwrite(&rec, sizeof(rec), 1, fp_) != 1 ||
            std::fwrite(item.text.data(), 1, rec.text_bytes, fp_) != rec.text_bytes ||
            std::fwrite(item.image_href.data(), 1, rec.href_bytes, fp_) != rec.href_bytes) return good_ = false;
    }
    ++count_; return true;
}

bool BookPageStore::Finish() {
    if (count_only_) return Good();
    // FatFS 在关闭写句柄时发布文件长度，独立读句柄才能看到完整分页及索引。
    return good_ = Good() && fp_ && Detach();
}

bool BookPageStore::CloneReadOnly(BookPageStore& out) const {
    out.clear();
    out.read_only_ = true;
    out.path_ = path_;
    out.count_ = count_;
    out.good_ = Good() && offsets_.CloneReadOnly(out.offsets_, count_) && out.EnsureFile();
    return out.Good();
}

bool BookPageStore::TakeReadCache(BookPageStore& other) {
    if (path_ != other.path_ || count_ != other.count_ || !other.Good()) return false;
    for (int i = 0; i < 2; ++i) {
        cache_[i].items.swap(other.cache_[i].items);
        std::swap(cached_[i], other.cached_[i]);
    }
    return true;
}

const Page& BookPageStore::operator[](size_t index) const {
    const size_t slot = index % 2;
    if (cached_[slot] == index) return cache_[slot];
    auto& page = cache_[slot]; page.items.clear(); cached_[slot] = SIZE_MAX;
    if (!Good() || index >= count_ || !EnsureFile() ||
        std::fseek(fp_, offsets_[index], SEEK_SET) != 0) { good_ = false; return page; }
    uint32_t n = 0;
    if (std::fread(&n, sizeof(n), 1, fp_) != 1 || n > 2048) { good_ = false; return page; }
    size_t payload = 0;
    try {
        for (uint32_t i = 0; i < n; ++i) {
            ItemRecord rec{};
            if (std::fread(&rec, sizeof(rec), 1, fp_) != 1 || rec.text_bytes > 65536 ||
                rec.href_bytes > 4096 || rec.kind > static_cast<uint8_t>(ContentKind::kImage)) { good_ = false; break; }
            payload += rec.text_bytes + rec.href_bytes;
            if (payload > 256 * 1024) { good_ = false; break; }
            PageItem item;
            item.source_block = rec.source_block; item.source_begin = rec.source_begin; item.source_end = rec.source_end;
            item.kind = static_cast<ContentKind>(rec.kind); item.image_w = rec.width; item.image_h = rec.height;
            item.para_indent = rec.flags & 1; item.para_gap_before = rec.flags & 2; item.chapter_title = rec.flags & 4;
            item.text.resize(rec.text_bytes); item.image_href.resize(rec.href_bytes);
            if (std::fread(item.text.data(), 1, rec.text_bytes, fp_) != rec.text_bytes ||
                std::fread(item.image_href.data(), 1, rec.href_bytes, fp_) != rec.href_bytes) { good_ = false; break; }
            page.items.push_back(std::move(item));
        }
    } catch (const std::bad_alloc&) {
        error_ = BookReadError::kNoMemory;
        good_ = false;
    }
    if (Good()) cached_[slot] = index; else page.items.clear();
    return page;
}

bool BookPageStore::Detach() const {
    bool ok = offsets_.Detach();
    if (fp_) { if (std::fclose(fp_) != 0) ok = false; fp_ = nullptr; }
    if (!ok) good_ = false;
    return ok && good_;
}

void BookPageStore::clear() {
    if (fp_) std::fclose(fp_);
    fp_ = nullptr;
    if (!read_only_ && !path_.empty()) std::remove(path_.c_str());
    path_.clear(); offsets_.Reset(); count_ = 0; good_ = true; error_ = BookReadError::kNone; count_only_ = false; read_only_ = false;
    for (int i = 0; i < 2; ++i) { cache_[i] = Page{}; cached_[i] = SIZE_MAX; }
}

void BookPageStore::swap(BookPageStore& other) noexcept {
    using std::swap;
    swap(fp_, other.fp_); swap(path_, other.path_); offsets_.Swap(other.offsets_);
    swap(count_, other.count_); swap(good_, other.good_); swap(error_, other.error_); swap(count_only_, other.count_only_);
    swap(read_only_, other.read_only_);
    for (int i = 0; i < 2; ++i) { swap(cache_[i], other.cache_[i]); swap(cached_[i], other.cached_[i]); }
}
} // namespace reader
