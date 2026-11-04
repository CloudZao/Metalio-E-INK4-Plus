#include "wallpaper_screen/wallpaper_screen_priv.h"
#include "wallpaper_screen/preview/wallpaper_adjust.h"
#include "wallpaper_screen/wallpaper_list.h"
#include "wallpaper_screen/wallpaper_multi.h"
#include "wallpaper_screen/preview/wallpaper_preview.h"
#include "wallpaper_screen/wallpaper_ui_helpers.h"

#include <freertos/FreeRTOS.h>
#include <lvgl.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <esp_timer.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>

#include "assets/lang_config.h"
#include "reader/reader.h"
#include "reader/image_file_write.h"
#include "cloud_screen/push/wallpaper_cloud_library.h"
#include "standby_screen/standby_wallpaper.h"

// 去掉 stem 末尾已有的 -YYYYMMDDHHmmss 或 -YYYYMMDDHHmmss_n，便于另存为只更新时间
void StripTrailingSaveAsStamp(char* stem) {
    if (stem == nullptr || stem[0] == '\0') {
        return;
    }
    char* dash = std::strrchr(stem, '-');
    if (dash == nullptr || dash == stem) {
        return;
    }
    const char* p = dash + 1;
    size_t digits = 0;
    while (p[digits] >= '0' && p[digits] <= '9') {
        ++digits;
    }
    if (digits != 14) {
        return;
    }
    if (p[digits] == '\0') {
        *dash = '\0';
        return;
    }
    if (p[digits] != '_') {
        return;
    }
    size_t i = digits + 1;
    if (p[i] < '0' || p[i] > '9') {
        return;
    }
    while (p[i] >= '0' && p[i] <= '9') {
        ++i;
    }
    if (p[i] == '\0') {
        *dash = '\0';
    }
}

// 另存为：原名后追加/更新 -年月日时分秒；已存在则再试 _n
bool BuildUniqueSaveAsPath(const FileEntry& src, char* out_path, size_t out_path_len,
                           char* out_name, size_t out_name_len) {
    if (out_path == nullptr || out_name == nullptr || out_path_len == 0 || out_name_len == 0) {
        return false;
    }
    const char* name = src.name;
    const char* dot = std::strrchr(name, '.');
    char stem[sizeof(FileEntry::name)] = {};
    char ext[16] = {};
    if (dot != nullptr && dot != name) {
        const size_t ext_len = std::strlen(dot);
        if (ext_len == 0 || ext_len >= sizeof(ext)) {
            return false;
        }
        size_t stem_len = static_cast<size_t>(dot - name);
        if (stem_len >= sizeof(stem)) {
            stem_len = sizeof(stem) - 1;
        }
        std::memcpy(stem, name, stem_len);
        stem[stem_len] = '\0';
        std::memcpy(ext, dot, ext_len + 1);
    } else {
        const size_t nlen = std::strlen(name);
        const size_t copy = nlen < sizeof(stem) - 1 ? nlen : sizeof(stem) - 1;
        std::memcpy(stem, name, copy);
        stem[copy] = '\0';
    }
    std::snprintf(ext,sizeof(ext),".a2l8");
    StripTrailingSaveAsStamp(stem);

    // out_name: stem + '-' + stamp(14) + 可选 '_NN' + ext
    const size_t reserve = 1 + 14 + 4 + std::strlen(ext) + 1;
    if (out_name_len <= reserve) {
        return false;
    }
    const size_t max_stem = out_name_len - reserve;
    if (std::strlen(stem) > max_stem) {
        stem[SdUtf8PrefixLength(stem, max_stem)] = '\0';
    }

    char stamp[16] = {};
    time_t now = time(nullptr);
    struct tm tm_info = {};
    if (localtime_r(&now, &tm_info) != nullptr) {
        // 夹紧字段，避免 -Wformat-truncation 对 tm 取值范围的误报
        unsigned y = static_cast<unsigned>(tm_info.tm_year + 1900);
        unsigned mo = static_cast<unsigned>(tm_info.tm_mon + 1);
        unsigned d = static_cast<unsigned>(tm_info.tm_mday);
        unsigned h = static_cast<unsigned>(tm_info.tm_hour);
        unsigned mi = static_cast<unsigned>(tm_info.tm_min);
        unsigned s = static_cast<unsigned>(tm_info.tm_sec);
        if (y > 9999) {
            y = 9999;
        }
        if (mo > 12) {
            mo = 12;
        }
        if (d > 31) {
            d = 31;
        }
        if (h > 23) {
            h = 23;
        }
        if (mi > 59) {
            mi = 59;
        }
        if (s > 60) {
            s = 60;
        }
        std::snprintf(stamp, sizeof(stamp), "%04u%02u%02u%02u%02u%02u", y, mo, d, h, mi, s);
    } else {
        const unsigned long long sec =
            static_cast<unsigned long long>(esp_timer_get_time() / 1000000) % 100000000000000ULL;
        std::snprintf(stamp, sizeof(stamp), "%014llu", sec);
    }

    for (int n = 0; n < 64; ++n) {
        int name_n = 0;
        if (n == 0) {
            name_n = std::snprintf(out_name, out_name_len, "%s-%s%s", stem, stamp, ext);
        } else {
            name_n = std::snprintf(out_name, out_name_len, "%s-%s_%d%s", stem, stamp, n, ext);
        }
        if (name_n < 0 || static_cast<size_t>(name_n) >= out_name_len) {
            return false;
        }
        const int path_n =
            std::snprintf(out_path, out_path_len, "%s/%s", kPosixDir, out_name);
        if (path_n < 0 || static_cast<size_t>(path_n) >= out_path_len) {
            return false;
        }
        struct stat st {};
        if (stat(out_path, &st) != 0) {
            return true;
        }
    }
    return false;
}

struct SaveWork {
    uint32_t epoch = 0;
    uint32_t generation = 0;
    int index = -1;
    uint8_t rot_cw = 0;
    bool mirror_h = false;
    bool save_as = false;
    int decode_w = DISPLAY_WIDTH; // 原图方向对应的屏幕宽度
    int decode_h = DISPLAY_HEIGHT; // 原图方向对应的屏幕高度
    char src_path[SD_FILE_PATH_CAPACITY] = {};
    char out_path[SD_FILE_PATH_CAPACITY] = {};
    char out_name[SD_FILE_NAME_CAPACITY] = {};
};

struct SaveResultMsg {
    uint32_t epoch = 0;
    uint32_t generation = 0;
    int index = -1;
    bool ok = false;
    bool save_as = false;
    size_t size_bytes = 0;
    char out_name[SD_FILE_NAME_CAPACITY] = {};
    char err[80] = {};
};

bool SaveTransformedWallpaper(const SaveWork& work, size_t& size_out, std::string& err_out) try {
    size_out = 0;
    err_out.clear();
    if (work.src_path[0] == '\0' || work.out_path[0] == '\0') {
        err_out = Lang::Strings::WALLPAPER_FILE_INVALID;
        return false;
    }
    if (work.epoch != Wallpaper_State().epoch || Wallpaper_State().decode_paused.load() ||
        work.generation != Wallpaper_State().decode_generation.load()) {
        err_out = Lang::Strings::WALLPAPER_SAVE_FAIL;
        return false;
    }
    WallpaperDecodeToken token{work.epoch, work.generation};
    reader::ImageDecodeError error = reader::ImageDecodeError::kNone;
    const int long_side = std::max(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    const int short_side = std::min(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    int width = 0, height = 0;
    int decode_w = work.decode_w, decode_h = work.decode_h;
    if (reader::PeekRasterFileSize(work.src_path, &width, &height)) {
        decode_w = width > height ? long_side : short_side;
        decode_h = width > height ? short_side : long_side;
    }
    reader::RasterImage image;
    if (!reader::DecodeImageFileToL8(work.src_path, decode_w, decode_h, image,
                                    Wallpaper_DecodeCancelled, &token, &error) || image.empty()) {
        char message[80];
        Wallpaper_FormatDecodeFailure(message, sizeof(message), error);
        err_out = message;
        return false;
    }
    TrimBakedLetterbox(image);
    reader::RasterImage oriented;
    ApplyWallpaperOrient(image, work.rot_cw, work.mirror_h, oriented);
    if (!reader::ScaleRasterToFit(oriented, oriented.width > oriented.height ? long_side : short_side,
                                  oriented.width > oriented.height ? short_side : long_side, oriented)) {
        err_out = Lang::Strings::WALLPAPER_SAVE_FAIL;
        return false;
    }
    std::vector<uint8_t> encoded;
    if (!reader::EncodeL8ToA2l8(oriented, encoded) || !reader::ValidateA2l8Bytes(encoded.data(), encoded.size())) {
        err_out = Lang::Strings::WALLPAPER_SAVE_FAIL;
        return false;
    }
    if (!reader::WriteWallpaperFileAtomic(work.out_path, encoded.data(), encoded.size(), !work.save_as,
                                         Wallpaper_DecodeCancelled, &token, &error)) {
        char message[80];
        std::snprintf(message, sizeof(message), "%s (%u)", Lang::Strings::WALLPAPER_SAVE_FAIL, unsigned(error));
        err_out = message;
        return false;
    }
    if (work.save_as) {
        std::string image_name;
        std::string original_filename;
        reader::ReadWallpaperMeta(work.src_path, image_name, original_filename);
        // 列表/预览标题读 meta：保留 imageName，originalFilename 跟新落盘名（含更新后的时间戳）
        reader::WriteWallpaperMeta(work.out_path, image_name, work.out_name);
    }
    size_out = encoded.size();
    return true;
} catch (const std::bad_alloc&) {
    char message[80];
    std::snprintf(message, sizeof(message), "%s (%u)", Lang::Strings::WALLPAPER_SAVE_FAIL,
                  unsigned(reader::ImageDecodeError::kNoMemory));
    err_out = message;
    return false;
}

void ApplySaveAsync(void* p) {
    auto* msg = static_cast<SaveResultMsg*>(p);
    Wallpaper_State().workers.save_busy.store(false);
    Wallpaper_State().workers.save_task = nullptr;
    if (msg == nullptr) {
        return;
    }
    if (!Wallpaper_State().screen_alive || msg->epoch != Wallpaper_State().epoch ||
        Wallpaper_State().decode_paused.load() || msg->generation != Wallpaper_State().decode_generation.load()) {
        delete msg;
        if (Wallpaper_State().screen_alive.load() && !Wallpaper_State().decode_paused.load() &&
            Wallpaper_State().mode == UiMode::kPreview) {
            UpdateEnableButtonUi();
            UpdateAdjustUi();
        }
        return;
    }
    if (!msg->ok) {
        ScreenShowToast(msg->err[0] != '\0' ? msg->err : Lang::Strings::WALLPAPER_SAVE_FAIL);
        UpdateEnableButtonUi();
        UpdateAdjustUi();
        delete msg;
        return;
    }

    Wallpaper_State().preview.adjust_open = false;
    ResetOrientState();
    Wallpaper_CloseDialog();

    if (msg->save_as) {
        CollectWallpapers();
        SyncWpSelectedSize();
        int found = -1;
        if (msg->out_name[0] != '\0') {
            for (size_t i = 0; i < Wallpaper_State().list.files.size(); ++i) {
                if (std::strcmp(Wallpaper_State().list.files[i].name, msg->out_name) == 0) {
                    found = static_cast<int>(i);
                    break;
                }
            }
        }
        if (found >= 0) {
            Wallpaper_State().preview.idx = found;
            if (Wallpaper_State().ui.preview_title != nullptr) {
                const FileEntry& fe = Wallpaper_State().list.files[static_cast<size_t>(found)];
                const char* raw = fe.title[0] != '\0' ? fe.title : fe.name;
                const std::string shown = Wallpaper_LayoutTitleTwoLines(raw, Wallpaper_UiFont(), Wallpaper_ContentWidth());
                lv_label_set_text(Wallpaper_State().ui.preview_title, shown.c_str());
            }
        }
    } else if (msg->index >= 0 && msg->index < static_cast<int>(Wallpaper_State().list.files.size())) {
        Wallpaper_State().list.files[static_cast<size_t>(msg->index)].size_bytes = msg->size_bytes;
        if (msg->index < static_cast<int>(Wallpaper_State().list.thumbs.size())) {
            Wallpaper_State().list.thumbs[static_cast<size_t>(msg->index)].reset();
            PaintCellThumb(msg->index);
        }
        StandbyWallpaper_InvalidateDecoded();
    }

    if (Wallpaper_State().preview.idx >= 0) {
        ScheduleLoadPreview(Wallpaper_State().preview.idx);
    }

    UpdateEnableButtonUi();
    UpdateDeleteButtonUi();
    UpdateAdjustUi();
    if (Wallpaper_State().mode == UiMode::kList) {
        RequestWallpaperListRebuild();
    } else {
        ScheduleThumbFill();
    }
    delete msg;
}

void SaveTask(void* arg) {
    auto* work = static_cast<SaveWork*>(arg);
    auto* msg = new SaveResultMsg{};
    if (work == nullptr) {
        std::snprintf(msg->err, sizeof(msg->err), "%s", Lang::Strings::WALLPAPER_FILE_INVALID);
    } else {
        msg->epoch = work->epoch;
        msg->generation = work->generation;
        msg->index = work->index;
        msg->save_as = work->save_as;
        std::snprintf(msg->out_name, sizeof(msg->out_name), "%s", work->out_name);
        // 离开页/取消会 ++epoch；写盘前再检查，避免覆盖后结果被丢弃
        if (work->epoch != Wallpaper_State().epoch) {
            std::snprintf(msg->err, sizeof(msg->err), "%s", Lang::Strings::WALLPAPER_SAVE_FAIL);
        } else {
            std::string err;
            msg->ok = SaveTransformedWallpaper(*work, msg->size_bytes, err);
            if (!msg->ok) {
                std::snprintf(msg->err, sizeof(msg->err), "%s",
                              err.empty() ? Lang::Strings::WALLPAPER_SAVE_FAIL : err.c_str());
            }
            if (work->epoch != Wallpaper_State().epoch) {
                msg->ok = false;
                std::snprintf(msg->err, sizeof(msg->err), "%s", Lang::Strings::WALLPAPER_SAVE_FAIL);
            }
        }
        delete work;
    }
    if (!ScreenLvAsync(ApplySaveAsync, msg)) {
        delete msg;
        Wallpaper_State().workers.save_busy.store(false);
        Wallpaper_State().workers.save_task = nullptr;
    }
    vTaskDeleteWithCaps(nullptr);
}

void ScheduleSaveTransform(bool save_as) {
    if (!Wallpaper_State().preview.adjust_open) {
        return;
    }
    if (Wallpaper_State().preview.idx < 0 || Wallpaper_State().preview.idx >= static_cast<int>(Wallpaper_State().list.files.size())) {
        return;
    }
    if (Wallpaper_State().workers.save_busy.exchange(true)) {
        return;
    }
    Wallpaper_CloseDialog();
    UpdateEnableButtonUi();
    UpdateAdjustUi();

    const FileEntry& fe = Wallpaper_State().list.files[static_cast<size_t>(Wallpaper_State().preview.idx)];
    auto* work = new SaveWork{};
    work->epoch = Wallpaper_State().epoch;
    work->generation = Wallpaper_State().decode_generation.load();
    work->index = Wallpaper_State().preview.idx;
    work->rot_cw = Wallpaper_State().preview.rot_cw;
    work->mirror_h = Wallpaper_State().preview.mirror_h;
    work->save_as = save_as;
    const auto* base = Wallpaper_State().preview.base;
    if (base && base->width > base->height) {
        work->decode_w = std::max(DISPLAY_WIDTH, DISPLAY_HEIGHT);
        work->decode_h = std::min(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    } else {
        work->decode_w = std::min(DISPLAY_WIDTH, DISPLAY_HEIGHT);
        work->decode_h = std::max(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    }
    std::snprintf(work->src_path, sizeof(work->src_path), "%s", fe.path);
    if (save_as) {
        if (!BuildUniqueSaveAsPath(fe, work->out_path, sizeof(work->out_path), work->out_name,
                                   sizeof(work->out_name))) {
            delete work;
            Wallpaper_State().workers.save_busy.store(false);
            Wallpaper_State().workers.save_task = nullptr;
            ScreenShowToast(Lang::Strings::WALLPAPER_SAVE_FAIL);
            UpdateEnableButtonUi();
            UpdateAdjustUi();
            return;
        }
    } else {
        std::snprintf(work->out_path, sizeof(work->out_path), "%s", fe.path);
        std::snprintf(work->out_name, sizeof(work->out_name), "%s", fe.name);
    }
    if (xTaskCreatePinnedToCoreWithCaps(SaveTask, "wp_save", kSaveStack, work, 5, &Wallpaper_State().workers.save_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        delete work;
        Wallpaper_State().workers.save_busy.store(false);
        Wallpaper_State().workers.save_task = nullptr;
        ScreenShowToast(Lang::Strings::WALLPAPER_SAVE_FAIL);
        UpdateEnableButtonUi();
        UpdateAdjustUi();
    }
}

