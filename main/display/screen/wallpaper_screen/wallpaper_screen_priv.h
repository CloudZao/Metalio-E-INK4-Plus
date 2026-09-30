#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"
#include "reader_types.h"
#include "screen_common.h"
#include "sd_paths.h"
#include "ui_scale.h"

constexpr const char* TAG = "WallpaperScreen";
constexpr const char* kScreenId = "wallpaper";
constexpr const char* kPosixDir = SD_PATH_WALLPAPER;

// 源值对齐 397 wallpaper_screen；经 UiSx/UiSy 同比例到 470
constexpr lv_coord_t kPad = UiSx(16);
constexpr lv_coord_t kBorderW = UiSx(2);
constexpr lv_coord_t kFooterH = UiSy(36);
constexpr lv_coord_t kActionH = UiSy(52);
constexpr lv_coord_t kDeleteH = UiSy(48);
constexpr lv_coord_t kPreviewGap = UiSy(12); // 设置页纵向间距
constexpr lv_coord_t kWpMultiBtnH = UiSy(36);
constexpr lv_coord_t kWpUnderlineH = 2;
constexpr lv_coord_t kWpUnderlinePadHor = UiSx(5);
constexpr lv_coord_t kWpMultiDotSize = UiSx(6);
constexpr lv_coord_t kWpCheckSize = UiSx(24);
constexpr int64_t kWpSuppressRowClickUs = 400000;
constexpr uint64_t kWpCheckPaintDebounceUs = 280000;
constexpr int kGridCols = 3;
constexpr int kGridRows = 3;
constexpr lv_coord_t kGridColGap = UiSx(16);
constexpr lv_coord_t kGridRowGap = UiSy(20);
constexpr lv_coord_t kCoverRadius = UiSx(12);
constexpr lv_coord_t kBtnRadius = 999; // 胶囊
constexpr lv_coord_t kGridFrameBorder = 2; // 网格缩略外框；≥2px 减轻 DU 残影
constexpr lv_coord_t kGridCellWDefault = UiSx(120);
constexpr lv_coord_t kGridCoverHDefault = UiSy(160);
constexpr lv_coord_t kPreviewMetaH = UiSy(36);
constexpr lv_coord_t kPreviewBtnRowH = UiSy(48);
constexpr lv_coord_t kPreviewChipH = UiSy(32);
constexpr int kMaxItems = 64;
// 预览和缩略使用 L8；不要直接用全屏 I1，容易出现白底。
constexpr int kPreviewMaxW = UiSx(280);
constexpr int kPreviewMaxH = UiSy(420);
constexpr int kSaveDecodeMaxW = DISPLAY_WIDTH;
constexpr int kSaveDecodeMaxH = DISPLAY_HEIGHT;
constexpr uint32_t kSaveStack = 12 * 1024;

/** @brief 边框内可用边长 */
inline int FrameInner(lv_coord_t outer, lv_coord_t border) {
    const lv_coord_t inset = 2 * border;
    return static_cast<int>(outer > inset ? outer - inset : 1);
}

enum class UiMode : uint8_t { kList = 0, kPreview = 1 };

struct FileEntry {
    char name[96] = {};
    char title[160] = {};
    char path[192] = {};
    size_t size_bytes = 0;
};

struct WallpaperUiState {
    struct Widgets {
        lv_obj_t* screen = nullptr;
        lv_obj_t* list_body = nullptr;
        lv_obj_t* list_host = nullptr;
        lv_obj_t* list_empty = nullptr;
        lv_obj_t* page_lbl = nullptr; // 页码；多选时靠右下
        lv_obj_t* multi_bar = nullptr; // 取消 / 全选 / 移除
        lv_obj_t* status_label = nullptr;
        lv_obj_t* preview_body = nullptr;
        lv_obj_t* preview_title = nullptr;
        lv_obj_t* preview_meta = nullptr;
        lv_obj_t* preview_status = nullptr;
        lv_obj_t* preview_img = nullptr;
        lv_obj_t* preview_img_host = nullptr;
        lv_obj_t* actions_row = nullptr;
        lv_obj_t* actions_normal_row = nullptr;
        lv_obj_t* actions_adjust_row = nullptr;
        lv_obj_t* enable_shutdown_btn = nullptr;
        lv_obj_t* enable_shutdown_lbl = nullptr;
        lv_obj_t* enable_standby_btn = nullptr;
        lv_obj_t* enable_standby_lbl = nullptr;
        lv_obj_t* adjust_btn = nullptr;
        lv_obj_t* adjust_lbl = nullptr;
        lv_obj_t* rotate_left_btn = nullptr;
        lv_obj_t* rotate_right_btn = nullptr;
        lv_obj_t* mirror_btn = nullptr;
        lv_obj_t* bottom_slot = nullptr;
        lv_obj_t* delete_btn = nullptr;
        lv_obj_t* delete_lbl = nullptr;
        lv_obj_t* save_row = nullptr;
        lv_obj_t* cancel_btn = nullptr;
        lv_obj_t* save_btn = nullptr;
        lv_obj_t* save_lbl = nullptr;
        lv_obj_t* dialog_mask = nullptr;
    } ui;

    bool screen_alive = false;
    bool sd_ready = false;
    UiMode mode = UiMode::kList;
    // 每次 Create / DELETE 递增，异步回调会带 epoch，避免过期结果访问 UAF。
    uint32_t epoch = 0;
    std::string shutdown_name;
    std::string standby_name;
    ScreenPaintCoalesce wp_check_paint{};
    ScreenPaintCoalesce wallpaper_paint{};

    struct List {
        int list_page = 0;
        int list_per_page = kGridCols * kGridRows;
        bool multi = false;
        std::vector<uint8_t> selected;
        int64_t suppress_click_until_us = 0;
        int suppress_click_idx = -1;
        lv_coord_t body_h = 0;
        lv_coord_t grid_cell_w = kGridCellWDefault;
        lv_coord_t grid_cover_h = kGridCoverHDefault;
        std::vector<FileEntry> files;
        std::vector<std::unique_ptr<reader::RasterImage>> thumbs;
    } list;

    struct Preview {
        int idx = -1;
        reader::RasterImage* raster = nullptr;
        reader::RasterImage* base = nullptr;  // 调整态相对原预览的叠加基准
        bool adjust_open = false;
        uint8_t rot_cw = 0;   // 相对原图顺时针 90° 次数 0..3
        bool mirror_h = false;  // 原图水平镜像（先镜像再旋转）
        // 设置页的提示文案；为空时不绘制。
        char delete_meta_hint[80] = {};
    } preview;

    struct Workers {
        std::atomic<bool> load_busy{false};
        std::atomic<bool> thumb_busy{false};
        std::atomic<bool> enable_busy{false};
        std::atomic<bool> delete_busy{false};
        std::atomic<bool> save_busy{false};
        TaskHandle_t load_task = nullptr;
        TaskHandle_t thumb_task = nullptr;
        TaskHandle_t enable_task = nullptr;
        TaskHandle_t delete_task = nullptr;
        TaskHandle_t save_task = nullptr;
    } workers;
};

/** @brief 壁纸页 UI 单例状态 */
WallpaperUiState& Wallpaper_State();


/** @brief 切换壁纸页 UI 模式 */
void ShowMode(UiMode mode);
/** @brief 壁纸页盖板键短按 */
bool Wallpaper_OnVkKey(const char* key);
/** @brief 壁纸页盖板键长按 */
bool Wallpaper_OnVkKeyLongPress(const char* key);
/** @brief 壁纸页盖板键抬起 */
bool Wallpaper_OnVkKeyPressUp(const char* key);
/** @brief 壁纸页删除事件 */
void Wallpaper_OnScreenDeleted(lv_event_t* e);
