#include "wallpaper_screen/wallpaper_list.h"
#include "wallpaper_screen/wallpaper_screen_priv.h"
#include "wallpaper_screen/wallpaper_active.h"
#include "wallpaper_screen/wallpaper_multi.h"
#include "wallpaper_screen/wallpaper_ui_helpers.h"

#include <lvgl.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <utility>
#include <esp_log.h>

#include "assets/lang_config.h"
#include "SdCardManager.hpp"
#include "haptic_feedback.h"
#include "reader/reader.h"
#include "cloud_screen/push/wallpaper_cloud_library.h"
#include "sd_paths.h"

void RefreshActiveName() {
    Wallpaper_State().shutdown_name = Wallpaper_GetActiveFilename();
    Wallpaper_State().standby_name = Wallpaper_GetStandbyFilename();
}

// 将当前启用的待机/关机项排到前面，并按文件名稳定排序。
int ActivePinRank(const FileEntry& e) {
    const bool shutdown_on = !Wallpaper_State().shutdown_name.empty() && Wallpaper_State().shutdown_name == e.name;
    const bool standby_on = !Wallpaper_State().standby_name.empty() && Wallpaper_State().standby_name == e.name;
    if (standby_on && shutdown_on) {
        return 0;
    }
    if (standby_on) {
        return 1;
    }
    if (shutdown_on) {
        return 2;
    }
    return 3;
}

void SortFilesActiveFirst() {
    const size_t n = Wallpaper_State().list.files.size();
    if (n <= 1) {
        return;
    }
    if (Wallpaper_State().list.thumbs.size() != n) {
        Wallpaper_State().list.thumbs.resize(n);
    }
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [](size_t a, size_t b) {
        const int ra = ActivePinRank(Wallpaper_State().list.files[a]);
        const int rb = ActivePinRank(Wallpaper_State().list.files[b]);
        if (ra != rb) {
            return ra < rb;
        }
        return std::strcmp(Wallpaper_State().list.files[a].name, Wallpaper_State().list.files[b].name) < 0;
    });
    bool changed = false;
    for (size_t i = 0; i < n; ++i) {
        if (order[i] != i) {
            changed = true;
            break;
        }
    }
    if (!changed) {
        return;
    }
    std::vector<FileEntry> files(n);
    std::vector<std::unique_ptr<reader::RasterImage>> thumbs(n);
    std::vector<uint8_t> selected;
    const bool keep_sel = (Wallpaper_State().list.selected.size() == n);
    if (keep_sel) {
        selected.resize(n);
    }
    for (size_t i = 0; i < n; ++i) {
        files[i] = Wallpaper_State().list.files[order[i]];
        thumbs[i] = std::move(Wallpaper_State().list.thumbs[order[i]]);
        if (keep_sel) {
            selected[i] = Wallpaper_State().list.selected[order[i]];
        }
    }
    Wallpaper_State().list.files = std::move(files);
    Wallpaper_State().list.thumbs = std::move(thumbs);
    if (keep_sel) {
        Wallpaper_State().list.selected = std::move(selected);
    }
}

void OnActiveHydrated(void* /*user*/) {
    if (!Wallpaper_State().screen_alive) {
        return;
    }
    RefreshActiveName();
    SortFilesActiveFirst();
    if (Wallpaper_State().mode == UiMode::kList) {
        RebuildListPage();
    } else {
        UpdateEnableButtonUi();
    }
}

void CollectWallpapers() {
    std::vector<FileEntry> next_files;
    if (!Wallpaper_State().sd_ready) {
        Wallpaper_State().list.files.clear();
        Wallpaper_State().list.thumbs.clear();
        return;
    }
    DIR* dir = opendir(kPosixDir);
    if (dir == nullptr) {
        ESP_LOGW(TAG, "opendir %s failed", kPosixDir);
        Wallpaper_State().list.files.clear();
        Wallpaper_State().list.thumbs.clear();
        return;
    }
    while (next_files.size() < static_cast<size_t>(kMaxItems)) {
        errno = 0;
        dirent* ent = readdir(dir);
        if (ent == nullptr) {
            break;
        }
        if (!IsListableFile(ent->d_name)) {
            continue;
        }
        const size_t name_len = std::strlen(ent->d_name);
        if (name_len == 0 || name_len >= sizeof(FileEntry::name)) {
            continue;
        }
        FileEntry entry;
        std::memcpy(entry.name, ent->d_name, name_len + 1);
        const int n = std::snprintf(entry.path, sizeof(entry.path), "%s/%s", kPosixDir, entry.name);
        if (n < 0 || static_cast<size_t>(n) >= sizeof(entry.path)) {
            continue;
        }
        struct stat st {};
        if (stat(entry.path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
            continue;
        }
        entry.size_bytes = static_cast<size_t>(st.st_size);
        std::string image_name;
        std::string original_filename = entry.name;
        reader::ReadWallpaperMeta(entry.path, image_name, original_filename);
        const std::string title =
            reader::FormatWallpaperItemTitle(image_name, original_filename);
        std::snprintf(entry.title, sizeof(entry.title), "%s", title.c_str());
        next_files.push_back(entry);
    }
    closedir(dir);
    std::sort(next_files.begin(), next_files.end(),
              [](const FileEntry& a, const FileEntry& b) { return std::strcmp(a.name, b.name) < 0; });

    // 按文件名+大小复用已解码缩略图，避免每次进 app 清空再异步刷白
    std::vector<std::unique_ptr<reader::RasterImage>> next_thumbs(next_files.size());
    for (size_t i = 0; i < next_files.size(); ++i) {
        for (size_t j = 0; j < Wallpaper_State().list.files.size(); ++j) {
            if (j >= Wallpaper_State().list.thumbs.size()) {
                break;
            }
            if (std::strcmp(next_files[i].name, Wallpaper_State().list.files[j].name) != 0 ||
                next_files[i].size_bytes != Wallpaper_State().list.files[j].size_bytes) {
                continue;
            }
            next_thumbs[i] = std::move(Wallpaper_State().list.thumbs[j]);
            break;
        }
    }
    Wallpaper_State().list.files = std::move(next_files);
    Wallpaper_State().list.thumbs = std::move(next_thumbs);
    SortFilesActiveFirst();
}

int Wallpaper_ListPageCount() {
    if (Wallpaper_State().list.files.empty() || Wallpaper_State().list.list_per_page <= 0) {
        return 1;
    }
    return (static_cast<int>(Wallpaper_State().list.files.size()) + Wallpaper_State().list.list_per_page - 1) / Wallpaper_State().list.list_per_page;
}

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

// 只保留当前页缩略图，翻页时释放其他缓存。
void ClearOffPageThumbs() {
    if (Wallpaper_State().list.list_per_page <= 0 || Wallpaper_State().list.thumbs.empty()) {
        return;
    }
    const int start = Wallpaper_State().list.list_page * Wallpaper_State().list.list_per_page;
    const int end = start + Wallpaper_State().list.list_per_page;
    for (int i = 0; i < static_cast<int>(Wallpaper_State().list.thumbs.size()); ++i) {
        if (i < start || i >= end) {
            Wallpaper_State().list.thumbs[static_cast<size_t>(i)].reset();
        }
    }
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

    if (index >= 0 && index < static_cast<int>(Wallpaper_State().list.thumbs.size()) &&
        Wallpaper_State().list.thumbs[static_cast<size_t>(index)] != nullptr &&
        !Wallpaper_State().list.thumbs[static_cast<size_t>(index)]->empty()) {
        lv_obj_t* thumb = lv_image_create(cell);
        lv_image_set_src(thumb, &Wallpaper_State().list.thumbs[static_cast<size_t>(index)]->dsc);
        lv_obj_set_size(thumb, Wallpaper_State().list.thumbs[static_cast<size_t>(index)]->width,
                        Wallpaper_State().list.thumbs[static_cast<size_t>(index)]->height);
        lv_obj_center(thumb);
        lv_obj_clear_flag(thumb, LV_OBJ_FLAG_CLICKABLE);
    }

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

void RebuildListPageInternal(bool schedule_thumbs) {
    if (!Wallpaper_State().screen_alive || Wallpaper_State().ui.list_host == nullptr) {
        return;
    }
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
        return;
    }

    Wallpaper_ClampListPage();
    SyncWpSelectedSize();
    if (schedule_thumbs) {
        ClearOffPageThumbs();
        // 本页同步解码：墨水屏一次刷出封面，避免白格后再二次全刷
        FillPageThumbsSync(Wallpaper_State().list.list_page);
    }
    const lv_coord_t cell_w = Wallpaper_State().list.grid_cell_w;
    const lv_coord_t cover_h = Wallpaper_State().list.grid_cover_h;
    const int start = Wallpaper_State().list.list_page * Wallpaper_State().list.list_per_page;
    const int end = std::min(start + Wallpaper_State().list.list_per_page, static_cast<int>(Wallpaper_State().list.files.size()));
    for (int i = start; i < end; ++i) {
        CreateGridCell(Wallpaper_State().ui.list_host, i, cell_w, cover_h);
    }
    if (Wallpaper_State().ui.page_lbl != nullptr) {
        char foot[48];
        std::snprintf(foot, sizeof(foot), "%d/%d", Wallpaper_State().list.list_page + 1, Wallpaper_ListPageCount());
        lv_label_set_text(Wallpaper_State().ui.page_lbl, foot);
    }
    RefreshWpFooterMode();
    if (schedule_thumbs) {
        ScheduleThumbFill();  // 已同步填满时为空操作；失败槽位仍可后台补
    }
}

void RebuildListPage() {
    RebuildListPageInternal(true);
}

void PaintWallpaperList() {
    ESP_LOGI(TAG, "async RebuildListPage page=%d/%d begin", Wallpaper_State().list.list_page + 1, Wallpaper_ListPageCount());
    RebuildListPage();
    ESP_LOGI(TAG, "async RebuildListPage done");
}

void RequestWallpaperListRebuild() {
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

