#include "sd_data_layout.h"
#include "sd_paths.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <new>
#include <memory>
#include <vector>
#include <mbedtls/md.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace sd_data {
static const char* TAG = "SdLayout";
static const char* const kSuffixes[] = {".pos", ".pos.tmp", ".bmk", ".bmk.tmp", ".cpg", ".idx", ".a2l8", ".a2i1"};

static bool Exists(const std::string& path, bool* ok = nullptr) {
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) return S_ISREG(st.st_mode);
    if (errno != ENOENT) {
        if (ok) *ok = false;
        ESP_LOGW(TAG, "auxiliary stat failed errno=%d", errno);
    }
    return false;
}

static bool Ends(const std::string& text, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}

static bool Relative(const char* path, const char* root, std::string& out) {
    if (!path) return false;
    const std::string prefix = std::string(root) + "/";
    if (std::strncmp(path, prefix.c_str(), prefix.size())) return false;
    out = path + prefix.size();
    if (out.empty()) return false;
    size_t begin = 0;
    do {
        const size_t end = out.find('/', begin);
        const auto part = out.substr(begin, end - begin);
        if (part.empty() || part == "." || part == ".." || part.find('\\') != std::string::npos) return false;
        if (end == std::string::npos) break;
        begin = end + 1;
    } while (true);
    return true;
}

static std::string Key(const std::string& relative) {
    unsigned char digest[32];
    const auto* algorithm = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!algorithm || mbedtls_md(algorithm, reinterpret_cast<const unsigned char*>(relative.data()), relative.size(), digest)) {
        ESP_LOGE(TAG, "path key failed");
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

static bool IsCover(const char* suffix) {
    return !std::strcmp(suffix, ".a2l8") || !std::strcmp(suffix, ".a2i1");
}

static std::string LegacyBookPath(const char* book, const char* suffix) {
    std::string path = book;
    if (IsCover(suffix)) {
        const size_t dot = path.find_last_of('.');
        const size_t slash = path.find_last_of('/');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) path.resize(dot);
    }
    return path + suffix;
}

static const char* BookRoot(const char* suffix) {
    return !std::strncmp(suffix, ".pos", 4) || !std::strncmp(suffix, ".bmk", 4) ? SD_PATH_BOOK_STATE : SD_PATH_BOOK_CACHE;
}

static std::string StoredPath(const char* root, const std::string& key, const char* suffix, bool sharded = true) {
    return key.empty() ? std::string{} : std::string(root) + "/" + (sharded ? key.substr(0, 1) + "/" : "") + key + suffix;
}

static bool EnsureParent(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos || !SdEnsureDir(path.substr(0, slash).c_str())) {
        ESP_LOGE(TAG, "auxiliary parent unavailable errno=%d", errno);
        return false;
    }
    return true;
}

static bool HasData(const std::string& path, bool temporary, bool& ok, bool recover = false) {
    if (Exists(path, &ok)) return true;
    if (!ok) return false;
    if (recover && Exists(path + ".previous", &ok)) {
        if (rename((path + ".previous").c_str(), path.c_str())) {
            const int error = errno;
            if (!Exists(path, &ok) || !ok) {
                ok = false;
                ESP_LOGE(TAG, "auxiliary recovery failed errno=%d", error);
            }
        }
        return ok;
    }
    return ok && temporary && Exists(path + ".tmp", &ok);
}

static std::string SelectStoredPath(const char* root, const std::string& key, const char* suffix, bool temporary = false) {
    if (key.empty()) return {};
    bool ok = true;
    for (const bool sharded : {true, false}) {
        const auto path = StoredPath(root, key, suffix, sharded);
        const bool present = HasData(path, temporary, ok, true);
        if (!ok) return {};
        if (present) return path;
    }
    const auto path = StoredPath(root, key, suffix);
    return EnsureParent(path) ? path : std::string{};
}

std::string BookPath(const char* book, const char* suffix) {
    std::string relative;
    if (!suffix || !Relative(book, SD_PATH_BOOKS, relative)) return {};
    bool ok = true;
    const bool temporary = !std::strcmp(suffix, ".pos") || !std::strcmp(suffix, ".bmk");
    const auto legacy = LegacyBookPath(book, suffix);
    const bool old = HasData(legacy, temporary, ok);
    if (!ok) return {};
    return old ? legacy : SelectStoredPath(BookRoot(suffix), Key(relative), suffix, temporary);
}

std::string WallpaperMetaPath(const char* wallpaper) {
    std::string relative;
    if (!Relative(wallpaper, SD_PATH_WALLPAPER, relative)) return {};
    bool ok = true;
    for (const auto* suffix : {".meta.json", ".meta"}) {
        const std::string old = std::string(wallpaper) + suffix;
        const bool present = HasData(old, false, ok);
        if (!ok) return {};
        if (present) return old;
    }
    return SelectStoredPath(SD_PATH_WALLPAPER_META, Key(relative), ".json");
}

std::string BookWorkPath(const char* kind, unsigned id) {
    if (!SdEnsureDir(SD_PATH_TEMP) || !SdEnsureDir(SD_PATH_BOOK_TEMP) || !SdEnsureDir(SD_PATH_BOOK_WORK)) {
        ESP_LOGE(TAG, "book work directory unavailable errno=%d", errno);
        return {};
    }
    return std::string(SD_PATH_BOOK_WORK) + "/" + kind + "-" + std::to_string(id) + ".tmp";
}

// 用户移动或改名时同步辅助文件；替换失败时保留可恢复副本。
static bool Move(const std::string& source, const std::string& target) {
    if (source.empty() || target.empty() || source == target) return false;
    const auto backup = target + ".previous";
    bool ok = true;
    const bool source_present = Exists(source, &ok);
    if (!ok || !source_present) return ok;
    bool target_present = Exists(target, &ok);
    bool backup_present = Exists(backup, &ok);
    if (!ok) return false;
    if (!EnsureParent(target)) return false;
    if (backup_present) {
        if (target_present) {
            if (unlink(backup.c_str())) goto fail;
        } else if (rename(backup.c_str(), target.c_str())) goto fail;
        target_present = true;
    }
    if (target_present && rename(target.c_str(), backup.c_str())) goto fail;
    backup_present = target_present;
    if (rename(source.c_str(), target.c_str())) {
        const int saved = errno;
        if (backup_present && rename(backup.c_str(), target.c_str()))
            ESP_LOGE(TAG, "auxiliary restore failed errno=%d", errno);
        errno = saved;
        goto fail;
    }
    if (backup_present && unlink(backup.c_str())) goto fail;
    return true;
fail:
    ESP_LOGE(TAG, "auxiliary move failed errno=%d", errno);
    return false;
}

static bool Book(const std::string& path) {
    auto ext = path.substr(path.find_last_of('.') == std::string::npos ? path.size() : path.find_last_of('.'));
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c; });
    return ext == ".txt" || ext == ".epub" || ext == ".ebook" || ext == ".epu" || ext == ".ebo";
}

struct Counts {
    unsigned scanned = 0, failed = 0; // 辅助文件维护结果
};

static bool Remove(const std::string& path, Counts& counts) {
    if (unlink(path.c_str()) == 0 || errno == ENOENT) return true;
    ++counts.failed;
    ESP_LOGW(TAG, "cache cleanup failed errno=%d", errno);
    return false;
}

static bool WorkName(const std::string& name) {
    for (const auto* kind : {"rpages-", "rlines-", "rindex-"}) {
        const auto at = name.rfind(kind);
        if (at == std::string::npos || (at && (name[at - 1] != '.' || !Book(name.substr(0, at - 1))))) continue;
        size_t end = at + std::strlen(kind);
        const size_t first = end;
        while (end < name.size() && name[end] >= '0' && name[end] <= '9') ++end;
        if (end != first && (name.substr(end) == ".tmp" || name.substr(end) == ".tmp.idx")) return true;
    }
    return false;
}

static bool HexWorkName(const std::string& name, const char* prefix) {
    const size_t first = std::strlen(prefix);
    if (name.size() != first + 12 || name.compare(0, first, prefix) || !Ends(name, ".tmp")) return false;
    for (size_t i = first; i < first + 8; ++i)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return false;
    return true;
}

// 每次只持有一个目录句柄，关闭后再处理文件，避免 FAT 删除导致遍历遗漏。
template <typename Visit> static void Walk(const char* root, Visit visit, Counts& counts, bool recursive = true) {
    std::vector<std::string> pending{root};
    while (!pending.empty()) {
        const auto directory = std::move(pending.back()); pending.pop_back();
        DIR* dir = opendir(directory.c_str());
        if (!dir) {
            if (errno != ENOENT) { ++counts.failed; ESP_LOGW(TAG, "auxiliary directory open failed errno=%d", errno); }
            continue;
        }
        std::unique_ptr<DIR, int (*)(DIR*)> handle(dir, closedir);
        std::vector<std::string> files;
        int read_error = 0;
        while (true) {
            errno = 0;
            const dirent* entry = readdir(dir);
            if (!entry) { read_error = errno; break; }
            if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, "..")) continue;
            const auto path = directory + "/" + entry->d_name;
            unsigned char type = entry->d_type;
            if (type == DT_UNKNOWN) {
                struct stat st{};
                if (stat(path.c_str(), &st)) {
                    if (!counts.failed) ESP_LOGW(TAG, "auxiliary item stat failed errno=%d", errno);
                    ++counts.failed;
                    continue;
                }
                if (S_ISDIR(st.st_mode)) type = DT_DIR;
                else if (S_ISREG(st.st_mode)) type = DT_REG;
            }
            if (type == DT_DIR && recursive) pending.push_back(path);
            else if (type == DT_REG) files.push_back(path);
            if ((++counts.scanned & 15u) == 0) vTaskDelay(1);
        }
        if (closedir(handle.release())) {
            ++counts.failed;
            ESP_LOGW(TAG, "auxiliary directory close failed errno=%d", errno);
        }
        if (read_error) { ++counts.failed; ESP_LOGW(TAG, "auxiliary directory read failed errno=%d", read_error); }
        for (const auto& path : files) visit(path);
    }
}

static void RelocateFile(const std::string& source, const std::string& target, Counts& counts) {
    if (!Move(source, target)) ++counts.failed;
}

bool CleanWorkFiles(const char* root, const char* prefix) try {
    if (!root || !prefix) return false;
    Counts counts;
    Walk(root, [&](const std::string& path) {
        if (HexWorkName(path.substr(path.find_last_of('/') + 1), prefix)) Remove(path, counts);
    }, counts, false);
    return counts.failed == 0;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "work cleanup interrupted: no memory");
    return false;
}

void Prepare() try {
    Counts counts;
    if (!SdEnsureAppLayout()) { ESP_LOGE(TAG, "storage layout unavailable"); return; }
    // 下载和图片处理缓存由所属模块清理；这里只维护分页工作文件和原图备份。
    Walk(SD_PATH_BOOK_WORK, [&](const std::string& path) {
        if (WorkName(path.substr(path.find_last_of('/') + 1))) Remove(path, counts);
    }, counts, false);
    // 保留覆盖保存中断后的原图恢复，避免工作缓存清理丢失唯一原图。
    Walk(SD_PATH_WALLPAPER_TEMP "/backup", [&](const std::string& path) {
        const auto name = path.substr(path.find_last_of('/') + 1);
        if (!SdIsValidFileName(name.c_str())) return;
        const auto image = std::string(SD_PATH_WALLPAPER) + "/" + name;
        if (Exists(image)) Remove(path, counts);
        else RelocateFile(path, image, counts);
    }, counts);
    if (counts.failed) ESP_LOGW(TAG, "storage work cleanup incomplete failed=%u", counts.failed);
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "storage work cleanup interrupted: no memory");
}

bool DeleteBookData(const char* book) {
    std::string relative;
    if (!Relative(book, SD_PATH_BOOKS, relative)) return false;
    const auto key = Key(relative);
    if (key.empty()) return false;
    Counts counts;
    for (const auto* suffix : kSuffixes) {
        if (Ends(suffix, ".tmp")) continue;
        for (const bool sharded : {true, false}) {
            const auto candidate = StoredPath(BookRoot(suffix), key, suffix, sharded);
            for (const auto* tail : {"", ".tmp", ".previous", ".tmp.previous"}) Remove(candidate + tail, counts);
        }
        if (!IsCover(suffix)) {
            const auto legacy = LegacyBookPath(book, suffix);
            for (const auto* tail : {"", ".tmp", ".previous"}) Remove(legacy + tail, counts);
        }
    }
    Remove(std::string(book) + ".tmp", counts);
    const auto cover = LegacyBookPath(book, ".a2l8");
    bool ok = true;
    const auto mono = LegacyBookPath(book, ".a2i1");
    const bool have_cover = Exists(cover, &ok) || Exists(mono, &ok) || Exists(cover + ".tmp", &ok) || Exists(mono + ".tmp", &ok);
    if (!ok) ++counts.failed;
    if (ok && have_cover) {
        const auto stem = cover.substr(0, cover.size() - 5);
        bool shared = false;
        for (const auto* suffix : {".txt", ".epub", ".ebook", ".epu", ".ebo"})
            shared |= Exists(stem + suffix, &ok);
        if (!ok) ++counts.failed;
        if (ok && !shared) {
            for (const auto* suffix : {".a2l8", ".a2i1"}) {
                const auto path = LegacyBookPath(book, suffix);
                Remove(path, counts);
                Remove(path + ".tmp", counts);
            }
        }
    }
    const std::string prefix = std::string(book) + ".";
    const auto parent = prefix.substr(0, prefix.find_last_of('/'));
    Walk(parent.c_str(), [&](const std::string& path) {
        if (path.compare(0, prefix.size(), prefix) == 0 && WorkName(path.substr(path.find_last_of('/') + 1)))
            Remove(path, counts);
    }, counts, false);
    return counts.failed == 0;
}

bool DeleteWallpaperData(const char* wallpaper) {
    std::string relative;
    if (!Relative(wallpaper, SD_PATH_WALLPAPER, relative)) return false;
    const auto key = Key(relative);
    if (key.empty()) return false;
    Counts counts;
    for (const bool sharded : {true, false}) {
        const auto path = StoredPath(SD_PATH_WALLPAPER_META, key, ".json", sharded);
        for (const auto* tail : {"", ".tmp", ".previous"}) Remove(path + tail, counts);
    }
    for (const auto* suffix : {".meta.json", ".meta"}) {
        const auto legacy = std::string(wallpaper) + suffix;
        Remove(legacy, counts);
        Remove(legacy + ".tmp", counts);
    }
    return counts.failed == 0;
}

bool Relocate(const char* from, const char* to) try {
    Counts counts;
    auto relocate = [&](const std::string& destination) {
        const auto original = std::string(from) + destination.substr(std::strlen(to));
        std::string relative, original_relative;
        const bool book = Book(destination) && Relative(destination.c_str(), SD_PATH_BOOKS, relative);
        const char* root = book ? SD_PATH_BOOKS : SD_PATH_WALLPAPER;
        if (!Relative(destination.c_str(), root, relative) || !Relative(original.c_str(), root, original_relative)) return;
        const auto source_key = Key(original_relative), target_key = Key(relative);
        if (source_key.empty() || target_key.empty()) { ++counts.failed; return; }
        if (book) {
            for (const auto* suffix : kSuffixes) {
                for (const bool sharded : {true, false})
                    RelocateFile(StoredPath(BookRoot(suffix), source_key, suffix, sharded),
                                 StoredPath(BookRoot(suffix), target_key, suffix, sharded), counts);
                RelocateFile(LegacyBookPath(original.c_str(), suffix), LegacyBookPath(destination.c_str(), suffix), counts);
            }
        } else {
            for (const bool sharded : {true, false})
                RelocateFile(StoredPath(SD_PATH_WALLPAPER_META, source_key, ".json", sharded),
                             StoredPath(SD_PATH_WALLPAPER_META, target_key, ".json", sharded), counts);
            for (const auto* suffix : {".meta", ".meta.json"})
                RelocateFile(original + suffix, destination + suffix, counts);
        }
    };
    struct stat st{};
    if (stat(to, &st)) {
        ESP_LOGE(TAG, "auxiliary relocation inspect failed errno=%d", errno);
        return false;
    }
    if (S_ISDIR(st.st_mode)) Walk(to, relocate, counts); else relocate(to);
    return !counts.failed;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "auxiliary relocation failed: no memory");
    return false;
}
}
