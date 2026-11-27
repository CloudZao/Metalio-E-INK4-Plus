#include <atomic>
#include <cctype>
#include <cstring>
#include "book_library.h"
#include "boards/common/storage/sd_access.h"
#include "list_thumbnail_cache.h"

static std::atomic<uint32_t> s_revision{0};

static std::string ToLowerExt(const char* name) {
    std::string s;
    if (name == nullptr) {
        return s;
    }
    const char* dot = std::strrchr(name, '.');
    if (dot == nullptr) {
        return s;
    }
    s = dot;
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

void InvalidateBookLibraryCache() {
    ++s_revision;
    ClearListThumbnailCache();
}

uint32_t BookLibraryRevision() {
    return s_revision.load() + SdAccessMountGeneration();
}

BookFormat DetectFormatByPath(const char* path) {
    const std::string ext = ToLowerExt(path);
    if (ext == ".txt") {
        return BookFormat::kTxt;
    }
    // .epub 正常；无 LFN 时 8.3 会把扩展名截成 .epu，一并认作 EPUB
    if (ext == ".epub" || ext == ".epu") {
        return BookFormat::kEpub;
    }
    // .ebook；8.3 可能截成 .ebo
    if (ext == ".ebook" || ext == ".ebo") {
        return BookFormat::kEbook;
    }
    return BookFormat::kUnknown;
}

std::string TitleFromPath(const char* path) {
    if (path == nullptr) {
        return {};
    }
    const char* base = std::strrchr(path, '/');
    base = base ? base + 1 : path;
    std::string title = base;
    const size_t dot = title.find_last_of('.');
    if (dot != std::string::npos) {
        title = title.substr(0, dot);
    }
    return title;
}
