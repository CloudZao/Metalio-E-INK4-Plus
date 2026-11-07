#include "book_screen/book_cloud_library.h"

#include "api_endpoints.h"
#include "assets/lang_config.h"
#include "board.h"
#include "book_cover_sidecar.h"
#include "power_policy.h"
#include "reader/book_home_snapshot.h"
#include "reader/book_library.h"
#include "reader/book_progress_cache.h"
#include "reader/book_session.h"
#include "reader/file_size.h"
#include "reader/http_download_file.h"
#include "reader/image_util.h"
#include "reader/reader_image_budget.h"
#include "sd_paths.h"
#include "SdCardManager.hpp"

#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <sys/stat.h>
#include <unistd.h>

namespace reader {

static constexpr const char* TAG = "BookCloud";
static bool SaveCoverFile(const char* book_path, const char* cover_path) {
    RasterImage image;
    return DecodeImageFileToL8(cover_path, kCoverSidecarTargetW, kCoverSidecarTargetH, image) &&
           SaveBookCoverSidecarFromL8(book_path, image);
}

std::string CloudBookTask::FileExtension() const {
    if (format == "EBOOK_V1" || format == "EBOOK") {
        return ".ebook";
    }
    if (format == "EPUB") {
        return ".epub";
    }
    if (format == "TXT") {
        return ".txt";
    }
    return ".ebook";
}

std::string CloudBookTask::LocalPath() const {
    std::string path = SD_PATH_BOOKS;
    if (!path.empty() && path.back() != '/') {
        path.push_back('/');
    }
    // 优先用接口 name（如「三国志.ebook」）；去掉路径分隔，避免穿越目录
    std::string file = name;
    if (!file.empty()) {
        const size_t slash = file.find_last_of("/\\");
        if (slash != std::string::npos) {
            file = file.substr(slash + 1);
        }
    }
    if (file.empty() || file == "." || file == "..") {
        file = book_id.empty() ? "book" : book_id;
        file.append(FileExtension());
    } else if (DetectFormatByPath(file.c_str()) == BookFormat::kUnknown) {
        // name 无已知扩展名时补上，保证书库扫描能识别
        file.append(FileExtension());
    }
    if (!SdIsValidFileName(file.c_str())) return {};
    path.append(file);
    return path;
}

bool CloudBookTask::IsDownloaded() const {
    struct stat st {};
    const std::string path = LocalPath();
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    return st.st_size > 0;
}

std::string FormatCloudFileSize(uint64_t bytes) {
    return FormatFileSizeString(bytes);
}

bool DownloadCloudBook(const CloudBookTask& task, std::string& err_out,
                       CloudDownloadProgressFn on_progress, void* progress_user, DownloadGate* gate,
                       const char* cover_path, const char* save_path, bool overwrite) {
    err_out.clear();
    PowerNeedHold hold_net(PowerNeed::OtaDownload);
    if (gate != nullptr && gate->IsCancelled()) {
        err_out = Lang::Strings::BOOK_CANCELLED;
        return false;
    }
    if (!Board::GetInstance().EnsureNetworkReady()) {
        err_out = Lang::Strings::BOOK_NET_NOT_READY;
        return false;
    }
    if (!SdCardManager::GetInstance().IsMounted()) {
        err_out = Lang::Strings::BOOK_NO_SD_SHORT;
        return false;
    }
    if (!SdEnsureAppLayout()) {
        err_out = Lang::Strings::BOOK_STORAGE_UNAVAIL;
        return false;
    }
    if (task.download_url.empty()) {
        err_out = Lang::Strings::BOOK_NO_DOWNLOAD_URL;
        return false;
    }

    const std::string final_path = save_path != nullptr ? save_path : task.LocalPath();
    // 必须落在书库目录下，防止异常 name 写出目录外
    const std::string books_prefix = std::string(SD_PATH_BOOKS) + "/";
    if (final_path.rfind(books_prefix, 0) != 0 && final_path != SD_PATH_BOOKS) {
        err_out = Lang::Strings::BOOK_PATH_INVALID;
        return false;
    }
    const std::string tmp_path = final_path + ".tmp";

    if (!DownloadHttpToFile(task.download_url.c_str(), tmp_path.c_str(),
                            task.sha256.empty() ? nullptr : task.sha256.c_str(), task.file_size,
                            on_progress, progress_user, err_out, gate)) {
        return false;
    }
    if (gate != nullptr && gate->IsCancelled()) {
        unlink(tmp_path.c_str());
        err_out = Lang::Strings::BOOK_CANCELLED;
        return false;
    }

    if (!CommitDownloadedFile(tmp_path, final_path, overwrite, err_out)) {
        unlink(tmp_path.c_str());
        return false;
    }
    ESP_LOGI(TAG, "saved cloud book -> %s", final_path.c_str());

    DeleteBookCoverSidecar(final_path.c_str());
    // 旁路封面复用列表磁盘缓存；无缓存时分块下载，再失败则抽取书内封面。
    const BookFormat fmt = DetectFormatByPath(final_path.c_str());
    if (fmt == BookFormat::kEbook || fmt == BookFormat::kEpub) {
        bool sidecar_ok = false;
        if (cover_path != nullptr) {
            if (SaveCoverFile(final_path.c_str(), cover_path)) {
                sidecar_ok = true;
                ESP_LOGI(TAG, "cover sidecar ok source=list_cache %s", final_path.c_str());
            } else {
                ESP_LOGW(TAG, "cover sidecar list_cache encode fail %s", final_path.c_str());
            }
        }
        if (!sidecar_ok && !task.cover_image_url.empty()) {
            ESP_LOGI(TAG, "cover sidecar try coverImageUrl HTTP");
            std::string cover_err;
            auto cache = DownloadHttpToCache(task.cover_image_url.c_str(), cover_err, gate);
            if (cache && SaveCoverFile(final_path.c_str(), cache->path.c_str())) {
                sidecar_ok = true;
                ESP_LOGI(TAG, "cover sidecar ok source=coverImageUrl %s", final_path.c_str());
            } else {
                ESP_LOGW(TAG, "cover sidecar coverImageUrl fail %s err=%s", final_path.c_str(),
                         cover_err.c_str());
            }
        } else if (!sidecar_ok) {
            ESP_LOGI(TAG, "cover sidecar no coverImageUrl, try ebook embed");
        }
        if (!sidecar_ok) {
            if (SaveBookCoverSidecar(final_path.c_str())) {
                ESP_LOGI(TAG, "cover sidecar ok source=ebook_embed %s", final_path.c_str());
            } else {
                ESP_LOGW(TAG, "cover sidecar miss %s", final_path.c_str());
            }
        }
    }
    // 磁盘书库已变；读侧 Ensure/首页 Load 前勿复用旧缓存
    InvalidateBookLibraryCache();
    return true;
}

bool DeleteCloudBookLocal(const CloudBookTask& task, std::string& err_out) {
    err_out.clear();
    const std::string path = task.LocalPath();
    const std::string books_prefix = std::string(SD_PATH_BOOKS) + "/";
    if (path.rfind(books_prefix, 0) != 0) {
        err_out = Lang::Strings::BOOK_PATH_INVALID;
        return false;
    }
    if (!task.IsDownloaded()) {
        err_out = Lang::Strings::BOOK_NO_LOCAL_FILE;
        return false;
    }
    // 先 Peek/缓存 再删 .pos：供首页聚合扣减
    BookSession::ProgressPeek peek;
    if (!book_progress_cache::TryGet(path.c_str(), peek)) {
        peek = BookSession::PeekProgress(path.c_str());
    }
    if (unlink(path.c_str()) != 0) {
        err_out = Lang::Strings::BOOK_DELETE_FAILED;
        return false;
    }
    unlink((path + ".tmp").c_str());
    unlink((path + ".pos").c_str());      // 进度+终身累计+当日阅读秒
    unlink((path + ".pos.tmp").c_str());  // 原子写残留
    DeleteBookCoverSidecar(path.c_str());
    if (DetectFormatByPath(path.c_str()) == BookFormat::kTxt) {
        unlink((path + ".idx").c_str());
    }
    InvalidateBookLibraryCache();
    book_progress_cache::Erase(path.c_str());
    book_home_snapshot::RemoveBook(path.c_str());
    book_home_snapshot::SubtractAggregateContribution(peek.reading_seconds,
                                                      peek.progress_x10 >= 1000);
    ESP_LOGI(TAG, "deleted local cloud book %s", path.c_str());
    return true;
}


BookInfo ToBookInfo(const CloudBookTask& task) {
    BookInfo info;
    info.path = task.LocalPath();
    info.title = task.title;
    info.format = DetectFormatByPath(info.path.c_str());
    struct stat st {};
    if (stat(info.path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        info.file_size = static_cast<size_t>(st.st_size);
        info.mtime = static_cast<int64_t>(st.st_mtime);
    } else {
        info.file_size = static_cast<size_t>(task.file_size);
        info.mtime = 0;
    }
    return info;
}

}  // namespace reader
