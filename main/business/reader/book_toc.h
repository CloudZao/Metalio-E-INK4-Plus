#pragma once

#include <cstdint>
#include <string>

struct TocEntry {
    std::string title;            // 章节标题，保留源文
    uint16_t depth = 0;           // 原生目录层级，TXT 与旧 EBOOK 默认为零
    uint32_t first_byte_off = 0;  // TXT 源文件字节锚点
};
