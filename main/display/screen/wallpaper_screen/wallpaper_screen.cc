#include "wallpaper_screen/wallpaper_screen.h"
#include "wallpaper_screen/wallpaper_screen_priv.h"
#include "wallpaper_screen/wallpaper_active.h"
#include "wallpaper_screen/preview/wallpaper_adjust.h"
#include "wallpaper_screen/wallpaper_list.h"
#include "wallpaper_screen/wallpaper_multi.h"
#include "wallpaper_screen/preview/wallpaper_preview.h"
#include "wallpaper_screen/wallpaper_ui_helpers.h"

#include <lvgl.h>
#include <algorithm>
#include <cstring>
#include <esp_log.h>

#include "assets/lang_config.h"
#include "SdCardManager.hpp"
#include "screen_common.h"
#include "vk_key_handler.h"
#include "vk_page_repeat.h"

WallpaperUiState& Wallpaper_State() {
    static WallpaperUiState s;
    return s;
}

void AsyncCloseOverwriteDialog(void* /*user_data*/) {
    CloseOverwriteDialog();
}

void AsyncExitAdjustMode(void* /*user_data*/) {
    ExitAdjustMode(true);
}

void AsyncClosePreview(void* /*user_data*/) {
    ESP_LOGI(TAG, "async ClosePreview begin");
    ClosePreview();
    ESP_LOGI(TAG, "async ClosePreview done");
}

void ShowMode(UiMode mode) {
    Wallpaper_State().mode = mode;
    if (Wallpaper_State().ui.list_body != nullptr) {
        if (mode == UiMode::kList) {
            lv_obj_clear_flag(Wallpaper_State().ui.list_body, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(Wallpaper_State().ui.list_body, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (Wallpaper_State().ui.preview_body != nullptr) {
        if (mode == UiMode::kPreview) {
            lv_obj_clear_flag(Wallpaper_State().ui.preview_body, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(Wallpaper_State().ui.preview_body, LV_OBJ_FLAG_HIDDEN);
        }
    }
    RefreshWpFooterMode();
}

bool WallpaperPageRepeatStep(int page_delta) {
    if (page_delta == 0 || Wallpaper_State().mode != UiMode::kList) {
        return false;
    }
    const int last = std::max(0, Wallpaper_ListPageCount() - 1);
    int next = Wallpaper_State().list.list_page + page_delta;
    if (next < 0) {
        next = 0;
    } else if (next > last) {
        next = last;
    }
    if (next == Wallpaper_State().list.list_page) {
        return false;
    }
    Wallpaper_State().list.list_page = next;
    RequestWallpaperListRebuild();
    return page_delta < 0 ? Wallpaper_State().list.list_page > 0 : Wallpaper_State().list.list_page < last;
}

bool Wallpaper_OnVkKeyLongPress(const char* key) {
    return VkPageRepeatTryStart(key, WallpaperPageRepeatStep);
}

bool Wallpaper_OnVkKeyPressUp(const char* key) {
    return VkPageRepeatOnPressUp(key);
}

bool Wallpaper_OnVkKey(const char* key) {
    if (key == nullptr) {
        return false;
    }
    if (Wallpaper_State().mode == UiMode::kPreview) {
        // 覆盖确认框优先关掉
        if (Wallpaper_State().ui.dialog_mask != nullptr &&
            (std::strcmp(key, "vk_prev") == 0 || std::strcmp(key, "vk_home") == 0)) {
            ScreenLvAsync(AsyncCloseOverwriteDialog);
            return true;
        }
        // 调整态：返回/Home 先退出调整（丢弃未保存变换），不直接关预览
        if (Wallpaper_State().preview.adjust_open &&
            (std::strcmp(key, "vk_prev") == 0 || std::strcmp(key, "vk_home") == 0)) {
            if (Wallpaper_State().workers.save_busy.load()) {
                ++Wallpaper_State().epoch;
                Wallpaper_State().workers.save_busy.store(false);
                Wallpaper_State().workers.save_task = nullptr;
            }
            ScreenLvAsync(AsyncExitAdjustMode);
            return true;
        }
        // 详情页：vk_home / vk_prev 均回网格（不直接回系统首页）
        if (std::strcmp(key, "vk_prev") == 0 || std::strcmp(key, "vk_home") == 0) {
            ESP_LOGI(TAG, "%s preview -> queue ClosePreview", key);
            ScreenLvAsync(AsyncClosePreview);
            return true;
        }
        if (std::strcmp(key, "vk_next") == 0) {
            return true;
        }
        return false;
    }
    if (std::strcmp(key, "vk_prev") == 0) {
        if (Wallpaper_State().list.list_page > 0) {
            --Wallpaper_State().list.list_page;
            ESP_LOGI(TAG, "vk_prev list -> page %d queue rebuild", Wallpaper_State().list.list_page + 1);
            RequestWallpaperListRebuild();
            return true;
        }
        // 第一页：多选中则退出批量，否则交给默认返回上一屏
        if (Wallpaper_State().list.multi) {
            ExitWpMultiMode(true);
            return true;
        }
        return false;
    }
    if (std::strcmp(key, "vk_next") == 0) {
        if (Wallpaper_State().list.list_page + 1 < Wallpaper_ListPageCount()) {
            ++Wallpaper_State().list.list_page;
            ESP_LOGI(TAG, "vk_next list -> page %d queue rebuild", Wallpaper_State().list.list_page + 1);
            RequestWallpaperListRebuild();
        } else {
            ESP_LOGI(TAG, "vk_next list already last page=%d", Wallpaper_State().list.list_page + 1);
        }
        return true;
    }
    return false;
}

void Wallpaper_OnScreenDeleted(lv_event_t* e) {
    if (lv_event_get_target(e) != Wallpaper_State().ui.screen) {
        return;
    }
    Wallpaper_State().screen_alive = false;
    ++Wallpaper_State().epoch;  // 作废在飞 preview/thumb/enable/delete/save 回调
    // 先拆掉引用 RasterImage 的控件；缩略图缓存在进程内复用，下次进 app 免白屏
    DetachRasterUsers();
    ClearPreviewImage();
    Wallpaper_State().preview.idx = -1;
    Wallpaper_State().mode = UiMode::kList;
    Wallpaper_State().list.multi = false;
    Wallpaper_State().preview.adjust_open = false;
    ResetOrientState();
    Wallpaper_State().list.selected.clear();
    Wallpaper_State().list.suppress_click_until_us = 0;
    Wallpaper_State().list.suppress_click_idx = -1;
    Wallpaper_State().workers.delete_busy.store(false);
    Wallpaper_State().workers.save_busy.store(false);
    ScreenPaintCoalesceReset(&Wallpaper_State().wp_check_paint);
    ScreenPaintCoalesceReset(&Wallpaper_State().wallpaper_paint);
    Wallpaper_State().ui = {};
}

lv_obj_t* WallpaperScreen::Create() {
    // 旧屏可能仍在 async delete：先拆引用，递增 epoch 丢弃在飞任务；缩略图按文件复用
    Wallpaper_State().screen_alive = false;
    ++Wallpaper_State().epoch;
    DetachRasterUsers();
    ClearPreviewImage();
    Wallpaper_State().ui = {};
    Wallpaper_State().mode = UiMode::kList;
    Wallpaper_State().list.list_page = 0;
    Wallpaper_State().preview.idx = -1;
    Wallpaper_State().list.multi = false;
    Wallpaper_State().preview.adjust_open = false;
    ResetOrientState();
    Wallpaper_State().workers.save_busy.store(false);
    Wallpaper_State().list.selected.clear();
    Wallpaper_State().list.suppress_click_until_us = 0;
    Wallpaper_State().list.suppress_click_idx = -1;
    ScreenPaintCoalesceReset(&Wallpaper_State().wp_check_paint);
    ScreenPaintCoalesceReset(&Wallpaper_State().wallpaper_paint);

    Wallpaper_State().sd_ready = SdCardManager::GetInstance().IsMounted() && SdEnsureAppLayout();
    // 禁止在 LVGL 任务读 NVS：只用内存缓存 / SD 回退；NVS 由后台 hydrate。
    RefreshActiveName();
    if (Wallpaper_State().sd_ready) {
        CollectWallpapers();
    } else {
        Wallpaper_State().list.files.clear();
        Wallpaper_State().list.thumbs.clear();
    }
    Wallpaper_State().list.selected.assign(Wallpaper_State().list.files.size(), 0);

    ScreenSetIsHome(false);

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, Wallpaper_UiFont(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    Wallpaper_DisableScroll(scr);
    Wallpaper_State().ui.screen = scr;
    lv_obj_add_event_cb(scr, Wallpaper_OnScreenDeleted, LV_EVENT_DELETE, nullptr);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    Wallpaper_State().ui.status_label = status.status_label;
    if (status.status_label) {
        lv_label_set_text(status.status_label, Lang::Strings::HOME_APP_WALLPAPER);
    }
    if (status.notification_label) {
        lv_obj_add_flag(status.notification_label, LV_OBJ_FLAG_HIDDEN);
    }

    Wallpaper_State().list.body_h = LV_VER_RES - status.height - kFooterH;
    Wallpaper_State().list.list_per_page = kGridCols * kGridRows;
    // 九格均分可用宽高（加大间距后仍铺满，不再用 4:3 压矮封面）
    Wallpaper_State().list.grid_cell_w = (Wallpaper_ContentWidth() - kGridColGap * (kGridCols - 1)) / kGridCols;
    {
        const lv_coord_t usable = Wallpaper_State().list.body_h - kPad * 2;
        lv_coord_t cover_h = (usable - kGridRowGap * (kGridRows - 1)) / kGridRows;
        if (cover_h < 72) {
            cover_h = 72;
        }
        Wallpaper_State().list.grid_cover_h = cover_h;
    }

    BuildListBody(scr, status.height);
    BuildPreviewBody(scr, status.height);
    BuildFooter(scr);

    Wallpaper_State().screen_alive = true;
    RebuildListPage();
    ShowMode(UiMode::kList);
    // 后台灌 NVS → 缓存，再 lv_async 刷新关机/待机标记
    Wallpaper_RequestHydrateFromNvs(OnActiveHydrated, nullptr);

    VkKey_AttachScreen(scr, kScreenId,
                       VkKeyScreenDesc{WallpaperScreen::Create, Wallpaper_OnVkKey, nullptr, nullptr, nullptr,
                                       nullptr, Wallpaper_OnVkKeyLongPress, Wallpaper_OnVkKeyPressUp});
    return scr;
}
