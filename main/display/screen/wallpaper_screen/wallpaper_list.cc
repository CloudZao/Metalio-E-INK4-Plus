#include "epd_content_policy.h"
#include "epd_refresh.h"
#include "wallpaper_screen/wallpaper_list.h"
#include "wallpaper_screen/wallpaper_screen_priv.h"
#include "wallpaper_screen/wallpaper_active.h"
#include "wallpaper_screen/wallpaper_multi.h"
#include "wallpaper_screen/wallpaper_ui_helpers.h"

#include <lvgl.h>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <utility>
#include <esp_log.h>

#include "assets/lang_config.h"
#include "SdCardManager.hpp"
#include "haptic_feedback.h"
#include "reader/reader.h"
#include "cloud_screen/push/wallpaper_cloud_library.h"
#include "sd_paths.h"
#include "sd_directory_page.h"
#include "sd_access.h"
#include "reader/list_thumbnail_cache.h"
#include <esp_timer.h>
#include "assets/lang_reader.h"

void RefreshActiveName() {
    Wallpaper_State().shutdown_name = Wallpaper_GetActiveFilename();
    Wallpaper_State().standby_name = Wallpaper_GetStandbyFilename();
}

// 目录顺序保持稳定，启用状态仅通过角标展示。

void OnActiveHydrated(void* /*user*/) {
    if (!Wallpaper_State().screen_alive) {
        return;
    }
    RefreshActiveName();
    if (Wallpaper_State().mode == UiMode::kList) {
        RebuildListPage();
    } else {
        UpdateEnableButtonUi();
    }
}

static SdDirectoryPage s_wallpaper_directory;
static lv_timer_t* s_wallpaper_directory_timer = nullptr;
static std::string s_requested_dir;
static int s_requested_page = -1;
static std::atomic<int> s_thumb_width{0}, s_thumb_height{0};
static std::atomic<int> s_thumb_page_size{kGridCols * kGridRows};

static bool WallpaperDirectoryFilter(const char* name, bool directory) {
    const char* relative = name + std::strlen(SD_PATH_WALLPAPER) + 1;
    return !directory && SdIsValidFileName(relative) && IsListableFile(relative);
}

static void WallpaperDirectoryMetadata(SdDirectoryPage::Entry& entry) {
    std::string name, original = entry.path.substr(entry.path.find_last_of('/') + 1);
    reader::ReadWallpaperMeta(entry.path.c_str(), name, original);
    entry.title = reader::FormatWallpaperItemTitle(name, original);
    const int width = s_thumb_width.load(), height = s_thumb_height.load();
    if (!entry.prefetch || width <= 0 || height <= 0 || s_wallpaper_directory.Cancelled() ||
        !reader::CanPrefetchListThumbnail(size_t(width) * height * s_thumb_page_size.load() *
            (entry.prefetch_distance + 1))) return;
    reader::RasterImage image;
    if (reader::GetListThumbnail(entry.path.c_str(), width, height, entry.bytes, entry.mtime, image)) return;
    auto cancelled = [](void*) { return s_wallpaper_directory.Cancelled(); };
    if (reader::DecodeImageFileToL8(entry.path.c_str(), width, height, image, cancelled, nullptr) && !cancelled(nullptr))
        reader::CacheListThumbnail(entry.path.c_str(), width, height, entry.bytes, entry.mtime, image);
}

void CancelWallpaperDirectory() {
    s_wallpaper_directory.Cancel();
    reader::ClearListThumbnailCache();
    if (s_wallpaper_directory_timer) lv_timer_del(s_wallpaper_directory_timer);
    s_wallpaper_directory_timer = nullptr;
    s_requested_page = -1;
}

bool WallpaperDirectoryLoading() { return Wallpaper_State().list.loading; }

static void WallpaperDirectoryTick(lv_timer_t*) {
    auto& st = Wallpaper_State();
    if (!st.screen_alive || sd_access::IsQuiescing()) {
        CancelWallpaperDirectory();
        return;
    }
    SdDirectoryPage::Result result;
    const bool received = s_wallpaper_directory.Poll(result);
    if (!received && st.list.loading && !st.list.loading_shown && esp_timer_get_time() - st.list.requested_us >= 180000) {
        st.list.loading_shown = true; RebuildListPage();
    }
    if (received) {
        if (result.page != st.list.list_page) return;
        CancelThumbFill();
        DetachRasterUsers();
        st.list.files.clear();
        st.list.thumbs.clear();
        for (const auto& item : result.entries) {
            if (item.path.size() >= sizeof(FileEntry::path)) continue;
            FileEntry entry;
            const std::string relative = item.path.substr(std::strlen(SD_PATH_WALLPAPER) + 1);
            if (relative.size() >= sizeof(entry.name)) continue;
            std::snprintf(entry.name, sizeof(entry.name), "%s", relative.c_str());
            std::snprintf(entry.path, sizeof(entry.path), "%s", item.path.c_str());
            const std::string& title = item.title;
            const size_t length = SdUtf8PrefixLength(title.c_str(), sizeof(entry.title) - 1);
            std::memcpy(entry.title, title.c_str(), length);
            entry.title[length] = '\0';
            entry.size_bytes = item.bytes;
            entry.mtime = item.mtime;
            st.list.files.push_back(entry);
        }
        st.list.thumbs.resize(st.list.files.size());
        for (size_t i = 0; i < st.list.files.size(); ++i) {
            const auto& entry = st.list.files[i];
            auto image = std::make_unique<reader::RasterImage>();
            if (reader::GetListThumbnail(entry.path, FrameInner(st.list.grid_cell_w, kGridFrameBorder),
                FrameInner(st.list.grid_cover_h, kGridFrameBorder), entry.size_bytes, entry.mtime, *image))
                st.list.thumbs[i] = std::move(image);
        }
        st.list.loading = false;
        st.list.failed = !result.ok;
        st.list.loaded_page = result.page;
        RebuildListPage();
        if (!result.ok) { CancelWallpaperDirectory(); return; }
    }
    if (!st.list.loading && st.ui.page_lbl && st.mode == UiMode::kList) {
        if (s_wallpaper_directory.Counted()) {
            lv_label_set_text_fmt(st.ui.page_lbl, "%d/%d", st.list.list_page + 1, s_wallpaper_directory.Pages());
            lv_timer_del(s_wallpaper_directory_timer);
            s_wallpaper_directory_timer = nullptr;
        } else {
            char text[32]; std::snprintf(text, sizeof(text), "%d/…", st.list.list_page + 1);
            if (std::strcmp(lv_label_get_text(st.ui.page_lbl), text)) lv_label_set_text(st.ui.page_lbl, text);
        }
    }
}

static void RequestWallpaperDirectory(bool refresh) {
    auto& st = Wallpaper_State();
    s_requested_dir = SD_PATH_WALLPAPER;
    s_requested_page = st.list.list_page;
    st.list.loading_shown = false;
    st.list.requested_us = esp_timer_get_time();
    s_thumb_width.store(FrameInner(st.list.grid_cell_w, kGridFrameBorder));
    s_thumb_height.store(FrameInner(st.list.grid_cover_h, kGridFrameBorder));
    s_thumb_page_size.store(st.list.list_per_page);
    st.list.loading = true;
    st.list.failed = false;
    st.list.loaded_page = -1;
    CancelThumbFill();
    s_wallpaper_directory.Request({s_requested_dir}, WallpaperDirectoryFilter, s_requested_page,
        st.list.list_per_page, refresh, WallpaperDirectoryMetadata);
    if (!s_wallpaper_directory_timer) s_wallpaper_directory_timer = lv_timer_create(WallpaperDirectoryTick, 100, nullptr);
    if (!s_wallpaper_directory_timer) {
        st.list.loading = false; st.list.failed = true;
        ESP_LOGE(TAG, "wallpaper directory timer creation failed");
    }
}

void CollectWallpapers() { reader::ClearListThumbnailCache(); Wallpaper_State().list.list_page = 0; RequestWallpaperDirectory(true); }

int Wallpaper_ListPageCount() { return s_wallpaper_directory.Pages(); }

void Wallpaper_ClampListPage() {
    const int pages = Wallpaper_ListPageCount();
    if (Wallpaper_State().list.list_page < 0) {
        Wallpaper_State().list.list_page = 0;
    }
    if (Wallpaper_State().list.list_page >= pages) {
        Wallpaper_State().list.list_page = pages - 1;
    }
}

// 先移除还在引用 RasterImage 的 lv_image，再释放像素缓冲。
void DetachRasterUsers() {
    if (Wallpaper_State().ui.preview_img != nullptr) {
        lv_image_set_src(Wallpaper_State().ui.preview_img, nullptr);
        lv_obj_add_flag(Wallpaper_State().ui.preview_img, LV_OBJ_FLAG_HIDDEN);
    }
    if (Wallpaper_State().ui.list_host != nullptr) {
        lv_obj_clean(Wallpaper_State().ui.list_host);
        Wallpaper_State().ui.list_empty = nullptr;
    }
}

lv_obj_t* FindCell(int index) {
    if (Wallpaper_State().ui.list_host == nullptr || !lv_obj_is_valid(Wallpaper_State().ui.list_host)) {
        return nullptr;
    }
    const uint32_t n = lv_obj_get_child_count(Wallpaper_State().ui.list_host);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* cell = lv_obj_get_child(Wallpaper_State().ui.list_host, i);
        if (cell != nullptr &&
            static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(cell))) == index) {
            return cell;
        }
    }
    return nullptr;
}

void StripCellThumb(lv_obj_t* cell) {
    uint32_t i = 0;
    while (i < lv_obj_get_child_count(cell)) {
        lv_obj_t* child = lv_obj_get_child(cell, i);
        if (child != nullptr &&
            (lv_obj_check_type(child, &lv_image_class) || lv_obj_check_type(child, &lv_label_class))) {
            lv_obj_delete(child);
            continue;
        }
        ++i;
    }
}

void ShowCellStatus(lv_obj_t* cell, const char* text) {
    lv_obj_t* lbl = lv_label_create(cell);
    lv_label_set_text(lbl, text);
    // 布局前单元格尺寸尚未确定，使用网格宽度计算可用区域，避免负宽裁切。
    lv_obj_set_width(lbl, FrameInner(Wallpaper_State().list.grid_cell_w, kGridFrameBorder));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl, Wallpaper_UiFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);
    lv_obj_move_to_index(lbl, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
}

void PaintCellThumb(int index) {
    lv_obj_t* cell = FindCell(index);
    if (cell == nullptr || !lv_obj_is_valid(cell)) {
        return;
    }
    StripCellThumb(cell);
    if (index < 0 || index >= static_cast<int>(Wallpaper_State().list.thumbs.size())) {
        ShowCellStatus(cell, Lang::Strings::WALLPAPER_LOADING);
        return;
    }
    reader::RasterImage* img = Wallpaper_State().list.thumbs[static_cast<size_t>(index)].get();
    if (img == nullptr) {
        ShowCellStatus(cell, Lang::Strings::WALLPAPER_LOADING);
        return;
    }
    if (img->empty()) {
        char message[80] = {};
        Wallpaper_FormatDecodeFailure(message, sizeof(message), Wallpaper_State().list.files[static_cast<size_t>(index)].thumb_error);
        ShowCellStatus(cell, message);
        return;
    }
    img->BindDsc();
    lv_obj_t* thumb = lv_image_create(cell);
    lv_image_set_src(thumb, &img->dsc);
    EpdSetContentRole(thumb, EpdContentRole::WallpaperThumbnail);
    lv_obj_set_size(thumb, img->width, img->height);
    lv_obj_center(thumb);
    lv_obj_move_to_index(thumb, 0);
    lv_obj_clear_flag(thumb, LV_OBJ_FLAG_CLICKABLE);
}

// 网格单元格：封面、启用状态角标和多选勾选框。
lv_obj_t* CreateGridCell(lv_obj_t* parent, int index, lv_coord_t cell_w, lv_coord_t cover_h) {
    lv_obj_t* cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, cell_w, cover_h);
    lv_obj_set_style_radius(cell, kCoverRadius, 0);
    lv_obj_set_style_clip_corner(cell, true, 0);
    lv_obj_set_style_border_width(cell, kGridFrameBorder, 0);
    lv_obj_set_style_border_color(cell, lv_color_black(), 0);
    lv_obj_set_style_pad_all(cell, kGridFrameBorder, 0);
    lv_obj_set_style_bg_color(cell, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    Wallpaper_DisableScroll(cell);
    lv_obj_set_user_data(cell, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    HapticAttachClick(cell);
    lv_obj_add_event_cb(cell, Wallpaper_OnRowClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    lv_obj_add_event_cb(cell, Wallpaper_OnRowLongPressed, LV_EVENT_LONG_PRESSED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(index)));

    PaintCellThumb(index);

    if (index >= 0 && index < static_cast<int>(Wallpaper_State().list.files.size())) {
        const char* name = Wallpaper_State().list.files[static_cast<size_t>(index)].name;
        const bool shutdown_on = !Wallpaper_State().shutdown_name.empty() && Wallpaper_State().shutdown_name == name;
        const bool standby_on = !Wallpaper_State().standby_name.empty() && Wallpaper_State().standby_name == name;
        if (shutdown_on || standby_on) {
            lv_obj_t* tips = lv_obj_create(cell);
            lv_obj_remove_style_all(tips);
            lv_obj_set_size(tips, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(tips, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(tips, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(tips, 4, 0);
            // 多选勾在右上，角标改左上避免叠盖
            lv_obj_align(tips, Wallpaper_State().list.multi ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_RIGHT, Wallpaper_State().list.multi ? 4 : -4,
                         4);
            lv_obj_clear_flag(tips, LV_OBJ_FLAG_CLICKABLE);
            Wallpaper_DisableScroll(tips);
            if (shutdown_on) {
                DrawPowerTipIcon(tips);
            }
            if (standby_on) {
                DrawMoonTipIcon(tips);
            }
        }
    }

    if (Wallpaper_State().list.multi) {
        lv_obj_t* check = lv_obj_create(cell);
        lv_obj_remove_style_all(check);
        lv_obj_set_size(check, kWpCheckSize, kWpCheckSize);
        lv_obj_set_style_bg_color(check, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(check, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(check, lv_color_black(), 0);
        lv_obj_set_style_border_width(check, kGridFrameBorder, 0);
        lv_obj_set_style_radius(check, 4, 0);
        lv_obj_align(check, LV_ALIGN_TOP_RIGHT, -4, 4);
        Wallpaper_DisableScroll(check);
        lv_obj_clear_flag(check, LV_OBJ_FLAG_CLICKABLE);
        if (WpItemSelected(index)) {
            lv_obj_t* mark = lv_label_create(check);
            lv_label_set_text(mark, "√");
            lv_obj_set_style_text_font(mark, Wallpaper_UiFont(), 0);
            lv_obj_set_style_text_color(mark, lv_color_black(), 0);
            lv_obj_center(mark);
            lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    return cell;
}

void RebuildListPageInternal() {
    auto& state = Wallpaper_State();
    if (state.sd_ready && (s_requested_dir != SD_PATH_WALLPAPER || s_requested_page != state.list.list_page))
        RequestWallpaperDirectory(false);
    if (!Wallpaper_State().screen_alive || Wallpaper_State().ui.list_host == nullptr) {
        return;
    }
    if (state.list.loading && !state.list.files.empty() && esp_timer_get_time() - state.list.requested_us < 180000) return;
    lv_obj_clean(Wallpaper_State().ui.list_host);
    Wallpaper_State().ui.list_empty = nullptr;
    Wallpaper_DisableScroll(Wallpaper_State().ui.list_host);

    lv_obj_set_style_layout(Wallpaper_State().ui.list_host, LV_LAYOUT_FLEX, 0);
    lv_obj_set_flex_flow(Wallpaper_State().ui.list_host, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(Wallpaper_State().ui.list_host, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(Wallpaper_State().ui.list_host, kGridRowGap, 0);
    lv_obj_set_style_pad_column(Wallpaper_State().ui.list_host, kGridColGap, 0);

    if (!Wallpaper_State().sd_ready) {
        lv_obj_set_flex_flow(Wallpaper_State().ui.list_host, LV_FLEX_FLOW_COLUMN);
        Wallpaper_ShowMessage(Wallpaper_State().ui.list_host, Lang::Strings::WALLPAPER_NO_SD);
        if (Wallpaper_State().ui.page_lbl != nullptr) {
            lv_label_set_text(Wallpaper_State().ui.page_lbl, "1/1");
        }
        RefreshWpFooterMode();
        CancelThumbFill();
        return;
    }
    if (Wallpaper_State().list.loading || Wallpaper_State().list.failed) {
        Wallpaper_ShowMessage(Wallpaper_State().ui.list_host, Wallpaper_State().list.loading ?
            Lang::Strings::BOOK_LOADING : Lang::Strings::DIRECTORY_READ_FAILED);
        return;
    }
    if (Wallpaper_State().list.files.empty()) {
        lv_obj_set_flex_flow(Wallpaper_State().ui.list_host, LV_FLEX_FLOW_COLUMN);
        char tip[96];
        std::snprintf(tip, sizeof(tip), Lang::Strings::WALLPAPER_EMPTY_FMT, SdUserPath(SD_PATH_WALLPAPER));
        Wallpaper_ShowMessage(Wallpaper_State().ui.list_host, tip);
        if (Wallpaper_State().ui.page_lbl != nullptr) {
            lv_label_set_text(Wallpaper_State().ui.page_lbl, "1/1");
        }
        RefreshWpFooterMode();
        CancelThumbFill();
        return;
    }

    Wallpaper_ClampListPage();

    // 仅保留当前页及相邻页缩略图，缓存占用不随壁纸数量增长。
    const lv_coord_t cell_w = Wallpaper_State().list.grid_cell_w;
    const lv_coord_t cover_h = Wallpaper_State().list.grid_cover_h;
    const int start = 0;
    const int end = std::min(start + Wallpaper_State().list.list_per_page, static_cast<int>(Wallpaper_State().list.files.size()));
    const int cache_start = std::max(0, start - Wallpaper_State().list.list_per_page);
    const int cache_end = end + Wallpaper_State().list.list_per_page;
    for (size_t i = 0; i < Wallpaper_State().list.thumbs.size(); ++i) {
        if (static_cast<int>(i) < cache_start || static_cast<int>(i) >= cache_end) {
            Wallpaper_State().list.thumbs[i].reset();
        }
    }
    for (int i = start; i < end; ++i) {
        CreateGridCell(Wallpaper_State().ui.list_host, i, cell_w, cover_h);
    }
    if (Wallpaper_State().ui.page_lbl != nullptr) {
        char foot[48];
        std::snprintf(foot, sizeof(foot), "%d/%d", Wallpaper_State().list.list_page + 1, Wallpaper_ListPageCount());
        lv_label_set_text(Wallpaper_State().ui.page_lbl, foot);
    }
    RefreshWpFooterMode();
    // 页面帧提交后再启动解码，避免缩略图任务延迟页面清屏。
    EpdAfterNextSubmit(ScheduleThumbFill);
}

void RebuildListPage() {
    RebuildListPageInternal();
}

static std::atomic<bool> s_wallpaper_screen_change{false};

void PaintWallpaperList() {
    ESP_LOGI(TAG, "async RebuildListPage page=%d/%d begin", Wallpaper_State().list.list_page + 1, Wallpaper_ListPageCount());
    RebuildListPage();
    if (s_wallpaper_screen_change.exchange(false)) {
        Wallpaper_RevealAfterWhite();
    }
    ESP_LOGI(TAG, "async RebuildListPage done");
}

void Wallpaper_RevealAfterWhite() {
    EpdCancelPageTurn();
    EpdRequestScreenChange();
    if (Wallpaper_State().ui.screen != nullptr) {
        lv_obj_invalidate(Wallpaper_State().ui.screen);
    }
}

void RequestWallpaperListRebuild(bool screen_change) {
    if (screen_change) s_wallpaper_screen_change.store(true);
    if (Wallpaper_State().wallpaper_paint.paint == nullptr) {
        Wallpaper_State().wallpaper_paint.paint = PaintWallpaperList;
    }
    ScreenPaintCoalesceRequest(&Wallpaper_State().wallpaper_paint);
}

void BuildListBody(lv_obj_t* scr, lv_coord_t status_h) {
    // ---- 网格（同书架 3×3，均分铺满）----
    lv_obj_t* list_body = lv_obj_create(scr);
    lv_obj_remove_style_all(list_body);
    lv_obj_set_size(list_body, LV_HOR_RES, Wallpaper_State().list.body_h);
    lv_obj_align(list_body, LV_ALIGN_TOP_MID, 0, status_h);
    lv_obj_set_style_bg_opa(list_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(list_body, kPad, 0);
    Wallpaper_DisableScroll(list_body);
    lv_obj_clear_flag(list_body, LV_OBJ_FLAG_CLICKABLE);
    Wallpaper_State().ui.list_body = list_body;

    lv_obj_t* list_host = lv_obj_create(list_body);
    lv_obj_remove_style_all(list_host);
    lv_obj_set_size(list_host, Wallpaper_ContentWidth(), Wallpaper_State().list.body_h - kPad * 2);
    lv_obj_align(list_host, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_flex_flow(list_host, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(list_host, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list_host, kGridRowGap, 0);
    lv_obj_set_style_pad_column(list_host, kGridColGap, 0);
    Wallpaper_DisableScroll(list_host);
    Wallpaper_State().ui.list_host = list_host;
}

