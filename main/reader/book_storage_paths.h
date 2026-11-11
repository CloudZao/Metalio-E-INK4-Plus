#pragma once
#include <string>
#ifndef EBOOK_EMULATOR
#include "sd_data_layout.h"
#endif

namespace reader {
/** @brief 生成阅读辅助数据路径；模拟器保留宿主文件旁路布局。 */
static inline std::string BookStoragePath(const char* book, const char* suffix) {
    #ifdef EBOOK_EMULATOR
    return book ? std::string(book) + suffix : std::string{};
    #else
    return sd_data::BookPath(book, suffix);
    #endif
}
/** @brief 生成分页工作路径；设备端集中存放到阅读应用临时目录。 */
static inline std::string BookStorageWorkPath(const std::string& book, const char* kind, unsigned id) {
    #ifdef EBOOK_EMULATOR
    return book + "." + kind + "-" + std::to_string(id) + ".tmp";
    #else
    (void)book;
    return sd_data::BookWorkPath(kind, id);
    #endif
}
}
