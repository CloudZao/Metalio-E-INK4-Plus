#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include <esp_heap_caps.h>
#include "reader_types.h"

namespace reader {

/** @brief SD 索引窗口，支持临时工作文件和永久索引只读区段。 */
template <typename T> class BookDiskArray {
public:
    BookDiskArray() = default;
    ~BookDiskArray() { Reset(); }
    BookDiskArray(const BookDiskArray&) = delete;
    BookDiskArray& operator=(const BookDiskArray&) = delete;

    /** @brief 扩展逻辑记录范围；只分配一个 PSRAM 窗口。 */
    bool Reserve(const std::string& path, size_t count) {
        if (read_only_) return false;
        if (count > (static_cast<size_t>(std::numeric_limits<long>::max()) - kBytes) / sizeof(T)) { error_ = BookReadError::kUnsupported; good_ = false; return false; }
        if (path_.empty()) {
            path_ = path;
            fp_ = std::fopen(path_.c_str(), "w+b");
            cache_ = static_cast<T*>(heap_caps_malloc(kBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!fp_ || !cache_) { const auto error = !fp_ ? BookReadError::kCacheIo : BookReadError::kNoMemory; Reset(); error_ = error; good_ = false; return false; }
        }
        count_ = std::max(count_, count);
        return good_;
    }
    /** @brief 以只读窗口访问已有索引区段，不复制或删除源文件。 */
    bool OpenReadOnly(const std::string& path, size_t offset, size_t count) {
        Reset();
        const size_t limit = static_cast<size_t>(std::numeric_limits<long>::max());
        if (offset > limit || count > (limit - offset) / sizeof(T)) {
            good_ = false; error_ = BookReadError::kUnsupported; return false;
        }
        path_ = path; read_only_ = true; offset_ = offset; count_ = written_ = count;
        fp_ = std::fopen(path_.c_str(), "rb");
        if (!fp_ || std::fseek(fp_, 0, SEEK_END) != 0 || std::ftell(fp_) < static_cast<long>(offset + count * sizeof(T))) {
            Reset(); good_ = false; error_ = BookReadError::kCacheIo; return false;
        }
        cache_ = static_cast<T*>(heap_caps_malloc(kBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!cache_) { Reset(); good_ = false; error_ = BookReadError::kNoMemory; return false; }
        return true;
    }
    /** @brief 查询是否已建立工作索引。 */
    bool IsOpen() const { return !path_.empty(); }
    /** @brief 按实际记录数建立独立只读窗口；源索引须已完成写入。 */
    bool CloneReadOnly(BookDiskArray& out, size_t count) const {
        if (count > count_) {
            out.Reset(); out.good_ = false; out.error_ = BookReadError::kCacheIo;
            return false;
        }
        return out.OpenReadOnly(path_, offset_, count);
    }
    /** @brief 查询工作缓存是否仍可用。 */
    bool Good() const { return good_; }
    /** @brief 获取内存分配或 SD 缓存错误。 */
    BookReadError Error() const { return error_ != BookReadError::kNone ? error_ : good_ ? BookReadError::kNone : BookReadError::kCacheIo; }
    const T& operator[](size_t index) const { Load(index); return cache_ ? cache_[index % kItems] : empty_; }
    T& operator[](size_t index) { Load(index); if (!read_only_) dirty_ = true; return cache_ ? cache_[index % kItems] : empty_; }
    /** @brief 将脏窗口同步到 SD。 */
    bool Flush() const {
        if (!dirty_) return good_;
        if (!EnsureFile() || std::fseek(fp_, static_cast<long>(base_ * sizeof(T)), SEEK_SET) != 0 ||
            std::fwrite(cache_, sizeof(T), kItems, fp_) != kItems || std::fflush(fp_) != 0) {
            good_ = false;
            return false;
        }
        written_ = std::max(written_, base_ + kItems);
        dirty_ = false;
        return good_;
    }
    /** @brief 卸卡前关闭句柄，后续访问时按需重开。 */
    bool Detach() const {
        const bool ok = Flush();
        if (fp_) { if (std::fclose(fp_) != 0) good_ = false; fp_ = nullptr; }
        return ok && good_;
    }
    /** @brief 关闭索引并释放窗口；仅删除会话工作文件。 */
    void Reset() {
        if (fp_) std::fclose(fp_);
        fp_ = nullptr;
        if (!read_only_ && !path_.empty()) std::remove(path_.c_str());
        path_.clear();
        heap_caps_free(cache_); cache_ = nullptr;
        count_ = written_ = 0; base_ = SIZE_MAX; dirty_ = false; good_ = true; error_ = BookReadError::kNone;
        offset_ = 0; read_only_ = false;
    }
    /** @brief 交换索引所有权。 */
    void Swap(BookDiskArray& other) {
        using std::swap;
        swap(fp_, other.fp_); swap(path_, other.path_); swap(cache_, other.cache_);
        swap(count_, other.count_); swap(written_, other.written_); swap(base_, other.base_);
        swap(dirty_, other.dirty_); swap(good_, other.good_); swap(error_, other.error_);
        swap(offset_, other.offset_); swap(read_only_, other.read_only_);
    }
private:
    static constexpr size_t kBytes = 4096;
    static constexpr size_t kItems = kBytes / sizeof(T);
    static_assert(kItems > 0, "index record exceeds cache window");
    bool EnsureFile() const {
        if (!fp_ && !path_.empty()) fp_ = std::fopen(path_.c_str(), read_only_ ? "rb" : "r+b");
        return fp_ != nullptr;
    }
    void Load(size_t index) const {
        if (!good_ || index >= count_ || !cache_) { good_ = false; return; }
        const size_t base = index / kItems * kItems;
        if (base == base_) return;
        if (!Flush() || !EnsureFile()) { good_ = false; return; }
        std::fill_n(cache_, kItems, T{});
        const size_t items = read_only_ ? std::min(kItems, count_ - base) : kItems;
        if (base < written_ && (std::fseek(fp_, static_cast<long>(offset_ + base * sizeof(T)), SEEK_SET) != 0 ||
            std::fread(cache_, sizeof(T), items, fp_) != items)) good_ = false;
        base_ = base;
    }
    mutable FILE* fp_ = nullptr; // 工作索引句柄，卸卡前关闭
    std::string path_; // 工作文件或已有永久索引路径
    mutable T* cache_ = nullptr; // PSRAM 固定窗口
    size_t count_ = 0; // 允许访问的记录数
    mutable size_t written_ = 0; // 已落盘记录范围
    mutable size_t base_ = SIZE_MAX; // 当前窗口首记录
    mutable bool dirty_ = false; // 窗口是否需要落盘
    mutable bool good_ = true; // 缓存 I/O 状态
    BookReadError error_ = BookReadError::kNone; // 缓存分配或文件错误
    size_t offset_ = 0; // 已有索引区段的文件偏移
    bool read_only_ = false; // 永久索引不写回、不随会话删除
    mutable T empty_{}; // 失败访问的安全占位
};

/** @brief 章节分页工作缓存，正文写 SD，内存最多物化两页。 */
class BookPageStore {
public:
    BookPageStore() = default;
    ~BookPageStore() { clear(); }
    BookPageStore(const BookPageStore&) = delete;
    BookPageStore& operator=(const BookPageStore&) = delete;
    BookPageStore(BookPageStore&& other) noexcept { swap(other); }
    BookPageStore& operator=(BookPageStore&& other) noexcept { clear(); swap(other); return *this; }
    /** @brief 创建分页缓存；仅计页模式不写入 SD。 */
    bool Open(const std::string& book_path, bool count_only = false);
    /** @brief 将一页写入 SD，随后可释放该页正文。 */
    bool Push(Page&& page);
    /** @brief 同步正文及偏移索引，完成分页缓存。 */
    bool Finish();
    /** @brief 建立独立只读句柄和窗口，不取得源工作文件的删除权。 */
    bool CloneReadOnly(BookPageStore& out) const;
    /** @brief 发布同一分页文件的读取缓存，不更换文件所有权或执行 I/O。 */
    bool TakeReadCache(BookPageStore& other);
    /** @brief 查询工作缓存是否仍可用。 */
    bool Good() const { return good_ && offsets_.Good(); }
    /** @brief 获取内存分配或 SD 缓存错误。 */
    BookReadError Error() const { return error_ != BookReadError::kNone ? error_ : !offsets_.Good() ? offsets_.Error() : good_ ? BookReadError::kNone : BookReadError::kCacheIo; }
    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    const Page& operator[](size_t index) const;
    /** @brief 删除会话工作文件并释放页面缓存。 */
    void clear();
    /** @brief 交换会话分页缓存所有权。 */
    void swap(BookPageStore& other) noexcept;
    /** @brief 卸卡前关闭句柄，后续访问时按需重开。 */
    bool Detach() const;
private:
    bool EnsureFile() const;
    mutable FILE* fp_ = nullptr; // 分页数据文件
    std::string path_; // 会话私有文件路径
    BookDiskArray<uint32_t> offsets_; // 页记录偏移，固定窗口访问
    size_t count_ = 0; // 当前章页数
    bool count_only_ = false; // 后台仅计页时不写正文
    bool read_only_ = false; // 借用工作文件，不随游标销毁删除
    mutable bool good_ = true; // 分页缓存 I/O 状态
    mutable BookReadError error_ = BookReadError::kNone; // 页面物化失败原因
    mutable Page cache_[2]; // 当前页和相邻页，避免书签扫描使当前指针失效
    mutable size_t cached_[2] = {SIZE_MAX, SIZE_MAX}; // 两个槽位对应的页码
};

} // namespace reader
