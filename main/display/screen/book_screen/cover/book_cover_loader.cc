#include "book_screen/cover/book_cover_loader.h"
#include "sd_paths.h"

#include "reader/book_cover_sidecar.h"
#include "reader/ebook_document.h"
#include "reader/epub_document.h"
#include "reader/image_util.h"
#include "reader/reader_image_budget.h"
#include "reader/list_thumbnail_cache.h"

#include <cstring>
#include <vector>
#include <utility>

#include <esp_log.h>

static constexpr const char* TAG = "BookCover";

static bool LoadCoverFromDocument(reader::BookInfo& info, reader::RasterImage& auth_out, bool& unavailable) {
    unavailable = info.format == reader::BookFormat::kTxt;
    auth_out.Reset();
    if (info.format == reader::BookFormat::kEpub) {
        reader::EpubDocument doc;
        if (!doc.Open(info.path.c_str(), false)) {
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
        const bool ok = doc.DecodeCoverImageToL8(reader::kCoverSidecarTargetW, reader::kCoverSidecarTargetH, auth_out);
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
        const bool ok = doc.LoadCoverBytes(bytes) && !bytes.empty() &&
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
    char sidecar[SD_FILE_PATH_CAPACITY];
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
    ESP_LOGD(TAG, "cover sidecar ready");
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
    ESP_LOGD(TAG, "cover preview ready width=%u height=%u", out.width, out.height);
    return true;
}

bool BookCoverLoader_LoadBookDetailCover(reader::BookInfo& info, int max_w, int max_h, reader::RasterImage& out) {
    out.Reset();
    if (max_w <= 0 || max_h <= 0 || info.path.empty()) {
        return false;
    }
    if (reader::ListThumbnailUnavailable(info.path.c_str(), info.file_size, info.mtime)) {
        return false;
    }
    // 旁路分辨率足够时直接缩放到槽位。
    if (BookCoverLoader_TryLoadBookDetailSidecar(info, max_w, max_h, out)) {
        return true;
    }
    // 内嵌封面按旁路目标尺寸解码，保存后再缩放到显示槽位。
    reader::RasterImage auth;
    bool unavailable = false;
    if (LoadCoverFromDocument(info, auth, unavailable) && !auth.empty()) {
        reader::SaveBookCoverSidecarFromL8(info.path.c_str(), auth);
        if (!reader::ScaleRasterToFit(auth, max_w, max_h, out) || out.empty()) {
            out = std::move(auth);
            out.BindDsc();
        }
        ESP_LOGD(TAG, "embedded cover ready width=%u height=%u", out.width, out.height);
        return !out.empty();
    }
    if (unavailable) {
        reader::CacheListThumbnail(info.path.c_str(), max_w, max_h, info.file_size, info.mtime, out);
        ESP_LOGD(TAG, "book has no embedded cover format=%u", static_cast<unsigned>(info.format));
    }
    return false;
}
