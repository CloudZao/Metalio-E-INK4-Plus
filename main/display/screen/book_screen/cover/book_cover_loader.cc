#include "book_screen/cover/book_cover_loader.h"

#include "reader/book_cover_sidecar.h"
#include "reader/ebook_document.h"
#include "reader/epub_document.h"
#include "reader/image_util.h"
#include "reader/reader_image_budget.h"

#include <cstring>
#include <vector>

#include <esp_log.h>

static constexpr const char* TAG = "BookCover";

static const char* CoverBaseName(const std::string& path) {
    const char* s = std::strrchr(path.c_str(), '/');
    return (s != nullptr && s[1] != '\0') ? (s + 1) : path.c_str();
}

static bool LoadCoverFromDocument(reader::BookInfo& info, reader::RasterImage& auth_out) {
    auth_out.Reset();
    if (info.format == reader::BookFormat::kEpub) {
        reader::EpubDocument doc;
        if (!doc.Open(info.path.c_str())) {
            return false;
        }
        if (!doc.Title().empty()) {
            info.title = doc.Title();
        }
        if (!doc.Author().empty()) {
            info.author = doc.Author();
        }
        const bool ok = doc.DecodeCoverImageToL8(reader::kCoverSidecarTargetW,
                                                 reader::kCoverSidecarTargetH, auth_out);
        if (!ok) {
            auth_out.Reset();
        }
        doc.Close();
        return ok;
    }

    if (info.format == reader::BookFormat::kEbook) {
        reader::EbookDocument doc;
        if (!doc.Open(info.path.c_str())) {
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
            doc.LoadCoverBytes(bytes) && !bytes.empty() &&
            reader::DecodeImageToL8(bytes.data(), bytes.size(), reader::kCoverSidecarTargetW,
                                    reader::kCoverSidecarTargetH, auth_out);
        if (!ok) {
            auth_out.Reset();
        }
        doc.Close();
        return ok;
    }

    return false;
}

static bool LoadSidecarScaled(const reader::BookInfo& info, int max_w, int max_h, reader::RasterImage& out) {
    out.Reset();
    char sidecar[192];
    if (!reader::BookCoverSidecarPath(info.path.c_str(), sidecar, sizeof(sidecar))) {
        return false;
    }
    if (!reader::DecodeImageFileToL8(sidecar, max_w, max_h, out) || out.empty()) {
        out.Reset();
        return false;
    }
    return true;
}


bool BookCoverLoader_TryLoadBookDetailSidecar(const reader::BookInfo& info, int max_w, int max_h,
                                              reader::RasterImage& out) {
    out.Reset();
    if (max_w <= 0 || max_h <= 0 || info.path.empty()) {
        return false;
    }
    if (!reader::BookCoverSidecarAdequate(info.path.c_str(), max_w, max_h)) {
        return false;
    }
    if (!LoadSidecarScaled(info, max_w, max_h, out)) {
        return false;
    }
    ESP_LOGI(TAG, "cover ok sidecar %s", CoverBaseName(info.path));
    return true;
}

bool BookCoverLoader_TryLoadBookSidecarPreview(const reader::BookInfo& info, int max_w, int max_h,
                                               reader::RasterImage& out) {
    out.Reset();
    if (max_w <= 0 || max_h <= 0 || info.path.empty()) {
        return false;
    }
    if (!LoadSidecarScaled(info, max_w, max_h, out)) {
        return false;
    }
    ESP_LOGI(TAG, "cover preview sidecar %s (%ux%u)", CoverBaseName(info.path), out.width,
             out.height);
    return true;
}

bool BookCoverLoader_LoadBookDetailCover(reader::BookInfo& info, int max_w, int max_h,
                                         reader::RasterImage& out) {
    out.Reset();
    if (max_w <= 0 || max_h <= 0 || info.path.empty()) {
        return false;
    }
    // 1) 旁路够大：秒开缩放到槽位
    if (BookCoverLoader_TryLoadBookDetailSidecar(info, max_w, max_h, out)) {
        return true;
    }
    // 2) 抽内嵌 → 权威档解码 → 只升不降写旁路 → 缩到槽位显示
    reader::RasterImage auth;
    if (LoadCoverFromDocument(info, auth) && !auth.empty()) {
        reader::SaveBookCoverSidecarFromL8(info.path.c_str(), auth);
        if (!reader::ScaleRasterToFit(auth, max_w, max_h, out) || out.empty()) {
            out = std::move(auth);
            out.BindDsc();
        }
        ESP_LOGI(TAG, "cover ok embed %s (%ux%u)", CoverBaseName(info.path), out.width, out.height);
        return !out.empty();
    }
    if (info.format == reader::BookFormat::kEpub || info.format == reader::BookFormat::kEbook) {
        ESP_LOGW(TAG, "cover miss %s", CoverBaseName(info.path));
    }
    return false;
}
