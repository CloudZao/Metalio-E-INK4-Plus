#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace reader {

/** 友好字节数：>=1GB 用 GB，>=1MB 用 MB（一位小数），否则 KB（向上取整）。 */
inline void FormatFileSize(char* buf, size_t buf_size, uint64_t bytes) {
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        std::snprintf(buf, buf_size, "%.1f GB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        std::snprintf(buf, buf_size, "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        const unsigned kb = static_cast<unsigned>((bytes + 1023ULL) / 1024ULL);
        std::snprintf(buf, buf_size, "%u KB", kb);
    }
}

inline std::string FormatFileSizeString(uint64_t bytes) {
    char buf[32];
    FormatFileSize(buf, sizeof(buf), bytes);
    return buf;
}

}  // namespace reader
