#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>
#include <esp_log.h>
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "business/wallpaper/wallpaper_service.h"
#include "sysmgmt/power/standby_wallpaper.h"
#include "ui/apps/wallpaper/preview/wallpaper_adjust.h"
#include "ui/apps/wallpaper/preview/wallpaper_preview.h"
#include "ui/apps/wallpaper/wallpaper_list.h"
#include "ui/apps/wallpaper/wallpaper_screen_priv.h"
#include "ui/apps/wallpaper/wallpaper_ui_helpers.h"
#include "ui/common/widgets/screen_common.h"

static void ApplySaveResult(const WallpaperResult& result) {
    auto& state = Wallpaper_State();
    const int index = state.request_index;
    if (result.succeeded > 0 && !result.save_as) {
        StandbyWallpaper_InvalidateDecoded();
    }
    if (!Wallpaper_RequestCurrent(true) || result.cancelled.load(std::memory_order_acquire)) {
        if (result.succeeded > 0 && Wallpaper_State().screen_alive.load() && Wallpaper_State().mode == UiMode::kList) {
            CollectWallpapers();
            RebuildListPage();
        }
        if (Wallpaper_State().screen_alive.load() && !Wallpaper_State().decode_paused.load() &&
            Wallpaper_State().mode == UiMode::kPreview) {
            if (result.succeeded > 0 && !result.save_as && state.preview.idx >= 0 &&
                state.preview.idx < static_cast<int>(state.list.files.size()) &&
                std::strcmp(state.list.files[state.preview.idx].path, result.path) == 0) {
                ClearPreviewImage();
                ScheduleLoadPreview(state.preview.idx);
            }
            UpdateEnableButtonUi();
            UpdateAdjustUi();
        }
        return;
    }
    if (result.succeeded == 0) {
        ScreenShowToast(result.error[0] != '\0' ? result.error : LangString_WALLPAPER_SAVE_FAIL);
        UpdateEnableButtonUi();
        UpdateAdjustUi();
        return;
    }

    if (result.save_as) {
        FileEntry entry;
        const char* name = std::strrchr(result.path, '/') + 1;
        std::snprintf(entry.name, sizeof(entry.name), "%s", name);
        std::snprintf(entry.path, sizeof(entry.path), "%s", result.path);
        const char* dot = std::strrchr(name, '.');
        const size_t title_length = dot ? static_cast<size_t>(dot - name) : std::strlen(name);
        std::memcpy(entry.title, name, title_length);
        entry.title[title_length] = '\0';
        entry.size_bytes = result.size_bytes;
        try {
            state.list.files.reserve(state.list.files.size() + 1);
            state.list.thumbs.reserve(state.list.files.size() + 1);
        } catch (const std::bad_alloc&) {
            ESP_LOGE(TAG, "operation=save stage=preview_entry result=no_memory action=keep_preview");
            ScreenShowToast(LangString_COMMON_MEMORY_RETRY);
            UpdateEnableButtonUi();
            UpdateAdjustUi();
            return;
        }
        CancelWallpaperDirectory();
        CancelThumbFill();
        // 当前预览接管已保存文件；返回列表时重新读取目录及排序。
        state.list.files.push_back(entry);
        state.list.thumbs.emplace_back();
        OpenPreview(static_cast<int>(state.list.files.size()) - 1);
        return;
    }

    Wallpaper_State().preview.adjust_open = false;
    ResetOrientState();
    Wallpaper_CloseDialog();

    if (index >= 0 && index < static_cast<int>(state.list.files.size())) {
        Wallpaper_State().list.files[static_cast<size_t>(index)].size_bytes = result.size_bytes;
        if (index < static_cast<int>(Wallpaper_State().list.thumbs.size())) {
            Wallpaper_State().list.thumbs[static_cast<size_t>(index)].reset();
            PaintCellThumb(index);
        }
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
}

void ScheduleSaveTransform(bool save_as) {
    auto& state = Wallpaper_State();
    if (!state.preview.adjust_open || state.preview.idx < 0 ||
        state.preview.idx >= static_cast<int>(state.list.files.size()) || WallpaperIsBusy() || state.request) {
        return;
    }
    bool accepted = false;
    try {
        WallpaperSaveOptions options;
        options.path = state.list.files[state.preview.idx].path;
        options.rotation = state.preview.rot_cw;
        options.mirror = state.preview.mirror_h;
        options.save_as = save_as;
        const bool landscape = state.preview.base && state.preview.base->width > state.preview.base->height;
        options.width = landscape ? std::max(DISPLAY_WIDTH, DISPLAY_HEIGHT) : std::min(DISPLAY_WIDTH, DISPLAY_HEIGHT);
        options.height = landscape ? std::min(DISPLAY_WIDTH, DISPLAY_HEIGHT) : std::max(DISPLAY_WIDTH, DISPLAY_HEIGHT);
        accepted = Wallpaper_TrackRequest(WallpaperSave(std::move(options)), ApplySaveResult, state.preview.idx);
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "operation=save stage=allocate result=no_memory");
    }
    if (!accepted) {
        ScreenShowToast(LangString_WALLPAPER_SAVE_FAIL);
    } else {
        Wallpaper_CloseDialog();
    }
    UpdateEnableButtonUi();
    UpdateAdjustUi();
}
