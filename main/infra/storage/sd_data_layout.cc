#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <vector>
#include <mbedtls/build_info.h>
#include <dirent.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/md.h>
#include "sd_data_layout.h"
#include "sd_paths.h"

static const char* SdData_TAG = "SdLayout";
static const char* const kSdDataSuffixes[] = {".pos", ".pos.tmp", ".bmk", ".bmk.tmp", ".cpg", ".idx", ".rsi", ".rsi.tmp", ".rsi.backup", ".a2l8", ".a2i1"};

static bool SdDataExists(const std::string& path, bool* ok = nullptr) {
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) {
        return S_ISREG(st.st_mode);
    }
    if (errno != ENOENT) {
        if (ok) {
            *ok = false;
        }
        ESP_LOGW(SdData_TAG, "auxiliary stat failed errno=%d", errno);
    }
    return false;
}

static bool SdDataEnds(const std::string& text, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}

static bool SdDataRelative(const char* path, const char* root, std::string& out) {
    if (!path) {
        return false;
    }
    const std::string prefix = std::string(root) + "/";
    if (std::strncmp(path, prefix.c_str(), prefix.size())) {
        return false;
    }
    out = path + prefix.size();
    if (out.empty()) {
        return false;
    }
    size_t begin = 0;
    do {
        const size_t end = out.find('/', begin);
        const auto part = out.substr(begin, end - begin);
        if (part.empty() || part == "." || part == ".." || part.find('\\') != std::string::npos) {
            return false;
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    } while (true);
    return true;
}

static std::string SdDataKey(const std::string& relative) {
    unsigned char digest[32];
    const auto* algorithm = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!algorithm ||
        mbedtls_md(algorithm, reinterpret_cast<const unsigned char*>(relative.data()), relative.size(), digest)) {
        ESP_LOGE(SdData_TAG, "path key failed");
        return {};
    }
    static const char hex[] = "0123456789abcdef";
    std::string key(64, '0');
    for (size_t i = 0; i < sizeof(digest); ++i) {
        key[i * 2] = hex[digest[i] >> 4];
        key[i * 2 + 1] = hex[digest[i] & 15];
    }
    return key;
}

static bool SdDataIsCover(const char* suffix) {
    return !std::strcmp(suffix, ".a2l8") || !std::strcmp(suffix, ".a2i1");
}

static std::string SdDataLegacyBookPath(const char* book, const char* suffix) {
    std::string path = book;
    if (SdDataIsCover(suffix)) {
        const size_t dot = path.find_last_of('.');
        const size_t slash = path.find_last_of('/');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
            path.resize(dot);
        }
    }
    return path + suffix;
}

static const char* SdDataBookRoot(const char* suffix) {
    return !std::strncmp(suffix, ".pos", 4) || !std::strncmp(suffix, ".bmk", 4) ? SD_PATH_BOOK_STATE
                                                                                : SD_PATH_BOOK_CACHE;
}

static std::string SdDataStoredPath(const char* root, const std::string& key, const char* suffix, bool sharded = true) {
    return key.empty() ? std::string{}
                       : std::string(root) + "/" + (sharded ? key.substr(0, 1) + "/" : "") + key + suffix;
}

static bool SdDataEnsureParent(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos || !SdEnsureDir(path.substr(0, slash).c_str())) {
        ESP_LOGE(SdData_TAG, "auxiliary parent unavailable errno=%d", errno);
        return false;
    }
    return true;
}

static bool SdDataHasData(const std::string& path, bool temporary, bool& ok, bool recover = false) {
    if (SdDataExists(path, &ok)) {
        return true;
    }
    if (!ok) {
        return false;
    }
    if (recover && SdDataExists(path + ".previous", &ok)) {
        if (rename((path + ".previous").c_str(), path.c_str())) {
            const int error = errno;
            if (!SdDataExists(path, &ok) || !ok) {
                ok = false;
                ESP_LOGE(SdData_TAG, "auxiliary recovery failed errno=%d", error);
            }
        }
        return ok;
    }
    return ok && temporary && SdDataExists(path + ".tmp", &ok);
}

static std::string SdDataSelectStoredPath(const char* root, const std::string& key, const char* suffix,
                                          bool temporary = false) {
    if (key.empty()) {
        return {};
    }
    bool ok = true;
    for (const bool sharded : {true, false}) {
        const auto path = SdDataStoredPath(root, key, suffix, sharded);
        const bool present = SdDataHasData(path, temporary, ok, true);
        if (!ok) {
            return {};
        }
        if (present) {
            return path;
        }
    }
    const auto path = SdDataStoredPath(root, key, suffix);
    return SdDataEnsureParent(path) ? path : std::string{};
}

// 用户移动或改名时同步辅助文件；替换失败时保留可恢复副本。
static bool SdDataMove(const std::string& source, const std::string& target) {
    if (source.empty() || target.empty() || source == target) {
        return false;
    }
    const auto backup = target + ".previous";
    bool ok = true;
    const bool source_present = SdDataExists(source, &ok);
    if (!ok || !source_present) {
        return ok;
    }
    bool target_present = SdDataExists(target, &ok);
    bool backup_present = SdDataExists(backup, &ok);
    if (!ok) {
        return false;
    }
    if (!SdDataEnsureParent(target)) {
        return false;
    }
    if (backup_present) {
        if (target_present) {
            if (unlink(backup.c_str())) {
                goto fail;
            }
        } else if (rename(backup.c_str(), target.c_str())) {
            goto fail;
        }
        target_present = true;
    }
    if (target_present && rename(target.c_str(), backup.c_str())) {
        goto fail;
    }
    backup_present = target_present;
    if (rename(source.c_str(), target.c_str())) {
        const int saved = errno;
        if (backup_present && rename(backup.c_str(), target.c_str())) {
            ESP_LOGE(SdData_TAG, "auxiliary restore failed errno=%d", errno);
        }
        errno = saved;
        goto fail;
    }
    if (backup_present && unlink(backup.c_str())) {
        goto fail;
    }
    return true;
fail:
    ESP_LOGE(SdData_TAG, "auxiliary move failed errno=%d", errno);
    return false;
}

static bool SdDataBook(const std::string& path) {
    auto ext = path.substr(path.find_last_of('.') == std::string::npos ? path.size() : path.find_last_of('.'));
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c; });
    return ext == ".txt" || ext == ".epub" || ext == ".ebook" || ext == ".epu" || ext == ".ebo";
}

struct SdDataCounts {
    unsigned scanned = 0, failed = 0;  // 辅助文件维护结果
};

static bool SdDataRemove(const std::string& path, SdDataCounts& counts) {
    if (unlink(path.c_str()) == 0 || errno == ENOENT) {
        return true;
    }
    ++counts.failed;
    ESP_LOGW(SdData_TAG, "cache cleanup failed errno=%d", errno);
    return false;
}

static bool SdDataWorkName(const std::string& name) {
    for (const auto* kind : {"rpages-", "rlines-", "rindex-"}) {
        const auto at = name.rfind(kind);
        if (at == std::string::npos || (at && (name[at - 1] != '.' || !SdDataBook(name.substr(0, at - 1))))) {
            continue;
        }
        size_t end = at + std::strlen(kind);
        const size_t first = end;
        while (end < name.size() && name[end] >= '0' && name[end] <= '9') {
            ++end;
        }
        if (end != first && (name.substr(end) == ".tmp" || name.substr(end) == ".tmp.idx")) {
            return true;
        }
    }
    return false;
}

static bool SdDataHexWorkName(const std::string& name, const char* prefix) {
    const size_t first = std::strlen(prefix);
    if (name.size() != first + 12 || name.compare(0, first, prefix) || !SdDataEnds(name, ".tmp")) {
        return false;
    }
    for (size_t i = first; i < first + 8; ++i) {
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

// 每次只持有一个目录句柄，关闭后再处理文件，避免 FAT 删除导致遍历遗漏。
template <typename Visit>
static void SdDataWalk(const char* root, Visit visit, SdDataCounts& counts, bool recursive = true) {
    std::vector<std::string> pending{root};
    while (!pending.empty()) {
        const auto directory = std::move(pending.back());
        pending.pop_back();
        DIR* dir = opendir(directory.c_str());
        if (!dir) {
            if (errno != ENOENT) {
                ++counts.failed;
                ESP_LOGW(SdData_TAG, "auxiliary directory open failed errno=%d", errno);
            }
            continue;
        }
        std::unique_ptr<DIR, int (*)(DIR*)> handle(dir, closedir);
        std::vector<std::string> files;
        int read_error = 0;
        while (true) {
            errno = 0;
            const dirent* entry = readdir(dir);
            if (!entry) {
                read_error = errno;
                break;
            }
            if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, "..")) {
                continue;
            }
            const auto path = directory + "/" + entry->d_name;
            unsigned char type = entry->d_type;
            if (type == DT_UNKNOWN) {
                struct stat st{};
                if (stat(path.c_str(), &st)) {
                    if (!counts.failed) {
                        ESP_LOGW(SdData_TAG, "auxiliary item stat failed errno=%d", errno);
                    }
                    ++counts.failed;
                    continue;
                }
                if (S_ISDIR(st.st_mode)) {
                    type = DT_DIR;
                } else if (S_ISREG(st.st_mode)) {
                    type = DT_REG;
                }
            }
            if (type == DT_DIR && recursive) {
                pending.push_back(path);
            } else if (type == DT_REG) {
                files.push_back(path);
            }
            if ((++counts.scanned & 15u) == 0) {
                vTaskDelay(1);
            }
        }
        if (closedir(handle.release())) {
            ++counts.failed;
            ESP_LOGW(SdData_TAG, "auxiliary directory close failed errno=%d", errno);
        }
        if (read_error) {
            ++counts.failed;
            ESP_LOGW(SdData_TAG, "auxiliary directory read failed errno=%d", read_error);
        }
        for (const auto& path : files) {
            visit(path);
        }
    }
}

static void SdDataRelocateFile(const std::string& source, const std::string& target, SdDataCounts& counts) {
    if (!SdDataMove(source, target)) {
        ++counts.failed;
    }
}

std::string SdDataBookPath(const char* book, const char* suffix) {
    std::string relative;
    if (!suffix || !SdDataRelative(book, SD_PATH_BOOKS, relative)) {
        return {};
    }
    bool ok = true;
    const bool temporary = !std::strcmp(suffix, ".pos") || !std::strcmp(suffix, ".bmk");
    const auto legacy = SdDataLegacyBookPath(book, suffix);
    const bool old = SdDataHasData(legacy, temporary, ok);
    if (!ok) {
        return {};
    }
    return old ? legacy : SdDataSelectStoredPath(SdDataBookRoot(suffix), SdDataKey(relative), suffix, temporary);
}

std::string SdDataWallpaperMetaPath(const char* wallpaper) {
    std::string relative;
    if (!SdDataRelative(wallpaper, SD_PATH_WALLPAPER, relative)) {
        return {};
    }
    bool ok = true;
    for (const auto* suffix : {".meta.json", ".meta"}) {
        const std::string old = std::string(wallpaper) + suffix;
        const bool present = SdDataHasData(old, false, ok);
        if (!ok) {
            return {};
        }
        if (present) {
            return old;
        }
    }
    return SdDataSelectStoredPath(SD_PATH_WALLPAPER_META, SdDataKey(relative), ".json");
}

std::string SdDataBookWorkPath(const char* kind, unsigned id) {
    if (!SdEnsureDirectory(SD_PATH_BOOK_WORK)) {
        ESP_LOGE(SdData_TAG, "book work directory unavailable errno=%d", errno);
        return {};
    }
    return std::string(SD_PATH_BOOK_WORK) + "/" + kind + "-" + std::to_string(id) + ".tmp";
}

void SdDataPrepare() try {
    SdDataCounts counts;
    if (!SdEnsureAppLayout()) {
        ESP_LOGE(SdData_TAG, "storage layout unavailable");
        return;
    }
    // 旧解码器工作文件不再产生，挂载后仅清理其受限前缀。
    SdDataCleanWorkFiles(SD_PATH_WALLPAPER_TEMP, "pj-");
    // 活动下载和图片处理缓存由所属模块清理；这里只维护分页工作文件和原图备份。
    SdDataWalk(
        SD_PATH_BOOK_WORK,
        [&](const std::string& path) {
            if (SdDataWorkName(path.substr(path.find_last_of('/') + 1))) {
                SdDataRemove(path, counts);
            }
        },
        counts, false);
    // 保留覆盖保存中断后的原图恢复，避免工作缓存清理丢失唯一原图。
    SdDataWalk(
        SD_PATH_WALLPAPER_BACKUP,
        [&](const std::string& path) {
            const auto name = path.substr(path.find_last_of('/') + 1);
            if (!SdIsValidFileName(name.c_str())) {
                return;
            }
            const auto image = std::string(SD_PATH_WALLPAPER) + "/" + name;
            if (SdDataExists(image)) {
                SdDataRemove(path, counts);
            } else {
                SdDataRelocateFile(path, image, counts);
            }
        },
        counts);
    if (counts.failed) {
        ESP_LOGW(SdData_TAG, "storage work cleanup incomplete failed=%u", counts.failed);
    }
} catch (const std::bad_alloc&) {
    ESP_LOGE(SdData_TAG, "storage work cleanup interrupted: no memory");
}

bool SdDataCleanWorkFiles(const char* root, const char* prefix) try {
    if (!root || !prefix) {
        return false;
    }
    SdDataCounts counts;
    SdDataWalk(
        root,
        [&](const std::string& path) {
            if (SdDataHexWorkName(path.substr(path.find_last_of('/') + 1), prefix)) {
                SdDataRemove(path, counts);
            }
        },
        counts, false);
    return counts.failed == 0;
} catch (const std::bad_alloc&) {
    ESP_LOGE(SdData_TAG, "work cleanup interrupted: no memory");
    return false;
}

bool SdDataDeleteBookData(const char* book) {
    std::string relative;
    if (!SdDataRelative(book, SD_PATH_BOOKS, relative)) {
        return false;
    }
    const auto key = SdDataKey(relative);
    if (key.empty()) {
        return false;
    }
    SdDataCounts counts;
    for (const auto* suffix : kSdDataSuffixes) {
        if (SdDataEnds(suffix, ".tmp")) {
            continue;
        }
        for (const bool sharded : {true, false}) {
            const auto candidate = SdDataStoredPath(SdDataBookRoot(suffix), key, suffix, sharded);
            for (const auto* tail : {"", ".tmp", ".previous", ".tmp.previous"}) {
                SdDataRemove(candidate + tail, counts);
            }
        }
        if (!SdDataIsCover(suffix)) {
            const auto legacy = SdDataLegacyBookPath(book, suffix);
            for (const auto* tail : {"", ".tmp", ".previous"}) {
                SdDataRemove(legacy + tail, counts);
            }
        }
    }
    SdDataRemove(std::string(book) + ".tmp", counts);
    const auto cover = SdDataLegacyBookPath(book, ".a2l8");
    bool ok = true;
    const auto mono = SdDataLegacyBookPath(book, ".a2i1");
    const bool have_cover = SdDataExists(cover, &ok) || SdDataExists(mono, &ok) || SdDataExists(cover + ".tmp", &ok) ||
                            SdDataExists(mono + ".tmp", &ok);
    if (!ok) {
        ++counts.failed;
    }
    if (ok && have_cover) {
        const auto stem = cover.substr(0, cover.size() - 5);
        bool shared = false;
        for (const auto* suffix : {".txt", ".epub", ".ebook", ".epu", ".ebo"}) {
            shared |= SdDataExists(stem + suffix, &ok);
        }
        if (!ok) {
            ++counts.failed;
        }
        if (ok && !shared) {
            for (const auto* suffix : {".a2l8", ".a2i1"}) {
                const auto path = SdDataLegacyBookPath(book, suffix);
                SdDataRemove(path, counts);
                SdDataRemove(path + ".tmp", counts);
            }
        }
    }
    // 带未知会话编号的旧工作文件由所属目录删除处理，单书删除不遍历书籍目录。
    return counts.failed == 0;
}

bool SdDataDeleteWallpaperData(const char* wallpaper) {
    std::string relative;
    if (!SdDataRelative(wallpaper, SD_PATH_WALLPAPER, relative)) {
        return false;
    }
    const auto key = SdDataKey(relative);
    if (key.empty()) {
        return false;
    }
    SdDataCounts counts;
    for (const bool sharded : {true, false}) {
        const auto path = SdDataStoredPath(SD_PATH_WALLPAPER_META, key, ".json", sharded);
        for (const auto* tail : {"", ".tmp", ".previous"}) {
            SdDataRemove(path + tail, counts);
        }
    }
    for (const auto* suffix : {".meta.json", ".meta"}) {
        const auto legacy = std::string(wallpaper) + suffix;
        SdDataRemove(legacy, counts);
        SdDataRemove(legacy + ".tmp", counts);
    }
    return counts.failed == 0;
}

bool SdDataRelocate(const char* from, const char* to) try {
    SdDataCounts counts;
    auto relocate = [&](const std::string& destination) {
        const auto original = std::string(from) + destination.substr(std::strlen(to));
        std::string relative, original_relative;
        const bool book = SdDataBook(destination) && SdDataRelative(destination.c_str(), SD_PATH_BOOKS, relative);
        const char* root = book ? SD_PATH_BOOKS : SD_PATH_WALLPAPER;
        if (!SdDataRelative(destination.c_str(), root, relative) ||
            !SdDataRelative(original.c_str(), root, original_relative)) {
            return;
        }
        const auto source_key = SdDataKey(original_relative), target_key = SdDataKey(relative);
        if (source_key.empty() || target_key.empty()) {
            ++counts.failed;
            return;
        }
        if (book) {
            for (const auto* suffix : kSdDataSuffixes) {
                for (const bool sharded : {true, false}) {
                    SdDataRelocateFile(SdDataStoredPath(SdDataBookRoot(suffix), source_key, suffix, sharded),
                                       SdDataStoredPath(SdDataBookRoot(suffix), target_key, suffix, sharded), counts);
                }
                SdDataRelocateFile(SdDataLegacyBookPath(original.c_str(), suffix),
                                   SdDataLegacyBookPath(destination.c_str(), suffix), counts);
            }
        } else {
            for (const bool sharded : {true, false}) {
                SdDataRelocateFile(SdDataStoredPath(SD_PATH_WALLPAPER_META, source_key, ".json", sharded),
                                   SdDataStoredPath(SD_PATH_WALLPAPER_META, target_key, ".json", sharded), counts);
            }
            for (const auto* suffix : {".meta", ".meta.json"}) {
                SdDataRelocateFile(original + suffix, destination + suffix, counts);
            }
        }
    };
    struct stat st{};
    if (stat(to, &st)) {
        ESP_LOGE(SdData_TAG, "auxiliary relocation inspect failed errno=%d", errno);
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        SdDataWalk(to, relocate, counts);
    } else {
        relocate(to);
    }
    return !counts.failed;
} catch (const std::bad_alloc&) {
    ESP_LOGE(SdData_TAG, "auxiliary relocation failed: no memory");
    return false;
}
