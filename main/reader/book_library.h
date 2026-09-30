#pragma once

#include "reader_types.h"
#include "sd_paths.h"

#include <cstdint>
#include <string>
#include <vector>

namespace reader {

constexpr const char* kDefaultBooksDir = SD_PATH_BOOKS;

// 递归扫描目录下的 .epub / .txt / .ebook（含嵌套子目录）。
// TXT 分页索引缓存保存在同目录，随书库路径迁移。
bool ScanBookLibrary(const char* dir, std::vector<BookInfo>& out);

// 默认书库列表缓存（仅 kDefaultBooksDir），对初始化和重扫做去重。
void InvalidateBookLibraryCache();

// 用当前权威列表整体替换缓存，避免重复全盘扫描。
void ReplaceBookLibraryCache(std::vector<BookInfo> books);

// 开机扫库结果投递：仅在缓存无效时写入，避免覆盖阅读侧最新更新。
void OfferBookLibraryCache(const std::vector<BookInfo>& books);

// 优先复用默认书库缓存；未命中则扫盘并 Replace 默认目录。
// 非默认目录只扫盘，不碰缓存。
bool LoadBookLibrary(const char* dir, std::vector<BookInfo>& out);

BookFormat DetectFormatByPath(const char* path);

// 从路径取无扩展名的文件名作为默认标题。
std::string TitleFromPath(const char* path);

/** 书架列表模式：单层目录项（文件夹或已识别书籍）。 */
struct BookDirEntry {
    enum class Kind : uint8_t { kFolder = 0, kBook };
    Kind kind = Kind::kFolder;
    std::string name;  // 显示名：文件夹为目录名；书籍为带后缀文件名
    std::string path;  // 绝对路径
    BookInfo book;     // kind==kBook 时有效
};

/**
 * @brief 扫描单层目录：文件夹 + 支持的书籍；自动跳过旁路/未知类型文件
 * @note 文件夹在前（名升序），书籍在后（mtime 新→旧，同时间按名）
 */
bool ScanBookDirListing(const char* dir, std::vector<BookDirEntry>& out);

}  // namespace reader
