#include "http_download_file.h"

// xtensa-g++：本文件在 -Os 下曾 ICE
#pragma GCC optimize("O1")

#include "api_endpoints.h"
#include "api_http.h"
#include "assets/lang_config.h"
#include "board.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <psa/crypto.h>
#include <unistd.h>

namespace reader {
namespace {

constexpr const char* TAG = "HttpDlFile";
constexpr int kHttpTimeoutMs = 30000;
// 大块读写少 syscall；数据在 PSRAM（栈仍须内 DRAM，写 SD 关 cache）
constexpr size_t kIoBytes = 32 * 1024;
constexpr size_t kIoFallback = 2048;

bool Sha256EqualIgnoreCase(const char* a, const char* b) {
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

void ReportProgress(HttpDownloadProgressFn on_progress, void* progress_user, uint64_t total,
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

}  // namespace

bool DownloadHttpToFile(const char* url, const char* tmp_path, const char* expected_sha256,
                        uint64_t expected_total, size_t max_bytes,
                        HttpDownloadProgressFn on_progress, void* progress_user,
                        std::string& err_out, DownloadGate* gate) {
    err_out.clear();
    if (url == nullptr || url[0] == '\0' || tmp_path == nullptr || tmp_path[0] == '\0') {
        err_out = Lang::Strings::BOOK_BAD_ARG;
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        err_out = Lang::Strings::BOOK_NO_NETWORK;
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        err_out = Lang::Strings::BOOK_CONN_FAIL;
        return false;
    }
    Http* http_raw = http.get();
    if (gate != nullptr) {
        if (gate->IsCancelled()) {
            err_out = Lang::Strings::BOOK_CANCELLED;
            return false;
        }
        if (!gate->BindHttp(std::move(http))) {
            err_out = Lang::Strings::BOOK_CANCELLED;
            return false;
        }
    }

    http_raw->SetTimeout(kHttpTimeoutMs);
    api::ApplyCommonHeaders(http_raw);
    api::LogHttpRequest(TAG, "GET", url);
    if (!http_raw->Open("GET", url)) {
        if (gate != nullptr) {
            gate->CloseHttp();
        }
        err_out = gate != nullptr && gate->IsCancelled() ? Lang::Strings::BOOK_CANCELLED
                                                         : Lang::Strings::BOOK_DOWNLOAD_FAIL;
        api::LogHttpResponse(TAG, -1, err_out);
        return false;
    }

    const int status = http_raw->GetStatusCode();
    if (status < 200 || status >= 300) {
        if (gate != nullptr) {
            gate->CloseHttp();
        } else {
            http_raw->Close();
        }
        err_out = Lang::Strings::BOOK_DOWNLOAD_FAIL;
        api::LogHttpResponse(TAG, status, err_out);
        return false;
    }

    FILE* f = fopen(tmp_path, "wb");
    if (f == nullptr) {
        if (gate != nullptr) {
            gate->CloseHttp();
        } else {
            http_raw->Close();
        }
        err_out = Lang::Strings::BOOK_WRITE_FAIL;
        return false;
    }

    psa_hash_operation_t hash_op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&hash_op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        if (gate != nullptr) {
            gate->CloseHttp();
        } else {
            http_raw->Close();
        }
        fclose(f);
        unlink(tmp_path);
        err_out = Lang::Strings::BOOK_OUT_OF_MEMORY;
        return false;
    }

    const size_t content_length = http_raw->GetBodyLength();
    uint64_t total_expected = expected_total;
    if (content_length > 0) {
        total_expected = content_length;
    }

    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> heap_buf(
        static_cast<uint8_t*>(
            heap_caps_malloc(kIoBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
        heap_caps_free);
    size_t buf_cap = kIoBytes;
    if (heap_buf == nullptr) {
        heap_buf.reset(static_cast<uint8_t*>(
            heap_caps_malloc(kIoFallback, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
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
        err_out = Lang::Strings::BOOK_OUT_OF_MEMORY;
        return false;
    }
    uint8_t* buf = heap_buf.get();

    size_t total = 0;
    int last_percent = -1;
    bool ok = true;

    while (ok) {
        if (gate != nullptr && gate->IsCancelled()) {
            ok = false;
            err_out = Lang::Strings::BOOK_CANCELLED;
            break;
        }
        const int n = http_raw->Read(reinterpret_cast<char*>(buf), buf_cap);
        if (n < 0) {
            ok = false;
            err_out = (gate != nullptr && gate->IsCancelled()) ? Lang::Strings::BOOK_CANCELLED
                                                              : Lang::Strings::BOOK_READ_FAIL;
            break;
        }
        if (n == 0) {
            break;
        }
        const size_t got = static_cast<size_t>(n);
        if (total + got > max_bytes) {
            ok = false;
            err_out = Lang::Strings::BOOK_FILE_TOO_LARGE;
            break;
        }
        if (fwrite(buf, 1, got, f) != got) {
            ok = false;
            err_out = Lang::Strings::BOOK_WRITE_FAIL;
            break;
        }
        if (psa_hash_update(&hash_op, buf, got) != PSA_SUCCESS) {
            ok = false;
            err_out = Lang::Strings::BOOK_CHECKSUM_FAIL;
            break;
        }
        total += got;
        ReportProgress(on_progress, progress_user, total, total_expected, last_percent);
    }

    if (gate != nullptr) {
        gate->CloseHttp();
    } else {
        http_raw->Close();
    }

    if (ok && fflush(f) != 0) {
        ok = false;
        err_out = Lang::Strings::BOOK_WRITE_FAIL;
    }
    if (fclose(f) != 0) {
        ok = false;
        if (err_out.empty()) {
            err_out = Lang::Strings::BOOK_WRITE_FAIL;
        }
    }

    unsigned char hash[32];
    char hex[65] = {};
    if (ok) {
        size_t hash_len = 0;
        if (psa_hash_finish(&hash_op, hash, sizeof(hash), &hash_len) != PSA_SUCCESS ||
            hash_len != sizeof(hash)) {
            ok = false;
            err_out = Lang::Strings::BOOK_CHECKSUM_FAIL;
        } else {
            for (int i = 0; i < 32; i++) {
                std::snprintf(hex + i * 2, sizeof(hex) - i * 2, "%02x", hash[i]);
            }
        }
    } else {
        psa_hash_abort(&hash_op);
    }

    if (!ok) {
        unlink(tmp_path);
        return false;
    }
    if (total == 0) {
        unlink(tmp_path);
        err_out = Lang::Strings::BOOK_EMPTY_FILE;
        return false;
    }

    if (expected_sha256 != nullptr && expected_sha256[0] != '\0' &&
        !Sha256EqualIgnoreCase(hex, expected_sha256)) {
        unlink(tmp_path);
        ESP_LOGW(TAG, "sha256 mismatch expect=%.16s... got=%.16s...", expected_sha256, hex);
        err_out = Lang::Strings::BOOK_CHECKSUM_FAIL;
        return false;
    }

    ESP_LOGI(TAG, "downloaded %u bytes sha256=%.16s... (psram io %u)", static_cast<unsigned>(total),
             hex, static_cast<unsigned>(buf_cap));
    if (on_progress != nullptr) {
        on_progress(100, progress_user);
    }
    return true;
}

}  // namespace reader
