#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <lvgl.h>

#include "book_screen/reader/book_reader_prefs.h"
#include "reader/book_library.h"
#include "reader/book_session.h"
#include "reader/reader_types.h"
#include "screen_common.h"
#include "sd_paths.h"
#include "ui_scale.h"

constexpr const char* TAG = "BookScreen";
constexpr const char* kScreenLibrary = "book";         // 阅读首页
constexpr const char* kScreenBookshelf = "book_shelf"; // 我的书架网格
constexpr const char* kScreenDetail = "book_detail";
constexpr const char* kScreenRead = "book_read";
// 轻量 epdfont：索引常驻 + 按页预热位图（替代整库 lv_binfont）
// 命名：misans_{px}_{bpp}.ef ，默认 25px / 2bpp；实际路径由 NVS 偏好决定
constexpr const char* kBookFontPath = SD_PATH_BOOK_FONT;
constexpr const char* kBookFontPathAlt = SD_PATH_BOOK_FONT_ALT;
// 源值对齐 397 book_screen；经 UiSx/UiSy 同比例到 470
constexpr lv_coord_t kListPad = UiSx(16);   // 首页/书架外边距
constexpr lv_coord_t kFooterH = UiSy(36);   // 阅读底栏槽高（397=36）
// 分页高度相对正文区再收一点：末行字形/下划线(+4)可能略超 line_height，避免钻进底栏
constexpr lv_coord_t kReadViewportBottomSlack = UiSy(8);
constexpr lv_coord_t kCoverFrameBorder = 2; // 封面外框；≥2px 减轻 DU 擦白残影
constexpr lv_coord_t kCoverSpineInset = UiSx(6);  // 书脊竖线相对左边框内缩
constexpr lv_coord_t kRowBorderW = UiSx(2);
constexpr lv_coord_t kRowRadius = UiSx(8);
constexpr lv_coord_t kReadPadTop = kListPad;  // 与左右边距一致
constexpr int kFontListPageSize = 5;         // 设置卡字体列表每页固定行数
constexpr uint32_t kLayoutDebounceMs = 300;   // 抬起且停顿后再排版（定时器内仍会等手指）
constexpr uint32_t kLayoutHintTickMs = 500;   // 「完成」提示到期检查
constexpr int64_t kLayoutHintDoneUs = 1500 * 1000;  // 「完成」显示时长

struct ReadFontEntry {
    std::string file;
    int size_px = 0;
};
constexpr lv_coord_t kDetailCoverMaxW = UiSx(152);
constexpr lv_coord_t kDetailCoverMaxH = UiSy(214);

struct BookUiState {
    struct Shelf {
        std::vector<reader::BookInfo> books;
        int selected = -1;
        int list_page = 0;
        int list_page_size = 9; // 书架默认 3×3；列表模式按行高算
        lv_coord_t shelf_cover_h = 0; // 书架封面高（按三行可用高度算）
        lv_coord_t shelf_content_h = 0; // 标题行以下可用高度
        int shelf_view = kBookReaderShelfViewCover;
        std::string shelf_dir; // 列表模式当前目录（绝对路径）
        std::vector<reader::BookDirEntry> shelf_entries;
        lv_obj_t* shelf_title_lbl = nullptr;
        lv_obj_t* shelf_view_cover_btn = nullptr;
        lv_obj_t* shelf_view_list_btn = nullptr;

        enum class NavRoot : uint8_t { kHome = 0, kShelf };
        NavRoot back_root = NavRoot::kHome;

        lv_obj_t* list_scr = nullptr;
        lv_obj_t* list_body = nullptr;
        lv_obj_t* list_footer = nullptr; // 页码；多选时靠右下
        lv_obj_t* multi_bar = nullptr;   // 取消 / 全选 / 移除
        bool shelf_multi = false;
        std::vector<uint8_t> shelf_selected; // 与 books 等长
        int64_t shelf_suppress_click_until_us = 0;
        int shelf_suppress_click_idx = -1;

        // 首页最近阅读 / 书架网格：旁路封面像素（控件只引用 dsc）
        std::vector<std::unique_ptr<reader::RasterImage>> recent_covers;
        std::vector<std::unique_ptr<reader::RasterImage>> shelf_covers;
        // 列表/目录：无旁路时串行后台抽内嵌；token 作废翻页/离页迟到回调
        std::atomic<uint32_t> list_cover_token{0};
        struct ListCoverPending {
            reader::BookInfo info;
            int max_w = 0;
            int max_h = 0;
            lv_obj_t* host = nullptr;
            reader::RasterImage* slot = nullptr;  // recent/shelf/detail_cover，非拥有
        };
        std::vector<ListCoverPending> list_cover_pending;
        // 已确认无内嵌封面的路径（进程内）；避免每次回书架重复 Open 抽封面
        std::vector<std::string> cover_embed_miss;
    } shelf;

    struct Detail {
        lv_obj_t* detail_scr = nullptr;
        reader::RasterImage detail_cover;
        // 无旁路时后台补文件内封面；token 作废离开/换书的迟到回调
        std::atomic<uint32_t> detail_cover_token{0};
        lv_obj_t* detail_cover_host = nullptr;
        lv_coord_t detail_cover_fw = kDetailCoverMaxW; // 详情封面外框宽
        lv_coord_t detail_cover_fh = kDetailCoverMaxH; // 详情封面外框高
        lv_obj_t* detail_title_lbl = nullptr;
        lv_obj_t* detail_meta_lbl = nullptr;
        reader::BookFormat detail_format = reader::BookFormat::kTxt;
    } detail;

    struct Reader {
        std::unique_ptr<reader::BookSession> session;
        lv_obj_t* read_scr = nullptr;
        lv_obj_t* content = nullptr;
        lv_obj_t* footer_host = nullptr; // 底栏白底槽；page_label 放其中
        lv_obj_t* page_label = nullptr;
        lv_obj_t* title_label = nullptr;
        lv_obj_t* open_bar = nullptr;
        lv_obj_t* open_pct_lbl = nullptr;

        // 阅读浮层状态机（均不另开 screen）：
        // kSettings → 顶部悬浮设置卡片（盖在正文上）；kToc → 目录整页
        enum class ReadChrome : uint8_t {
            kReading = 0,
            kToc,
            kSettings,
        };
        ReadChrome read_chrome = ReadChrome::kReading;
        bool settings_resume_after_open = false;  // 整本重开失败回退时回到设置卡片
        int toc_list_page = 0;
        int toc_page_size = 8;
        lv_obj_t* status_label = nullptr;
        lv_obj_t* status_bar = nullptr;
        lv_obj_t* status_overlay = nullptr;
        lv_coord_t status_h = 0;
        lv_coord_t viewport_w = 0;
        lv_coord_t viewport_h = 0;

        // 异步打开：worker 解析，经 ScreenLvAsync 回 LVGL（禁止 worker 裸 lv_async_call）
        std::atomic<bool> opening{false};
        std::atomic<uint32_t> open_token{0};
        bool deferred_cleanup = false;  // 打开中离开：结束后再关 session/字库
        reader::BookFormat opening_format = reader::BookFormat::kTxt;

        // TXT 换参：当前页预览后后台建索引；busy 期间翻页禁用，连点只记 again
        std::atomic<bool> layout_busy{false};
        std::atomic<uint32_t> layout_token{0};
        bool layout_again = false;
        bool layout_reload_font = false;
        reader::BookSession::LayoutAnchor layout_anchor{};
        // ±/字体：卡文案 coalesce；正文排版等抬起+停顿；全书页表关设置卡后再建
        lv_timer_t* layout_debounce_timer = nullptr;
        bool layout_debounce_reload_font = false;
        // 章节全书页表后台重建：不挡翻页（与 layout_busy 分离）
        std::atomic<bool> chapter_pages_busy{false};
        std::atomic<uint32_t> chapter_pages_token{0};
        bool chapter_pages_again = false;
        // 正文插图流式解码：不挡翻页；token 作废翻页/离页迟到回调
        std::atomic<bool> page_image_busy{false};
        std::atomic<uint32_t> page_image_token{0};
        std::atomic<bool> page_image_abort{false};
        bool page_image_again = false;
        struct PageImagePending {
            std::string href;
            int max_w = 0;
            int max_h = 0;
            lv_obj_t* slot = nullptr; // 占位容器；回调时校验仍有效
        };
        std::vector<PageImagePending> page_image_pending;
        // 底栏总页旁：编排中 / 完成
        lv_timer_t* layout_hint_timer = nullptr;
        int64_t layout_hint_done_until_us = 0;
        // 底栏含时间/电量时周期刷新（墨水屏约 30s）
        lv_timer_t* footer_clock_timer = nullptr;

        // 阅读时长 checkpoint：仅 LVGL 任务启停；周期 fold+写 .pos
        lv_timer_t* read_time_ckpt_timer = nullptr;
        bool reading_paused_by_standby = false; // 进待机停钟，退出待机可恢复
    } reader;

    struct Settings {
        lv_obj_t* settings_sheet = nullptr;  // 悬浮设置卡片（盖正文）
        // 设置卡控件缓存：字体翻页 / 边距行距 ± 原地刷，避免整卡 clean
        struct SheetFontRow {
            lv_obj_t* row = nullptr;
            lv_obj_t* name = nullptr;
            lv_obj_t* px = nullptr;
            lv_obj_t* check = nullptr; // 多选勾选框
        };
        SheetFontRow sheet_font_rows[kFontListPageSize]{};
        lv_obj_t* sheet_font_title = nullptr; // 「字体」/「已选 N」
        lv_obj_t* sheet_font_page_prev = nullptr;
        lv_obj_t* sheet_font_page_next = nullptr;
        lv_obj_t* sheet_font_page_lab = nullptr;
        lv_obj_t* sheet_font_multi_bar = nullptr; // 多选时插入的操作行：取消 / 全选 / 移除
        lv_obj_t* sheet_margin_value = nullptr;
        lv_obj_t* sheet_margin_dec = nullptr;
        lv_obj_t* sheet_margin_inc = nullptr;
        lv_obj_t* sheet_margin_bound = nullptr;
        lv_obj_t* sheet_gap_value = nullptr;
        lv_obj_t* sheet_gap_dec = nullptr;
        lv_obj_t* sheet_gap_inc = nullptr;
        lv_obj_t* sheet_gap_bound = nullptr;
        lv_obj_t* sheet_fl_bright_value = nullptr; // 前光亮度文案
        std::vector<ReadFontEntry> font_entries;
        int font_list_page = 0;  // 设置卡字体列表页码
        bool font_multi = false;
        std::vector<uint8_t> font_selected; // 与 font_entries 等长
        int64_t font_suppress_click_until_us = 0;
        int font_suppress_click_idx = -1;
        bool font_suppress_next_click = false; // 长按进多选后吃掉松手假 CLICKED
    } settings;

    struct Ttf {
        enum class TtfMode : uint8_t { kNone, kList, kConfirm, kProgress, kResult };
        TtfMode ttf_mode = TtfMode::kNone;
        std::vector<std::string> ttf_files;
        int ttf_page = 0;
        std::string ttf_pick;      // 选中的字体文件名（fonts_ttf 内）
        int ttf_size_px = 25;      // 确认页步进器：20–40，默认 25；只输出一档
        bool ttf_result_ok = false;
        lv_obj_t* sheet_font_import = nullptr; // 字体标题行右侧「导入 TTF」
        lv_obj_t* ttf_sheet = nullptr;
        struct TtfRow {
            lv_obj_t* row = nullptr;
            lv_obj_t* name = nullptr;
        };
        TtfRow ttf_rows[kFontListPageSize]{};
        lv_obj_t* ttf_bar = nullptr;
        lv_obj_t* ttf_pct_lbl = nullptr;
        lv_obj_t* ttf_msg_lbl = nullptr;
        lv_obj_t* ttf_size_value = nullptr;
        lv_obj_t* ttf_size_dec = nullptr;
        lv_obj_t* ttf_size_inc = nullptr;
        lv_timer_t* ttf_poll_timer = nullptr;
    } ttf;

    struct Tap {
        enum class TapZoneUi : uint8_t { kClosed, kGrid, kPick };
        TapZoneUi tap_zone_ui = TapZoneUi::kClosed;
        int tap_zone_pick_cell = -1;
        lv_obj_t* tap_zone_sheet = nullptr;
    } tap;

    using NavRoot = Shelf::NavRoot;
    using ReadChrome = Reader::ReadChrome;
    using TtfMode = Ttf::TtfMode;
    using TapZoneUi = Tap::TapZoneUi;
    using ListCoverPending = Shelf::ListCoverPending;
    using SheetFontRow = Settings::SheetFontRow;
    using PageImagePending = Reader::PageImagePending;
    using TtfRow = Ttf::TtfRow;
};

enum class ReaderGeomMode : uint8_t { kImmersive = 0, kOverlayList };

extern ScreenPaintCoalesce s_reader_paint;
extern ScreenPaintCoalesce s_library_paint;
extern ScreenPaintCoalesce s_shelf_check_paint;
extern ScreenPaintCoalesce s_toc_paint;
extern ScreenPaintCoalesce s_settings_sheet_paint;
extern std::atomic<int> s_reader_page_delta;

/** @brief 书库 UI 单例状态 */
BookUiState& Book_State();
/** @brief 是否为目录/设置等覆盖层 chrome */
bool IsReadOverlayChrome(BookUiState::ReadChrome c);
/** @brief 按偏好刷新正文几何（须在 LVGL 任务） */
void ApplyReaderPageGeometry(ReaderGeomMode mode);
/** @brief 方向切换后重设顶栏/底栏/设置卡尺寸（横屏卡可滚） */
void ApplyReaderChromeSize();

/** @brief 封面外框内可用边长 */
inline int CoverFrameInner(lv_coord_t outer) {
    const lv_coord_t inset = 2 * kCoverFrameBorder;
    return static_cast<int>(outer > inset ? outer - inset : 1);
}
