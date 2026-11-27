#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <esp_log.h>
#include <sys/stat.h>
#include <unistd.h>
#include "infra/storage/file_write.h"
#include "book_cover_sidecar.h"
#include "infra/storage/sd_data_layout.h"
#include "infra/storage/sd_paths.h"
#include "book_library.h"
#include "ebook_document.h"
#include "epub_document.h"
#include "infra/image/image_decode.h"
#include "reader_image_budget.h"
#include "list_thumbnail_cache.h"

static constexpr const char* TAG = "BookCoverSidecar";
static constexpr size_t kSidecarMaxFileBytes = 2 * 1024 * 1024;

static bool IsUnderBooksDir(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    const std::string prefix = std::string(SD_PATH_BOOKS) + "/";
    return std::strncmp(path, prefix.c_str(), prefix.size()) == 0;
}

static bool BuildSidecarPath(const char* book_path, char* out, size_t out_sz) {
    if (book_path == nullptr || out == nullptr || out_sz == 0) {
        return false;
    }
    const std::string gray = SdDataBookPath(book_path, ".a2l8");
    const std::string legacy = SdDataBookPath(book_path, ".a2i1");
    if (gray.empty() || legacy.empty()) {
        return false;
    }
    const std::string& path =
        access(gray.c_str(), F_OK) == 0 ? gray : (access(legacy.c_str(), F_OK) == 0 ? legacy : gray);
    if (path.size() + 1 > out_sz) {
        return false;
    }
    std::memcpy(out, path.c_str(), path.size() + 1);
    return true;
}

static bool WriteSidecarBytes(const char* sidecar_path, const std::vector<uint8_t>& bytes) {
    if (sidecar_path == nullptr || bytes.empty() || bytes.size() > kSidecarMaxFileBytes) {
        return false;
    }
    return WriteFileAtomic(sidecar_path, bytes.data(), bytes.size());
}

// 仅当新图像素面积更大时覆盖（只升不降）
static bool ShouldUpgradeSidecar(const char* sidecar_path, int new_w, int new_h) {
    if (std::strstr(sidecar_path, ".a2i1") && access(sidecar_path, F_OK) == 0) {
        return false;
    }
    int old_w = 0;
    int old_h = 0;
    if (!PeekRasterFileSize(sidecar_path, &old_w, &old_h)) {
        return true;
    }
    const uint32_t old_area = static_cast<uint32_t>(old_w) * static_cast<uint32_t>(old_h);
    const uint32_t new_area = static_cast<uint32_t>(new_w) * static_cast<uint32_t>(new_h);
    return new_area > old_area;
}

static bool EncodeCoverBytesToSidecar(const char* book_path, const uint8_t* data, size_t len, char* sidecar,
                                      size_t sidecar_sz) {
    if (data == nullptr || len == 0 || !BuildSidecarPath(book_path, sidecar, sidecar_sz)) {
        return false;
    }

    std::vector<uint8_t> file_bytes;
    int new_w = 0;
    int new_h = 0;
    if (ValidateA2i1Bytes(data, len)) {
        if (access(sidecar, F_OK) == 0) {
            return true;
        }
        const size_t path_len = std::strlen(sidecar);
        if (path_len >= 5) {
            std::memcpy(sidecar + path_len - 5, ".a2i1", 6);
        }
        if (!PeekA2i1Size(data, len, &new_w, &new_h)) {
            return false;
        }
        if (!ShouldUpgradeSidecar(sidecar, new_w, new_h)) {
            ESP_LOGD(TAG, "sidecar retained format=a2i1 width=%d height=%d", new_w, new_h);
            return true;
        }
        file_bytes.assign(data, data + len);
    } else {
        RasterImage l8;
        if (!DecodeImageToL8(data, len, kCoverSidecarTargetW, kCoverSidecarTargetH, l8) || l8.empty()) {
            ESP_LOGW(TAG, "sidecar decode failed bytes=%u", static_cast<unsigned>(len));
            return false;
        }
        new_w = l8.width;
        new_h = l8.height;
        if (!ShouldUpgradeSidecar(sidecar, new_w, new_h)) {
            ESP_LOGD(TAG, "sidecar retained width=%d height=%d", new_w, new_h);
            return true;
        }
        if (!EncodeL8ToA2l8(l8, file_bytes) || !ValidateA2l8Bytes(file_bytes.data(), file_bytes.size())) {
            ESP_LOGW(TAG, "sidecar encode failed width=%d height=%d", new_w, new_h);
            return false;
        }
    }

    if (!WriteSidecarBytes(sidecar, file_bytes)) {
        ESP_LOGW(TAG, "sidecar write failed bytes=%u", static_cast<unsigned>(file_bytes.size()));
        return false;
    }
    ESP_LOGD(TAG, "sidecar saved bytes=%u width=%d height=%d", static_cast<unsigned>(file_bytes.size()), new_w, new_h);
    return true;
}

bool BookCoverSidecarPath(const char* book_path, char* out, size_t out_sz) {
    if (!IsUnderBooksDir(book_path)) {
        return false;
    }
    return BuildSidecarPath(book_path, out, out_sz);
}

bool BookCoverSidecarAdequate(const char* book_path, int need_w, int need_h) {
    char sidecar[SD_FILE_PATH_CAPACITY];
    if (!BookCoverSidecarPath(book_path, sidecar, sizeof(sidecar))) {
        return false;
    }
    int w = 0;
    int h = 0;
    if (!PeekRasterFileSize(sidecar, &w, &h)) {
        return false;
    }
    if (std::strstr(sidecar, ".a2i1")) {
        return true;
    }
    return RasterCoversNeed(w, h, need_w, need_h);
}

bool SaveBookCoverSidecar(const char* book_path) {
    if (book_path == nullptr || book_path[0] == '\0') {
        return false;
    }
    if (!IsUnderBooksDir(book_path)) {
        ESP_LOGD(TAG, "sidecar skipped reason=outside_books_root");
        return false;
    }

    const BookFormat fmt = DetectFormatByPath(book_path);
    if (fmt != BookFormat::kEbook && fmt != BookFormat::kEpub) {
        return false;
    }

    struct stat st{};
    if (stat(book_path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        ESP_LOGW(TAG, "sidecar skipped reason=unavailable_book");
        return false;
    }

    // 已有权威档足够则跳过 Open，加快二次进入
    if (BookCoverSidecarAdequate(book_path, kCoverSidecarTargetW, kCoverSidecarTargetH)) {
        ESP_LOGD(TAG, "sidecar retained reason=adequate");
        return true;
    }

    char sidecar[SD_FILE_PATH_CAPACITY];
    std::vector<uint8_t> cover_bytes;

    if (fmt == BookFormat::kEbook) {
        if (!EbookDocument::PeekCoverImageBytes(book_path, cover_bytes)) {
            DeleteBookCoverSidecar(book_path);
            ESP_LOGW(TAG, "sidecar cover extraction failed format=ebook");
            return false;
        }
    } else {
        EpubDocument doc;
        if (!doc.Open(book_path, false)) {
            ESP_LOGW(TAG, "sidecar skipped reason=epub_open_failed");
            return false;
        }
        RasterImage l8;
        const bool ok = doc.DecodeCoverImageToL8(kCoverSidecarTargetW, kCoverSidecarTargetH, l8);
        doc.Close();
        if (!ok || l8.empty()) {
            DeleteBookCoverSidecar(book_path);
            ESP_LOGW(TAG, "sidecar cover extraction failed format=epub");
            return false;
        }
        return SaveBookCoverSidecarFromL8(book_path, l8);
    }

    return EncodeCoverBytesToSidecar(book_path, cover_bytes.data(), cover_bytes.size(), sidecar, sizeof(sidecar));
}

bool SaveBookCoverSidecarFromL8(const char* book_path, const RasterImage& l8) {
    if (book_path == nullptr || book_path[0] == '\0' || l8.empty()) {
        return false;
    }
    if (!IsUnderBooksDir(book_path)) {
        return false;
    }
    char sidecar[SD_FILE_PATH_CAPACITY];
    if (!BuildSidecarPath(book_path, sidecar, sizeof(sidecar))) {
        return false;
    }
    if (!ShouldUpgradeSidecar(sidecar, l8.width, l8.height)) {
        ESP_LOGD(TAG, "sidecar retained width=%u height=%u", l8.width, l8.height);
        return true;
    }
    std::vector<uint8_t> file_bytes;
    if (!EncodeL8ToA2l8(l8, file_bytes) || !ValidateA2l8Bytes(file_bytes.data(), file_bytes.size())) {
        ESP_LOGW(TAG, "sidecar encode failed width=%u height=%u", l8.width, l8.height);
        return false;
    }
    if (!WriteSidecarBytes(sidecar, file_bytes)) {
        ESP_LOGW(TAG, "sidecar write failed bytes=%u", static_cast<unsigned>(file_bytes.size()));
        return false;
    }
    ESP_LOGD(TAG, "sidecar saved bytes=%u width=%u height=%u", static_cast<unsigned>(file_bytes.size()), l8.width,
             l8.height);
    return true;
}

void DeleteBookCoverSidecar(const char* book_path) {
    char sidecar[SD_FILE_PATH_CAPACITY];
    if (!BookCoverSidecarPath(book_path, sidecar, sizeof(sidecar))) {
        return;
    }
    unlink(sidecar);
    unlink((std::string(sidecar) + ".tmp").c_str());
}

static bool CoverLoadCancelled(void* context) {
    const auto* abort = static_cast<const std::atomic<bool>*>(context);
    return abort && abort->load();
}

static bool LoadCoverFromDocument(BookInfo& info, RasterImage& auth_out, bool& unavailable,
                                   const std::atomic<bool>* abort) {
    unavailable = info.format == BookFormat::kTxt;
    auth_out.Reset();
    if (info.format == BookFormat::kEpub) {
        EpubDocument doc;
        if (!doc.Open(info.path.c_str(), false) || (abort && abort->load())) {
            return false;
        }
        if (!doc.Title().empty()) {
            info.title = doc.Title();
        }
        if (!doc.Author().empty()) {
            info.author = doc.Author();
        }
        if (doc.CoverHref().empty()) {
            unavailable = true;
            doc.Close();
            return false;
        }
        const bool ok = doc.DecodeCoverImageToL8(kCoverSidecarTargetW, kCoverSidecarTargetH, auth_out, abort);
        if (!ok) {
            auth_out.Reset();
        }
        doc.Close();
        return ok;
    }

    if (info.format == BookFormat::kEbook) {
        EbookDocument doc;
        if (!doc.Open(info.path.c_str()) || (abort && abort->load())) {
            return false;
        }
        if (!doc.Title().empty()) {
            info.title = doc.Title();
        }
        if (!doc.Author().empty()) {
            info.author = doc.Author();
        }
        std::vector<uint8_t> bytes;
        const bool ok =
            doc.LoadCoverBytes(bytes) && !bytes.empty() && !(abort && abort->load()) &&
            DecodeImageToL8(bytes.data(), bytes.size(), kCoverSidecarTargetW, kCoverSidecarTargetH, auth_out,
                            CoverLoadCancelled, const_cast<std::atomic<bool>*>(abort));
        if (!ok) {
            auth_out.Reset();
        }
        doc.Close();
        return ok;
    }

    return false;
}

static bool LoadSidecarScaled(const BookInfo& info, int max_w, int max_h, RasterImage& out,
                                const std::atomic<bool>* abort = nullptr) {
    out.Reset();
    char sidecar[SD_FILE_PATH_CAPACITY];
    if (!BookCoverSidecarPath(info.path.c_str(), sidecar, sizeof(sidecar))) {
        return false;
    }
    if (!DecodeImageFileToL8(sidecar, max_w, max_h, out, CoverLoadCancelled,
                             const_cast<std::atomic<bool>*>(abort)) || out.empty()) {
        out.Reset();
        return false;
    }
    return true;
}

bool BookCoverLoader_TryLoadBookDetailSidecar(const BookInfo& info, int max_w, int max_h, RasterImage& out) {
    out.Reset();
    if (max_w <= 0 || max_h <= 0 || info.path.empty()) {
        return false;
    }
    if (!BookCoverSidecarAdequate(info.path.c_str(), max_w, max_h)) {
        return false;
    }
    if (!LoadSidecarScaled(info, max_w, max_h, out)) {
        return false;
    }
    ESP_LOGD(TAG, "cover sidecar ready");
    return true;
}

bool BookCoverLoader_TryLoadBookSidecarPreview(const BookInfo& info, int max_w, int max_h, RasterImage& out) {
    out.Reset();
    if (max_w <= 0 || max_h <= 0 || info.path.empty()) {
        return false;
    }
    if (!LoadSidecarScaled(info, max_w, max_h, out)) {
        return false;
    }
    ESP_LOGD(TAG, "cover preview ready width=%u height=%u", out.width, out.height);
    return true;
}

bool BookCoverLoader_LoadBookDetailCover(BookInfo& info, int max_w, int max_h, RasterImage& out,
                                        const std::atomic<bool>* abort) {
    out.Reset();
    if (max_w <= 0 || max_h <= 0 || info.path.empty() || (abort && abort->load())) {
        return false;
    }
    if (ListThumbnailUnavailable(info.path.c_str(), info.file_size, info.mtime)) {
        return false;
    }
    // 旁路分辨率足够时直接缩放到槽位。
    if (BookCoverSidecarAdequate(info.path.c_str(), max_w, max_h) &&
        LoadSidecarScaled(info, max_w, max_h, out, abort)) {
        return true;
    }
    if (abort && abort->load()) {
        out.Reset();
        return false;
    }
    // 内嵌封面按旁路目标尺寸解码，保存后再缩放到显示槽位。
    RasterImage auth;
    bool unavailable = false;
    if (LoadCoverFromDocument(info, auth, unavailable, abort) && !auth.empty()) {
        if (abort && abort->load()) {
            return false;
        }
        SaveBookCoverSidecarFromL8(info.path.c_str(), auth);
        if (!ScaleRasterToFit(auth, max_w, max_h, out) || out.empty()) {
            out = std::move(auth);
            out.BindDsc();
        }
        ESP_LOGD(TAG, "embedded cover ready width=%u height=%u", out.width, out.height);
        return !out.empty();
    }
    if (unavailable && !(abort && abort->load())) {
        CacheListThumbnail(info.path.c_str(), max_w, max_h, info.file_size, info.mtime, out);
        ESP_LOGD(TAG, "book has no embedded cover format=%u", static_cast<unsigned>(info.format));
    }
    return false;
}
