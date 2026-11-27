#pragma once
#include <cstdint>
#include <string>
#include <vector>

// TXT 的章节和内容块为零，偏移为源文件字节；章节书记录原章节、正文块及块内 UTF-8 偏移，独立于分页。
struct BookSourcePosition {
    uint32_t chapter = 0, block = 0, offset = 0;
};

/** @brief 按章节、内容块和块内偏移比较两个稳定源位置的先后 */
inline bool SourceBefore(const BookSourcePosition& a, const BookSourcePosition& b) {
    if (a.chapter != b.chapter) {
        return a.chapter < b.chapter;
    }
    if (a.block != b.block) {
        return a.block < b.block;
    }
    return a.offset < b.offset;
}

/** @brief 判断源位置是否位于包含起点、不包含终点的页面区间 */
inline bool SourceInPage(const BookSourcePosition& point, const BookSourcePosition& first,
                         const BookSourcePosition& end) {
    return !SourceBefore(point, first) && SourceBefore(point, end);
}

struct BookBookmark {
    uint32_t id = 0;
    BookSourcePosition start, end;
    uint32_t page = 0;
    int64_t added_at = 0;
    std::string excerpt;
};

class BookBookmarkStore {
public:
    static constexpr size_t kMaxBookmarks = 256, kMaxExcerptBytes = 768;

    /** @brief 加载与书籍文件信息匹配的书签，允许恢复完整临时记录，失败原因由 Error 提供 */
    void Load(const std::string& book_path, uint64_t file_size, int64_t mtime, uint32_t format);

    /** @brief 借用按编号从新到旧排列的书签列表，顺序不依赖系统时钟是否已校准 */
    const std::vector<BookBookmark>& Entries() const {
        return entries_;
    }

    /** @brief 借用加载阶段的错误信息，非空时禁止覆盖原书签记录 */
    const std::string& Error() const {
        return error_;
    }

    /** @brief 分配书签编号并保存新记录，失败保留原列表并写出原因 */
    bool Add(BookBookmark entry, std::string& error);

    /** @brief 按编号删除书签并保存，失败保留原列表并写出原因 */
    bool Remove(const std::vector<uint32_t>& ids, std::string& error);

private:
    bool Save(const std::vector<BookBookmark>& entries, std::string& error);
    std::string path_, error_;
    uint64_t file_size_ = 0;
    int64_t mtime_ = 0;
    uint32_t format_ = 0;
    std::vector<BookBookmark> entries_;
};
