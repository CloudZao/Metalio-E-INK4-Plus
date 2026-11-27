#include "http_download_file.h"
#include "assets/lang_screen.h"

// xtensa-g++：本文件在 -Os 下曾 ICE
#pragma GCC optimize("O1")

#include <mbedtls/build_info.h>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <fcntl.h>
#include <psa/crypto.h>
#include <sys/stat.h>
#include <unistd.h>
#include "assets/lang_config.h"
#include "boards/common/board/board.h"
#include "boards/common/storage/sd_access.h"
#include "infra/storage/sd_data_layout.h"
#include "infra/storage/sd_paths.h"
#include "infra/storage/sd_work_file.h"
#include "infra/network/endpoints/api_endpoints.h"
#include "infra/network/http/api_http.h"

static void (*s_activity_begin)() = nullptr;
static void (*s_activity_end)() = nullptr;

static constexpr const char* TAG = "HttpDlFile";
static constexpr int kHttpTimeoutMs = 30000;
// 大块读写少 syscall；数据在 PSRAM（栈仍须内 DRAM，写 SD 关 cache）
static constexpr size_t kIoBytes = 32 * 1024;
static constexpr size_t kIoFallback = 2048;

static bool Sha256EqualIgnoreCase(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    for (size_t i = 0; i < 64; i++) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca == 0 || cb == 0) {
            return ca == cb;
        }
        if (std::tolower(ca) != std::tolower(cb)) {
            return false;
        }
    }
    return true;
}

static void ReportProgress(HttpDownloadProgressFn on_progress, void* progress_user, uint64_t total,
                           uint64_t total_expected, int& last_percent) {
    if (on_progress == nullptr || total_expected == 0) {
        return;
    }
    int percent = static_cast<int>((total * 100ULL) / total_expected);
    if (percent > 99) {
        percent = 99;
    }
    if (percent != last_percent) {
        last_percent = percent;
        on_progress(percent, progress_user);
    }
}

static std::shared_ptr<HttpTempFile> HttpDownloadCreateHttpTempFile(std::string& err_out, HttpCacheApp app) try {
    err_out.clear();
    SdAccessTransaction transaction;
    const unsigned index = static_cast<unsigned>(app);
    static const SdWorkArea areas[] = {SdWorkArea::Transfer, SdWorkArea::Books, SdWorkArea::Wallpaper};
    if (index >= 3) {
        err_out = LangString_BOOK_BAD_ARG;
        return {};
    }
    const char* directory_path = SdWorkDirectory(areas[index]);
    if (!transaction || !SdEnsureDirectory(directory_path)) {
        err_out = LangString_BOOK_WRITE_FAIL;
        return {};
    }
    static std::mutex cache_mutex;
    static bool cleaned[3] = {};
    std::lock_guard<std::mutex> lock(cache_mutex);
    if (!cleaned[index]) {
        if (!SdDataCleanWorkFiles(directory_path, "http-")) {
            err_out = LangString_BOOK_WRITE_FAIL;
            return {};
        }
        cleaned[index] = true;
    }
    auto cache = std::make_shared<HttpTempFile>();
    cache->path.reserve(SD_FILE_PATH_CAPACITY);
    char path[SD_FILE_PATH_CAPACITY];
    const int fd = SdCreateWorkFile(areas[index], "http-", path, sizeof(path));
    if (fd >= 0) {
        cache->path = path;
        if (close(fd) == 0) {
            return cache;
        }
    }
    err_out = LangString_BOOK_WRITE_FAIL;
    return {};
} catch (const std::bad_alloc&) {
    err_out = LangString_BOOK_OUT_OF_MEMORY;
    return {};
}

void HttpDownloadConfigureActivity(void (*begin)(), void (*end)()) {
    if ((begin == nullptr) != (end == nullptr)) {
        ESP_LOGE("HttpDownload", "operation=configure result=invalid_activity_callbacks");
        return;
    }
    s_activity_begin = begin;
    s_activity_end = end;
}

HttpTempFile::~HttpTempFile() {
    SdAccessTransaction transaction;
    if (transaction && !path.empty() && unlink(path.c_str()) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "HTTP cache cleanup failed");
    }
}

std::shared_ptr<HttpTempFile> DownloadHttpToCache(const char* url, std::string& err_out, DownloadGate* gate,
                                                  HttpDownloadCancelledFn cancelled, void* context, HttpCacheApp app) {
    SdAccessTransaction transaction;
    if (!transaction) {
        LogStorageError("HttpDisk", transaction.Error());
        err_out = LangString_BOOK_WRITE_FAIL;
        return {};
    }
    auto cache = HttpDownloadCreateHttpTempFile(err_out, app);
    if (!cache || !DownloadHttpToFile(url, cache->path.c_str(), nullptr, 0, nullptr, nullptr, err_out, gate, cancelled,
                                      context)) {
        return {};
    }
    return cache;
}

bool DownloadHttpToFile(const char* url, const char* tmp_path, const char* expected_sha256, uint64_t expected_total,
                        HttpDownloadProgressFn on_progress, void* progress_user, std::string& err_out,
                        DownloadGate* gate, HttpDownloadCancelledFn cancelled, void* context, StorageError* storage_error) {
    StorageError local_error;
    if (!storage_error) {
        storage_error = &local_error;
    }
    *storage_error = {};
    struct StorageReporter {
        StorageError* error;
        ~StorageReporter() { LogStorageError("HttpDisk", *error); }
    } reporter{storage_error};
    err_out.clear();
    if (url == nullptr || url[0] == '\0' || tmp_path == nullptr || tmp_path[0] == '\0') {
        err_out = LangString_BOOK_BAD_ARG;
        return false;
    }

    struct ActivityHold {
        void (*end)();  // 配对使用本次活动的结束回调

        ActivityHold() : end(s_activity_end) {
            if (s_activity_begin) {
                s_activity_begin();
            }
        }

        ~ActivityHold() {
            if (end) {
                end();
            }
        }
    } activity_hold;

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        err_out = LangString_BOOK_NO_NETWORK;
        return false;
    }

    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        err_out = LangString_BOOK_CONN_FAIL;
        return false;
    }

    DownloadGate::Binding binding(gate, http.get());
    Http* http_raw = http.get();
    if (!binding) {
        err_out = LangString_BOOK_CANCELLED;
        return false;
    }

    http_raw->SetTimeout(kHttpTimeoutMs);
    ApiApplyCommonHeaders(http_raw);

    if (!http_raw->Open("GET", url)) {
        if (gate != nullptr) {
            gate->CloseHttp();
        }
        err_out = gate != nullptr && gate->IsCancelled() ? LangString_BOOK_CANCELLED : LangString_BOOK_DOWNLOAD_FAIL;
        return false;
    }

    const int status = http_raw->GetStatusCode();

    if (status < 200 || status >= 300) {
        if (gate != nullptr) {
            gate->CloseHttp();
        } else {
            http_raw->Close();
        }
        err_out = LangString_BOOK_DOWNLOAD_FAIL;
        return false;
    }

    FILE* f = fopen(tmp_path, "wb");
    if (f == nullptr) {
        CaptureStorageError(storage_error, StorageStage::Open, errno);
        if (gate != nullptr) {
            gate->CloseHttp();
        } else {
            http_raw->Close();
        }
        err_out = LangString_BOOK_WRITE_FAIL;
        return false;
    }

    psa_hash_operation_t hash_op = PSA_HASH_OPERATION_INIT;
    const bool check_hash = expected_sha256 != nullptr && expected_sha256[0] != '\0';

    if (check_hash && psa_hash_setup(&hash_op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        if (gate != nullptr) {
            gate->CloseHttp();
        } else {
            http_raw->Close();
        }
        fclose(f);
        unlink(tmp_path);
        err_out = LangString_BOOK_OUT_OF_MEMORY;
        return false;
    }

    const size_t content_length = http_raw->GetBodyLength();
    uint64_t total_expected = expected_total;
    if (content_length > 0) {
        total_expected = content_length;
    }

    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> heap_buf(
        static_cast<uint8_t*>(heap_caps_malloc(kIoBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), heap_caps_free);
    size_t buf_cap = kIoBytes;
    if (heap_buf == nullptr) {
        heap_buf.reset(static_cast<uint8_t*>(heap_caps_malloc(kIoFallback, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        buf_cap = kIoFallback;
    }
    if (heap_buf == nullptr) {
        if (gate != nullptr) {
            gate->CloseHttp();
        } else {
            http->Close();
        }
        fclose(f);
        unlink(tmp_path);
        psa_hash_abort(&hash_op);
        err_out = LangString_BOOK_OUT_OF_MEMORY;
        return false;
    }
    uint8_t* buf = heap_buf.get();

    uint64_t total = 0;
    size_t buffered = 0;
    int last_percent = -1;
    bool ok = true;
    const auto write_buffer = [&]() {
        const size_t written = fwrite(buf, 1, buffered, f);
        if (written != buffered) {
            CaptureStorageError(storage_error, StorageStage::Write, errno);
            err_out = LangString_BOOK_WRITE_FAIL;
            return false;
        }

        if (check_hash && psa_hash_update(&hash_op, buf, buffered) != PSA_SUCCESS) {
            err_out = LangString_BOOK_CHECKSUM_FAIL;
            return false;
        }
        buffered = 0;
        return true;
    };

    while (ok) {
        if ((gate != nullptr && gate->IsCancelled()) || (cancelled && cancelled(context))) {
            ok = false;
            err_out = LangString_BOOK_CANCELLED;
            break;
        }
        const int n = http_raw->Read(reinterpret_cast<char*>(buf + buffered), buf_cap - buffered);
        if (n < 0) {
            ok = false;
            err_out = (gate != nullptr && gate->IsCancelled()) ? LangString_BOOK_CANCELLED : LangString_BOOK_READ_FAIL;
            break;
        }
        if (n == 0) {
            if (buffered > 0) {
                ok = write_buffer();
            }
            break;
        }
        const size_t got = static_cast<size_t>(n);
        total += got;
        buffered += got;
        if (buffered == buf_cap && !write_buffer()) {
            ok = false;
            break;
        }
        ReportProgress(on_progress, progress_user, total, total_expected, last_percent);
    }

    if (gate != nullptr) {
        gate->CloseHttp();
    } else {
        http_raw->Close();
    }

    if (ok && fflush(f) != 0) {
        CaptureStorageError(storage_error, StorageStage::Flush, errno);
        ok = false;
        err_out = LangString_BOOK_WRITE_FAIL;
    }
    if (fclose(f) != 0) {
        CaptureStorageError(storage_error, StorageStage::Close, errno);
        ok = false;
        if (err_out.empty()) {
            err_out = LangString_BOOK_WRITE_FAIL;
        }
    }

    unsigned char hash[32];
    char hex[65] = {};
    if (ok && check_hash) {
        size_t hash_len = 0;

        const auto hash_result = psa_hash_finish(&hash_op, hash, sizeof(hash), &hash_len);
        if (hash_result != PSA_SUCCESS || hash_len != sizeof(hash)) {
            ok = false;
            err_out = LangString_BOOK_CHECKSUM_FAIL;
        } else {
            for (int i = 0; i < 32; i++) {
                std::snprintf(hex + i * 2, sizeof(hex) - i * 2, "%02x", hash[i]);
            }
        }
    } else if (check_hash) {
        psa_hash_abort(&hash_op);
    }

    if (!ok) {
        unlink(tmp_path);
        return false;
    }

    if (expected_total > 0 && total != expected_total) {
        ESP_LOGW(TAG, "push fileSize mismatch actual=%llu expected=%llu content_length=%llu (metadata only)",
                 static_cast<unsigned long long>(total), static_cast<unsigned long long>(expected_total),
                 static_cast<unsigned long long>(content_length));
    }
    if (content_length > 0 && total != content_length) {
        unlink(tmp_path);
        err_out = LangString_BOOK_READ_FAIL;
        return false;
    }
    if (total == 0) {
        unlink(tmp_path);
        err_out = LangString_BOOK_EMPTY_FILE;
        return false;
    }

    const bool hash_matches = !check_hash || Sha256EqualIgnoreCase(hex, expected_sha256);
    if (!hash_matches) {
        ESP_LOGW(TAG, "sha256 mismatch expect=%.16s... got=%.16s... (metadata only)", expected_sha256, hex);
    }

    ESP_LOGI(TAG, "downloaded %llu bytes integrity=%s (psram io %u)", static_cast<unsigned long long>(total),
             check_hash && hash_matches ? "sha256" : "length", static_cast<unsigned>(buf_cap));
    if (on_progress != nullptr) {
        on_progress(100, progress_user);
    }
    return true;
}
