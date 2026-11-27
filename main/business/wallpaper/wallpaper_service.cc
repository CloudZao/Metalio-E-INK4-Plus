#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <new>
#include <utility>
#include <esp_log.h>
#include <esp_timer.h>
#include <sys/stat.h>
#include "wallpaper_service.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "boards/common/storage/sd_access.h"
#include "business/cloud/wallpaper_cloud_library.h"
#include "business/reader/book_reader_prefs.h"
#include "business/reader/list_thumbnail_cache.h"
#include "infra/image/image_decode.h"
#include "business/wallpaper/image_file_write.h"
#include "business/wallpaper/wallpaper_active.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "sysmgmt/tasks/task_executor.h"

static constexpr const char* TAG = "Wallpaper";
static std::atomic<WallpaperOperation> s_operation{WallpaperOperation::None};
static std::atomic<uint32_t> s_revision{0};

static bool WallpaperSaveCancelled(void* context) {
    return static_cast<WallpaperResult*>(context)->cancelled.load(std::memory_order_acquire);
}

static bool IsWallpaperPath(const std::string& path) {
    constexpr char prefix[] = SD_PATH_WALLPAPER "/";
    return path.find('\0') == std::string::npos && path.compare(0, sizeof(prefix) - 1, prefix) == 0 &&
           SdIsValidFileName(path.c_str() + sizeof(prefix) - 1) && path.size() < SD_FILE_PATH_CAPACITY;
}

static bool BuildSaveAsPath(const std::string& source, char* destination, size_t capacity) {
    std::string stem = source.substr(source.find_last_of('/') + 1);
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos && dot != 0) {
        stem.resize(dot);
    }
    // 另存为只更新已有时间戳，不连续叠加后缀。
    const size_t dash = stem.find_last_of('-');
    if (dash != std::string::npos && dash != 0) {
        const std::string suffix = stem.substr(dash + 1);
        size_t digits = 0;
        while (digits < suffix.size() && suffix[digits] >= '0' && suffix[digits] <= '9') {
            ++digits;
        }
        bool stamp = digits == 14;
        if (stamp && digits < suffix.size()) {
            stamp = suffix[digits] == '_' && digits + 1 < suffix.size();
            for (size_t i = digits + 1; stamp && i < suffix.size(); ++i) {
                stamp = suffix[i] >= '0' && suffix[i] <= '9';
            }
        }
        if (stamp) {
            stem.resize(dash);
        }
    }
    constexpr size_t reserve = 1 + 14 + 4 + sizeof(".a2l8");
    const size_t max_stem = SD_FILE_NAME_CAPACITY - reserve;
    if (stem.size() > max_stem) {
        stem.resize(SdUtf8PrefixLength(stem.c_str(), max_stem));
    }
    char stamp[16] = {};
    const time_t now = time(nullptr);
    struct tm value{};
    if (localtime_r(&now, &value) != nullptr) {
        std::snprintf(stamp, sizeof(stamp), "%04u%02u%02u%02u%02u%02u",
                      std::min(unsigned(value.tm_year + 1900), 9999u), std::min(unsigned(value.tm_mon + 1), 12u),
                      std::min(unsigned(value.tm_mday), 31u), std::min(unsigned(value.tm_hour), 23u),
                      std::min(unsigned(value.tm_min), 59u), std::min(unsigned(value.tm_sec), 60u));
    } else {
        std::snprintf(stamp, sizeof(stamp), "%014llu",
                      static_cast<unsigned long long>(esp_timer_get_time() / 1000000) % 100000000000000ULL);
    }
    for (int n = 0; n < 64; ++n) {
        char name[SD_FILE_NAME_CAPACITY];
        if (n == 0) {
            std::snprintf(name, sizeof(name), "%s-%s.a2l8", stem.c_str(), stamp);
        } else {
            std::snprintf(name, sizeof(name), "%s-%s_%d.a2l8", stem.c_str(), stamp, n);
        }
        const int length = std::snprintf(destination, capacity, "%s/%s", SD_PATH_WALLPAPER, name);
        if (length < 0 || static_cast<size_t>(length) >= capacity) {
            return false;
        }
        struct stat info{};
        if (stat(destination, &info) != 0) {
            return errno == ENOENT;
        }
    }
    return false;
}

static void SaveWallpaper(WallpaperResult& result, const WallpaperSaveOptions& options) {
    if (WallpaperSaveCancelled(&result)) {
        return;
    }
    SdAccessTransaction transaction;
    if (!transaction) {
        result.failed = 1;
        std::snprintf(result.error, sizeof(result.error), "%s", LangString_STORAGE_IO_ERROR);
        ESP_LOGW(TAG, "operation=save stage=sd_admission result=unavailable");
        return;
    }
    std::snprintf(result.path, sizeof(result.path), "%s", options.path.c_str());
    if (options.save_as && !BuildSaveAsPath(options.path, result.path, sizeof(result.path))) {
        result.failed = 1;
        std::snprintf(result.error, sizeof(result.error), "%s", LangString_WALLPAPER_SAVE_FAIL);
        ESP_LOGE(TAG, "operation=save stage=destination errno=%d", errno);
        return;
    }
    const int long_side = std::max(options.width, options.height);
    const int short_side = std::min(options.width, options.height);
    int width = 0, height = 0;
    int decode_w = options.width, decode_h = options.height;
    if (PeekRasterFileSize(options.path.c_str(), &width, &height)) {
        decode_w = width > height ? long_side : short_side;
        decode_h = width > height ? short_side : long_side;
    }
    ImageDecodeError error = ImageDecodeError::kNone;
    RasterImage image;
    const char* stage = "decode";
    bool ok = DecodeImageFileToL8(options.path.c_str(), decode_w, decode_h, image,
                                 WallpaperSaveCancelled, &result, &error) && !image.empty();
    RasterImage oriented;
    std::vector<uint8_t> encoded;
    if (ok) {
        TrimRasterLetterbox(image);
        OrientRasterImage(image, options.rotation, options.mirror, oriented);
        stage = "scale";
        ok = ScaleRasterToFit(oriented, oriented.width > oriented.height ? long_side : short_side,
                              oriented.width > oriented.height ? short_side : long_side, oriented);
    }
    if (ok) {
        stage = "encode";
        ok = EncodeL8ToA2l8(oriented, encoded) && ValidateA2l8Bytes(encoded.data(), encoded.size());
    }
    if (ok) {
        stage = "write";
        ok = WallpaperWriteWallpaperFileAtomic(result.path, encoded.data(), encoded.size(), !options.save_as,
                                               WallpaperSaveCancelled, &result, &error);
    }
    if (!ok) {
        if (error != ImageDecodeError::kCancelled && !WallpaperSaveCancelled(&result)) {
            result.failed = 1;
            std::snprintf(result.error, sizeof(result.error), "%s (%u)", LangString_WALLPAPER_SAVE_FAIL,
                          unsigned(error));
            ESP_LOGE(TAG, "operation=save stage=%s error=%u", stage, unsigned(error));
        }
        return;
    }
    // 文件已经提交，迟到的取消不改写成功结果。
    result.succeeded = 1;
    result.size_bytes = encoded.size();
    if (options.save_as) {
        if (!WriteWallpaperMeta(result.path, "", std::strrchr(result.path, '/') + 1)) {
            ESP_LOGW(TAG, "operation=save stage=metadata result=failed action=keep_image");
        }
    }
}

static WallpaperRequest SubmitWallpaper(const std::shared_ptr<WallpaperResult>& result,
                                         std::function<void()> run, TaskDomain domain) {
    WallpaperOperation expected = WallpaperOperation::None;
    if (!s_operation.compare_exchange_strong(expected, result->operation, std::memory_order_acq_rel)) {
        return {};
    }
    bool accepted = false;
    try {
        accepted = TaskExecutor::Schedule([result, run = std::move(run)] {
            {
                PowerNeedHold awake(PowerNeed::StandbyInhibit);
                try {
                    run();
                } catch (const std::bad_alloc&) {
                    if (result->succeeded == 0 || result->operation == WallpaperOperation::Delete) {
                        ++result->failed;
                        std::snprintf(result->error, sizeof(result->error), "%s", LangString_COMMON_MEMORY_RETRY);
                    }
                    if (result->succeeded > 0 && result->operation == WallpaperOperation::Save) {
                        ESP_LOGW(TAG, "operation=save stage=metadata result=no_memory action=keep_image");
                    } else {
                        ESP_LOGE(TAG, "operation=run kind=%u stage=allocate result=no_memory", unsigned(result->operation));
                    }
                }
                if (result->succeeded > 0 || (result->operation == WallpaperOperation::Delete && result->failed > 0)) {
                    ClearListThumbnailCache();
                    s_revision.fetch_add(1, std::memory_order_release);
                }
            }
            result->completed.store(true, std::memory_order_release);
            s_operation.store(WallpaperOperation::None, std::memory_order_release);
        }, domain);
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "operation=submit stage=allocate result=no_memory");
    }
    if (!accepted) {
        s_operation.store(WallpaperOperation::None, std::memory_order_release);
        return {};
    }
    return result;
}

WallpaperRequest WallpaperSelect(WallpaperTarget target, std::string path, bool clear, bool overlay) try {
    if (!IsWallpaperPath(path) || target > WallpaperTarget::Reading) {
        return {};
    }
    auto result = std::make_shared<WallpaperResult>();
    result->operation = WallpaperOperation::Select;
    result->target = target;
    result->clear = clear;
    return SubmitWallpaper(result, [result, path = std::move(path), overlay] {
        std::string error;
        bool ok;
        if (result->clear) {
            ok = result->target == WallpaperTarget::Reading ? Wallpaper_ClearReadingWallpaper() :
                 result->target == WallpaperTarget::Standby ? Wallpaper_ClearStandbyWallpaper() :
                                                               Wallpaper_ClearShutdownWallpaper();
        } else {
            ok = result->target == WallpaperTarget::Reading ? Wallpaper_SetReadingFromFile(path.c_str(), error) :
                 result->target == WallpaperTarget::Standby ? Wallpaper_SetStandbyFromFile(path.c_str(), error, overlay) :
                                                               Wallpaper_SetActiveFromFile(path.c_str(), error, overlay);
        }
        if (ok) {
            if (result->target == WallpaperTarget::Reading) {
                BookReaderPrefsSetBackground(result->clear ? 0 : 3);
            }
            result->succeeded = 1;
        } else {
            result->failed = 1;
            std::snprintf(result->error, sizeof(result->error), "%s",
                          error.empty() ? LangString_STORAGE_IO_ERROR : error.c_str());
            ESP_LOGE(TAG, "operation=select target=%u result=failed", unsigned(result->target));
        }
    }, TaskDomain::Storage);
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=select stage=allocate result=no_memory");
    return {};
}

WallpaperRequest WallpaperDelete(std::vector<std::string> names, bool batch) try {
    if (names.empty() || (!batch && names.size() != 1)) {
        return {};
    }
    for (const auto& name : names) {
        if (name.find('\0') != std::string::npos || name.size() >= SD_FILE_NAME_CAPACITY ||
            !SdIsValidFileName(name.c_str())) {
            return {};
        }
    }
    auto result = std::make_shared<WallpaperResult>();
    result->operation = WallpaperOperation::Delete;
    result->batch = batch;
    return SubmitWallpaper(result, [result, names = std::move(names)] {
        for (const auto& name : names) {
            if (result->batch && result->cancelled.load(std::memory_order_acquire)) {
                break;
            }
            const std::string path = std::string(SD_PATH_WALLPAPER) + "/" + name;
            if (WallpaperDeleteWallpaperFile(path.c_str())) {
                ++result->succeeded;
            } else {
                ++result->failed;
                std::snprintf(result->error, sizeof(result->error), "%s", LangString_WALLPAPER_DELETE_FAIL);
                ESP_LOGE(TAG, "operation=delete stage=storage result=failed");
            }
        }
    }, TaskDomain::Storage);
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=delete stage=allocate result=no_memory");
    return {};
}

WallpaperRequest WallpaperSave(WallpaperSaveOptions options) try {
    if (!IsWallpaperPath(options.path) || options.rotation > 3 || options.width <= 0 || options.height <= 0) {
        return {};
    }
    auto result = std::make_shared<WallpaperResult>();
    result->operation = WallpaperOperation::Save;
    result->save_as = options.save_as;
    return SubmitWallpaper(result, [result, options = std::move(options)] { SaveWallpaper(*result, options); }, TaskDomain::Image);
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=save stage=allocate result=no_memory");
    return {};
}

bool WallpaperIsBusy(WallpaperOperation operation) {
    const auto current = s_operation.load(std::memory_order_acquire);
    return current != WallpaperOperation::None && (operation == WallpaperOperation::None || operation == current);
}

uint32_t WallpaperRevision() {
    return s_revision.load(std::memory_order_acquire);
}
