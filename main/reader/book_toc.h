#pragma once

#include <cstdint>
#include <string>

namespace reader {

struct TocEntry {
    std::string title; // 章节标题，保留源文
    uint16_t depth = 0; // 原生目录层级，TXT 与旧 EBOOK 默认为零
    uint32_t first_line = 0; // TXT 显示行索引
    uint32_t first_byte_off = 0; // TXT 源文件字节锚点
    int first_page = 0; // 已绑定的页码
};

} // namespace reader
