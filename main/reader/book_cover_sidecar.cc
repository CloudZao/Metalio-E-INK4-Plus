#include "book_cover_sidecar.h"

#include "book_library.h"
#include "ebook_document.h"
#include "epub_document.h"
#include "image_util.h"
#include "reader_image_budget.h"
#include "sd_paths.h"

#include <cstring>
#include <string>
#include <vector>

#include <esp_log.h>
#include <sys/stat.h>
#include <unistd.h>

namespace reader {
namespace {

constexpr const char* TAG = "BookCoverSidecar";
constexpr size_t kSidecarMaxFileBytes = 256 * 1024;

bool IsUnderBooksDir(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    const std::string prefix = std::string(SD_PATH_BOOKS) + "/";
    return std::strncmp(path, prefix.c_str(), prefix.size()) == 0;
}

bool BuildSidecarPath(const char* book_path, char* out, size_t out_sz) {
    if (book_path == nullptr || out == nullptr || out_sz == 0) {
        return false;
    }
    const char* dot = std::strrchr(book_path, '.');
    const char* slash = std::strrchr(book_path, '/');
    if (dot == nullptr || (slash != nullptr && dot < slash)) {
        return false;
    }
    const size_t prefix_len = static_cast<size_t>(dot - book_path);
    constexpr const char kExt[] = ".a2i1";
    constexpr size_t kExtLen = sizeof(kExt);  // 含 '\0'
    if (prefix_len + kExtLen > out_sz) {
        return false;
    }
    std::memcpy(out, book_path, prefix_len);
    std::memcpy(out + prefix_len, kExt, kExtLen);
    return true;
}

bool WriteSidecarBytes(const char* sidecar_path, const std::vector<uint8_t>& bytes) {
    if (sidecar_path == nullptr || bytes.empty() || bytes.size() > kSidecarMaxFileBytes) {
        return false;
    }
    return WriteFileAtomic(sidecar_path, bytes.data(), bytes.size());
}

/** 仅当新图像素面积更大时覆盖（只升不降） */
bool ShouldUpgradeSidecar(const char* sidecar_path, int new_w, int new_h) {
    int old_w = 0;
    int old_h = 0;
    if (!PeekA2i1FileSize(sidecar_path, &old_w, &old_h)) {
        return true;
    }
    const uint32_t old_area = static_cast<uint32_t>(old_w) * static_cast<uint32_t>(old_h);
    const uint32_t new_area = static_cast<uint32_t>(new_w) * static_cast<uint32_t>(new_h);
    return new_area > old_area;
}

bool EncodeCoverBytesToSidecar(const char* book_path, const uint8_t* data, size_t len,
                               char* sidecar, size_t sidecar_sz) {
    if (data == nullptr || len == 0 || !BuildSidecarPath(book_path, sidecar, sidecar_sz)) {
        return false;
    }

    std::vector<uint8_t> file_bytes;
    int new_w = 0;
    int new_h = 0;
    if (ValidateA2i1Bytes(data, len)) {
        if (!PeekA2i1Size(data, len, &new_w, &new_h)) {
            return false;
        }
        if (!ShouldUpgradeSidecar(sidecar, new_w, new_h)) {
            ESP_LOGI(TAG, "sidecar keep (a2i1 not larger): %s", sidecar);
            return true;
        }
        file_bytes.assign(data, data + len);
    } else {
        RasterImage l8;
        if (!DecodeImageToL8(data, len, kCoverSidecarTargetW, kCoverSidecarTargetH, l8) ||
            l8.empty()) {
            ESP_LOGW(TAG, "sidecar decode fail: %s", book_path);
            return false;
        }
        new_w = l8.width;
        new_h = l8.height;
        if (!ShouldUpgradeSidecar(sidecar, new_w, new_h)) {
            ESP_LOGI(TAG, "sidecar keep (decode not larger): %s", sidecar);
            return true;
        }
        if (!EncodeL8ToA2i1(l8, file_bytes) ||
            !ValidateA2i1Bytes(file_bytes.data(), file_bytes.size())) {
            ESP_LOGW(TAG, "sidecar encode fail: %s", book_path);
            return false;
        }
    }

    if (!WriteSidecarBytes(sidecar, file_bytes)) {
        ESP_LOGW(TAG, "sidecar write fail: %s", sidecar);
        return false;
    }
    ESP_LOGI(TAG, "sidecar saved %s (%u bytes %dx%d)", sidecar,
             static_cast<unsigned>(file_bytes.size()), new_w, new_h);
    return true;
}

}  // namespace

bool BookCoverSidecarPath(const char* book_path, char* out, size_t out_sz) {
    if (!IsUnderBooksDir(book_path)) {
        return false;
    }
    return BuildSidecarPath(book_path, out, out_sz);
}

bool BookCoverSidecarAdequate(const char* book_path, int need_w, int need_h) {
    char sidecar[192];
    if (!BookCoverSidecarPath(book_path, sidecar, sizeof(sidecar))) {
        return false;
    }
    int w = 0;
    int h = 0;
    if (!PeekA2i1FileSize(sidecar, &w, &h)) {
        return false;
    }
    return RasterCoversNeed(w, h, need_w, need_h);
}

void DeleteBookCoverSidecar(const char* book_path) {
    char sidecar[192];
    if (!BookCoverSidecarPath(book_path, sidecar, sizeof(sidecar))) {
        return;
    }
    unlink(sidecar);
    unlink((std::string(sidecar) + ".tmp").c_str());
}

bool SaveBookCoverSidecarFromL8(const char* book_path, const RasterImage& l8) {
    if (book_path == nullptr || book_path[0] == '\0' || l8.empty()) {
        return false;
    }
    if (!IsUnderBooksDir(book_path)) {
        return false;
    }
    char sidecar[192];
    if (!BuildSidecarPath(book_path, sidecar, sizeof(sidecar))) {
        return false;
    }
    if (!ShouldUpgradeSidecar(sidecar, l8.width, l8.height)) {
        ESP_LOGI(TAG, "sidecar keep from L8 (not larger) %s", sidecar);
        return true;
    }
    std::vector<uint8_t> file_bytes;
    if (!EncodeL8ToA2i1(l8, file_bytes) || !ValidateA2i1Bytes(file_bytes.data(), file_bytes.size())) {
        ESP_LOGW(TAG, "sidecar encode from L8 fail: %s", book_path);
        return false;
    }
    if (!WriteSidecarBytes(sidecar, file_bytes)) {
        ESP_LOGW(TAG, "sidecar write fail: %s", sidecar);
        return false;
    }
    ESP_LOGI(TAG, "sidecar ok from L8 %s (%u bytes %ux%u)", sidecar,
             static_cast<unsigned>(file_bytes.size()), l8.width, l8.height);
    return true;
}

bool SaveBookCoverSidecarFromBytes(const char* book_path, const uint8_t* data, size_t len) {
    if (book_path == nullptr || book_path[0] == '\0' || data == nullptr || len == 0) {
        return false;
    }
    if (!IsUnderBooksDir(book_path)) {
        ESP_LOGW(TAG, "sidecar skip: path outside books dir %s", book_path);
        return false;
    }
    char sidecar[192];
    if (!EncodeCoverBytesToSidecar(book_path, data, len, sidecar, sizeof(sidecar))) {
        return false;
    }
    ESP_LOGI(TAG, "sidecar ok from bytes %s (%u in)", sidecar, static_cast<unsigned>(len));
    return true;
}

bool SaveBookCoverSidecar(const char* book_path) {
    if (book_path == nullptr || book_path[0] == '\0') {
        return false;
    }
    if (!IsUnderBooksDir(book_path)) {
        ESP_LOGW(TAG, "sidecar skip: path outside books dir %s", book_path);
        return false;
    }

    const BookFormat fmt = DetectFormatByPath(book_path);
    if (fmt != BookFormat::kEbook && fmt != BookFormat::kEpub) {
        return false;
    }

    struct stat st {};
    if (stat(book_path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        ESP_LOGW(TAG, "sidecar skip: missing book %s", book_path);
        return false;
    }

    // 已有权威档足够则跳过 Open，加快二次进入
    if (BookCoverSidecarAdequate(book_path, kCoverSidecarTargetW, kCoverSidecarTargetH)) {
        ESP_LOGI(TAG, "sidecar already adequate: %s", book_path);
        return true;
    }

    char sidecar[192];
    std::vector<uint8_t> cover_bytes;

    if (fmt == BookFormat::kEbook) {
        if (!EbookDocument::PeekCoverImageBytes(book_path, cover_bytes)) {
            DeleteBookCoverSidecar(book_path);
            ESP_LOGW(TAG, "no cover in ebook: %s", book_path);
            return false;
        }
    } else {
        EpubDocument doc;
        if (!doc.Open(book_path)) {
            ESP_LOGW(TAG, "sidecar skip: epub open fail %s", book_path);
            return false;
        }
        RasterImage l8;
        const bool ok = doc.DecodeCoverImageToL8(kCoverSidecarTargetW, kCoverSidecarTargetH, l8);
        doc.Close();
        if (!ok || l8.empty()) {
            DeleteBookCoverSidecar(book_path);
            ESP_LOGW(TAG, "no cover in epub: %s", book_path);
            return false;
        }
        return SaveBookCoverSidecarFromL8(book_path, l8);
    }

    return EncodeCoverBytesToSidecar(book_path, cover_bytes.data(), cover_bytes.size(), sidecar,
                                     sizeof(sidecar));
}

}  // namespace reader
