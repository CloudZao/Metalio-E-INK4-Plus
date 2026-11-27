#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "infra/storage/sd_paths.h"
#include "reader_types.h"

constexpr const char* kDefaultBooksDir = SD_PATH_BOOKS;

/** @brief 文件传输或删除后使目录页失效，不触发扫描 */
void InvalidateBookLibraryCache();

/** @brief 返回书籍目录的内容变更代次 */
uint32_t BookLibraryRevision();

/** @brief 按文件扩展名识别支持的书籍格式，不读取正文；不支持时返回未知格式 */
BookFormat DetectFormatByPath(const char* path);

/** @brief 从路径取无扩展名的文件名作为默认标题 */
std::string TitleFromPath(const char* path);

// 书架列表模式：单层目录项（文件夹或已识别书籍）。
struct BookDirEntry {
    enum class Kind : uint8_t { kFolder = 0, kBook };
    Kind kind = Kind::kFolder;
    std::string name;  // 显示名：文件夹为目录名；书籍为带后缀文件名
    std::string path;  // 绝对路径
    BookInfo book;     // kind==kBook 时有效
};
