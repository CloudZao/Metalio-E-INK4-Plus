#include <algorithm>
#include <cstdio>
#include <cstring>
#include <esp_timer.h>
#include <lvgl.h>
#include "src/misc/cache/instance/lv_image_cache.h"
#include "ui/common/widgets/storage_error_text.h"
#include "ui/apps/book/shelf/book_bookshelf.h"
#include "assets/lang_config.h"
#include "assets/lang_reader.h"
#include "assets/lang_screen.h"
#include "boards/common/storage/SdCardManager.hpp"
#include "business/reader/book_library.h"
#include "business/reader/reader.h"
#include "ui/common/drawing/adapter/epd_refresh.h"
#include "ui/common/drawing/display_orient.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/drawing/rendering/epd_content_policy.h"
#include "ui/apps/book/book_font_runtime.h"
#include "ui/apps/book/book_geom.h"
#include "ui/apps/book/book_nav.h"
#include "ui/apps/book/book_screen_priv.h"
#include "ui/apps/book/book_text_util.h"
#include "ui/apps/book/book_vk.h"
#include "ui/apps/book/cover/book_cover_cell.h"
#include "ui/apps/book/cover/book_cover_geometry.h"
#include "ui/apps/book/cover/book_cover_workers.h"
#include "ui/apps/book/reader/book_layout_debounce.h"
#include "business/reader/book_reader_prefs.h"
#include "ui/apps/book/reader/book_toc_footer.h"
#include "ui/apps/book/settings/book_font_multi.h"
#include "ui/apps/book/settings/book_settings_sheet.h"
#include "ui/apps/book/settings/book_ttf_panel.h"
#include "ui/apps/book/shelf/book_shelf_ops.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/widgets/ime/zh_CN/screen_pinyin_input.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/runtime/screen_common_nav.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"

static constexpr lv_coord_t kShelfBaseDepth = UiSx(12);
static constexpr lv_coord_t kShelfBaseTop = UiSy(18);

static constexpr lv_coord_t kShelfSearchH = UiSy(52);
static lv_obj_t* s_search_row = nullptr;
static lv_obj_t* s_search_panel = nullptr;
static lv_obj_t* s_search_input = nullptr;
static lv_obj_t* s_search_clear = nullptr;
static lv_obj_t* s_search_keyboard = nullptr;

static void RefreshShelfSearchClear() {
    if (!s_search_clear || !s_search_input) {
        return;
    }
    if (lv_textarea_get_text(s_search_input)[0]) {
        lv_obj_remove_flag(s_search_clear, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_search_clear, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ApplyShelfSearch(const char* query) {
    auto& st = Book_State();
    if (st.shelf.shelf_query == query) {
        return;
    }
    if (st.shelf.shelf_multi) {
        ExitShelfMultiMode(false);
    }
    st.shelf.shelf_selected.clear();
    st.shelf.shelf_query = query;
    st.shelf.list_page = 0;
    if (std::strcmp(lv_textarea_get_text(s_search_input), query) != 0) {
        lv_textarea_set_text(s_search_input, query);
    }
    CancelShelfDirectory();
    ReloadShelfDirListing();
    RenderBookshelfPage();
}

static void CloseShelfSearch(bool apply) {
    if (!s_search_panel) {
        return;
    }
    if (apply) {
        ApplyShelfSearch(lv_textarea_get_text(s_search_input));
    } else {
        lv_textarea_set_text(s_search_input, Book_State().shelf.shelf_query.c_str());
    }
    lv_obj_remove_state(s_search_input, LV_STATE_FOCUSED);
    lv_obj_set_style_opa(s_search_input, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_delete(s_search_panel);
    s_search_panel = nullptr;
}

static void OpenShelfSearch(lv_event_t*) {
    if (s_search_panel || ShelfDeleteBusy()) {
        return;
    }
    HapticPulseOnFingerDown();
    if (Book_State().shelf.shelf_multi) {
        ExitShelfMultiMode(false);
    }
    auto* screen = Book_State().shelf.list_scr;
    lv_obj_update_layout(screen);
    lv_area_t row_area, screen_area;
    lv_obj_get_coords(s_search_row, &row_area);
    lv_obj_get_coords(screen, &screen_area);
    auto* panel = lv_obj_create(screen);
    s_search_panel = panel;
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, lv_pct(100), lv_pct(100));
    Book_DisableScroll(panel);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, [](lv_event_t* e) {
        if (lv_event_get_target(e) == s_search_panel) {
            s_search_panel = nullptr;
            s_search_keyboard = nullptr;
        }
    }, LV_EVENT_DELETE, nullptr);

    // 透明遮罩留出原搜索框，拦截框外点击，避免收起时误触书籍。
    const lv_coord_t row_top = row_area.y1 - screen_area.y1;
    const lv_coord_t row_bottom = row_area.y2 - screen_area.y1 + 1;
    const lv_coord_t keyboard_top = lv_obj_get_height(screen) - ScreenPinyinInputPreferredHeight();
    auto dismiss_area = [&](lv_coord_t x, lv_coord_t y, lv_coord_t width, lv_coord_t height) {
        if (width <= 0 || height <= 0) {
            return;
        }
        auto* outside = lv_obj_create(panel);
        lv_obj_remove_style_all(outside);
        lv_obj_set_pos(outside, x, y);
        lv_obj_set_size(outside, width, height);
        Book_DisableScroll(outside);
        lv_obj_add_flag(outside, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(outside, [](lv_event_t*) { CloseShelfSearch(false); }, LV_EVENT_CLICKED, nullptr);
    };
    const lv_coord_t width = lv_obj_get_width(screen);
    dismiss_area(0, 0, width, row_top);
    dismiss_area(0, row_bottom, width, keyboard_top - row_bottom);
    dismiss_area(0, row_top, row_area.x1 - screen_area.x1, row_bottom - row_top);
    dismiss_area(row_area.x2 - screen_area.x1 + 1, row_top,
                 screen_area.x2 - row_area.x2, row_bottom - row_top);

    auto* keyboard = ScreenPinyinInputCreate(panel, s_search_input, []() { CloseShelfSearch(true); });
    if (!keyboard) {
        lv_obj_delete(panel);
        ScreenShowToast(LangString_COMMON_MEMORY_RETRY);
        return;
    }
    s_search_keyboard = keyboard;
    lv_obj_set_style_opa(s_search_input, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_add_state(s_search_input, LV_STATE_FOCUSED);
}

static lv_coord_t ShelfSideInset() {
    return std::max<lv_coord_t>(kShelfBaseDepth, kShelfCoverCheckSize / 2);
}

static lv_coord_t ShelfColumnGap() {
    return std::max<lv_coord_t>(kShelfColGap, kShelfCoverCheckSize / 2 + UiSx(4));
}

static lv_coord_t ShelfBaseFrontHeight() {
    return Book_ItemFont()->line_height + UiSy(14);
}

// Native L8 image: LVGL owns its clipping, stacking and gray blending.
static std::vector<uint8_t> shelf_base_pixels;
static lv_image_dsc_t shelf_base_image{};

static void PrepareShelfBase(int width) {
    lv_image_cache_drop(&shelf_base_image);
    const int front = ShelfBaseFrontHeight(), height = kShelfBaseTop + front;
    shelf_base_pixels.assign(width * height, 255);
    for (int y = 0; y < height; ++y) {
        const int shift =
            y < kShelfBaseTop ? kShelfBaseDepth * (kShelfBaseTop - 1 - y) / std::max<int>(1, kShelfBaseTop - 1) : 0;
        const int bottom = height - kShelfBaseTop;
        const int end_shift = y >= bottom ? kShelfBaseDepth * (y - bottom) / std::max<int>(1, kShelfBaseTop - 1) : 0;
        const int left = y < kShelfBaseTop ? shift : 0;
        const int face_right = width - kShelfBaseDepth - 1 + shift;
        for (int x = left; x < width - end_shift; ++x) {
            if (BOARD_DISPLAY_GRAY_LEVELS <= 2) {
                static constexpr uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
                const bool side = x > face_right;
                const int density = side ? 12 : (y < kShelfBaseTop ? 8 : 2);
                const bool edge = x == left || x == width - end_shift - 1 || y == 0 || y == kShelfBaseTop ||
                                  y == height - 1 || (y >= kShelfBaseTop && x == face_right);
                shelf_base_pixels[y * width + x] = edge || bayer[y & 3][x & 3] < density ? 0 : 255;
            } else {
                shelf_base_pixels[y * width + x] = x > face_right ? 0 : (y < kShelfBaseTop ? 85 : 119);
            }
        }
    }
    shelf_base_image = {};
    shelf_base_image.header.magic = LV_IMAGE_HEADER_MAGIC;
    shelf_base_image.header.cf = LV_COLOR_FORMAT_L8;
    shelf_base_image.header.w = width;
    shelf_base_image.header.h = height;
    shelf_base_image.header.stride = width;
    shelf_base_image.data_size = shelf_base_pixels.size();
    shelf_base_image.data = shelf_base_pixels.data();
}

static void CreateShelfBase(lv_obj_t* row, int cover_h) {
    auto* base = lv_image_create(row);
    lv_image_set_src(base, &shelf_base_image);
    EpdSetContentRole(base, EpdContentRole::Shelf);
    lv_obj_set_pos(base, 0, cover_h + UiSy(6) - kShelfBaseTop);
    lv_obj_clear_flag(base, LV_OBJ_FLAG_CLICKABLE);
}

bool HandleShelfSearchKey(const char* key) {
    if (!s_search_panel) {
        return false;
    }
    if (std::strcmp(key, "vk_prev") == 0 || std::strcmp(key, "vk_home") == 0) {
        BookLvAsync([](void*) {
            CloseShelfSearch(false);
        });
    }
    return true;
}

lv_obj_t* CreateBookshelfScreen() {
    auto& st = Book_State();
    CancelShelfDirectory();
    ReleaseReaderSessionForLeave();
    st.shelf.shelf_multi = false;
    st.shelf.shelf_selected.clear();
    st.shelf.shelf_suppress_click_until_us = 0;
    st.shelf.shelf_suppress_click_idx = -1;
    st.shelf.multi_bar = nullptr;
    st.shelf.back_root = BookUiState::NavRoot::kShelf;
    CancelListCoverFill();
    st.shelf.recent_covers.clear();
    st.shelf.shelf_covers.clear();
    ScreenPaintCoalesceReset(&s_shelf_check_paint);

    ScreenSetIsHome(false);

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, Book_ListFont(), 0);
    lv_obj_set_style_text_color(scr, UiColorForeground(), 0);
    st.shelf.list_scr = scr;
    lv_obj_add_event_cb(
        scr,
        [](lv_event_t* e) {
            auto& s = Book_State();
            const auto* tgt = static_cast<lv_obj_t*>(lv_event_get_target(e));
            if (s.shelf.list_scr == tgt) {
                CancelShelfDirectory();
                CancelListCoverFill();
                s_search_row = nullptr;
                s_search_input = nullptr;
                s_search_clear = nullptr;
                s_search_keyboard = nullptr;
                s.shelf.list_scr = nullptr;
                s.shelf.list_body = nullptr;
                s.shelf.list_footer = nullptr;
                s.shelf.multi_bar = nullptr;
                s.shelf.shelf_title_lbl = nullptr;
                s.shelf.shelf_view_cover_btn = nullptr;
                s.shelf.shelf_view_list_btn = nullptr;
                s.shelf.shelf_multi = false;
                s.shelf.shelf_selected.clear();
                ScreenPaintCoalesceReset(&s_shelf_check_paint);
                s.shelf.recent_covers.clear();
                s.shelf.shelf_covers.clear();
                s.shelf.shelf_entries.clear();
            }
        },
        LV_EVENT_DELETE, nullptr);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    st.reader.status_label = status.status_label;
    if (status.status_label) {
        // 标题改在内容区「我的书架」行；状态栏留给多选计数
        lv_label_set_text(status.status_label, "");
    }
    if (status.notification_label) {
        lv_obj_add_flag(status.notification_label, LV_OBJ_FLAG_HIDDEN);
    }

    BookReaderPrefsEnsureLoaded();
    st.shelf.shelf_view = BookReaderPrefsShelfView();
    if (st.shelf.shelf_dir.empty() || st.shelf.shelf_dir.rfind(kDefaultBooksDir, 0) != 0) {
        st.shelf.shelf_dir = kDefaultBooksDir;
    }
    st.shelf.shelf_entries.clear();
    st.shelf.shelf_title_lbl = nullptr;
    st.shelf.shelf_view_cover_btn = nullptr;
    st.shelf.shelf_view_list_btn = nullptr;

    const lv_coord_t body_h = LV_VER_RES - status.height - kFooterH;
    if (st.shelf.list_page < 0) {
        st.shelf.list_page = 0;
    }

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, body_h);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(body, kListPad, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(body, kShelfHeaderGap, 0);
    Book_DisableScroll(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    // 标题行：左「我的书架」/当前文件夹，右封面|列表切换
    lv_obj_t* header = lv_obj_create(body);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, Book_ContentWidth(), kShelfHeaderH);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    Book_DisableScroll(header);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title_lbl = lv_label_create(header);
    const lv_font_t* title_font = UiFont(UiFontRole::TitleEmphasis);  // 加粗
    if (title_font == nullptr) {
        title_font = Book_ListFont();
    }
    lv_obj_set_style_text_font(title_lbl, title_font, 0);
    lv_obj_set_style_text_color(title_lbl, UiColorForeground(), 0);
    lv_obj_set_flex_grow(title_lbl, 1);
    lv_obj_clear_flag(title_lbl, LV_OBJ_FLAG_CLICKABLE);
    st.shelf.shelf_title_lbl = title_lbl;

    lv_obj_t* toggle = lv_obj_create(header);
    lv_obj_remove_style_all(toggle);
    lv_obj_set_height(toggle, kShelfViewToggleH);
    lv_obj_set_width(toggle, LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(toggle, 2, 0);
    lv_obj_set_style_border_color(toggle, UiColorForeground(), 0);
    lv_obj_set_style_radius(toggle, 4, 0);
    lv_obj_set_flex_flow(toggle, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toggle, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    Book_DisableScroll(toggle);
    lv_obj_clear_flag(toggle, LV_OBJ_FLAG_CLICKABLE);

    auto make_view_btn = [&](const char* text, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_obj_create(toggle);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, kShelfViewBtnW, kShelfViewBtnH);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(btn);
        HapticAttachClick(btn);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, Book_ItemFont(), 0);
        lv_obj_set_style_text_color(lbl, UiColorForeground(), 0);
        lv_obj_center(lbl);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        return btn;
    };
    st.shelf.shelf_view_cover_btn = make_view_btn(LangString_BOOK_VIEW_COVER, OnShelfViewCoverClicked);
    st.shelf.shelf_view_list_btn = make_view_btn(LangString_BOOK_VIEW_LIST, OnShelfViewListClicked);
    // 中间竖线
    lv_obj_t* sep = lv_obj_create(toggle);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, 1, 20);
    lv_obj_set_style_bg_color(sep, UiColorForeground(), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_clear_flag(sep, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_to_index(sep, 1);

    auto* search_row = lv_obj_create(body);
    lv_obj_remove_style_all(search_row);
    lv_obj_set_size(search_row, Book_ContentWidth(), kShelfSearchH);
    lv_obj_set_flex_flow(search_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(search_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(search_row, 2, 0);
    lv_obj_set_style_border_color(search_row, UiColorForeground(), 0);
    lv_obj_set_style_radius(search_row, UiSx(6), 0);
    lv_obj_set_style_pad_hor(search_row, UiSx(10), 0);
    Book_DisableScroll(search_row);
    lv_obj_add_flag(search_row, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(search_row);
    lv_obj_add_event_cb(search_row, OpenShelfSearch, LV_EVENT_CLICKED, nullptr);
    auto* search_icon = lv_obj_create(search_row);
    lv_obj_remove_style_all(search_icon);
    lv_obj_set_size(search_icon, UiSx(24), UiSx(24));
    Book_DisableScroll(search_icon);
    lv_obj_remove_flag(search_icon, LV_OBJ_FLAG_CLICKABLE);
    auto* lens = lv_obj_create(search_icon);
    lv_obj_remove_style_all(lens);
    lv_obj_set_size(lens, UiSx(16), UiSx(16));
    lv_obj_set_style_radius(lens, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(lens, 2, 0);
    lv_obj_set_style_border_color(lens, UiColorForeground(), 0);
    Book_DisableScroll(lens);
    lv_obj_remove_flag(lens, LV_OBJ_FLAG_CLICKABLE);
    static const lv_point_precise_t handle[] = {{UiSx(14), UiSx(14)}, {UiSx(23), UiSx(23)}};
    auto* stem = lv_line_create(search_icon);
    lv_line_set_points(stem, handle, 2);
    lv_obj_set_style_line_width(stem, 2, 0);
    lv_obj_set_style_line_color(stem, UiColorForeground(), 0);
    lv_obj_remove_flag(stem, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_column(search_row, UiSx(10), 0);
    s_search_row = search_row;
    s_search_input = lv_textarea_create(search_row);
    lv_obj_remove_style_all(s_search_input);
    lv_obj_set_width(s_search_input, 0);
    lv_obj_set_height(s_search_input, kShelfSearchH - UiSy(12));
    lv_obj_set_flex_grow(s_search_input, 1);
    lv_obj_set_style_text_font(s_search_input, Book_ItemFont(), 0);
    lv_textarea_set_one_line(s_search_input, true);
    lv_textarea_set_max_length(s_search_input, 80);
    lv_obj_set_scroll_dir(s_search_input, LV_DIR_HOR);
    lv_textarea_set_placeholder_text(s_search_input, LangString_BOOK_SEARCH_HINT);
    lv_textarea_set_text(s_search_input, st.shelf.shelf_query.c_str());
    lv_obj_set_style_border_color(s_search_input, UiColorForeground(), LV_PART_CURSOR);
    lv_obj_set_style_border_width(s_search_input, 2, LV_PART_CURSOR);
    lv_obj_set_style_border_side(s_search_input, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
    lv_obj_set_style_anim_duration(s_search_input, 0, LV_PART_CURSOR);
    lv_obj_set_style_opa(s_search_input, LV_OPA_TRANSP, LV_PART_CURSOR);
    HapticAttachClick(s_search_input);
    lv_obj_add_event_cb(s_search_input, OpenShelfSearch, LV_EVENT_CLICKED, nullptr);
    s_search_clear = lv_obj_create(search_row);
    lv_obj_remove_style_all(s_search_clear);
    lv_obj_set_size(s_search_clear, UiSx(36), kShelfSearchH - UiSy(8));
    Book_DisableScroll(s_search_clear);
    lv_obj_add_flag(s_search_clear, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(s_search_clear);
    lv_obj_add_event_cb(s_search_clear, [](lv_event_t*) {
        if (ShelfDeleteBusy()) {
            return;
        }
        HapticPulseOnFingerDown();
        if (s_search_keyboard) {
            ScreenPinyinInputClear(s_search_keyboard);
        } else {
            lv_textarea_set_text(s_search_input, "");
            ApplyShelfSearch("");
        }
    }, LV_EVENT_CLICKED, nullptr);
    auto* clear_ring = lv_obj_create(s_search_clear);
    lv_obj_remove_style_all(clear_ring);
    lv_obj_set_size(clear_ring, UiSx(24), UiSx(24));
    lv_obj_set_style_radius(clear_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(clear_ring, 1, 0);
    lv_obj_set_style_border_color(clear_ring, UiColorForeground(), 0);
    Book_DisableScroll(clear_ring);
    lv_obj_remove_flag(clear_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(clear_ring);
    static const lv_point_precise_t cross_a[] = {{UiSx(7), UiSx(7)}, {UiSx(17), UiSx(17)}};
    static const lv_point_precise_t cross_b[] = {{UiSx(17), UiSx(7)}, {UiSx(7), UiSx(17)}};
    for (const auto* points : {cross_a, cross_b}) {
        auto* line = lv_line_create(clear_ring);
        lv_line_set_points(line, points, 2);
        lv_obj_set_style_line_width(line, 2, 0);
        lv_obj_set_style_line_color(line, UiColorForeground(), 0);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_add_event_cb(s_search_input, [](lv_event_t*) { RefreshShelfSearchClear(); },
                        LV_EVENT_VALUE_CHANGED, nullptr);
    RefreshShelfSearchClear();

    lv_obj_t* list_body = lv_obj_create(body);
    lv_obj_remove_style_all(list_body);
    lv_obj_set_width(list_body, Book_ContentWidth());
    lv_obj_set_flex_grow(list_body, 1);
    lv_obj_set_flex_flow(list_body, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(list_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list_body, kShelfRowGap, 0);
    lv_obj_set_style_pad_column(list_body, kShelfColGap, 0);
    Book_DisableScroll(list_body);
    st.shelf.list_body = list_body;

    // 底栏槽：页码与多选共用，不改书籍区高度
    lv_obj_t* foot = lv_obj_create(scr);
    lv_obj_remove_style_all(foot);
    lv_obj_set_size(foot, LV_HOR_RES, kFooterH);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(foot, LV_OPA_TRANSP, 0);
    Book_DisableScroll(foot);
    lv_obj_clear_flag(foot, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* page_lbl = lv_label_create(foot);
    lv_obj_set_width(page_lbl, LV_HOR_RES - 16);
    lv_obj_set_style_text_align(page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(page_lbl, Book_ItemFont(), 0);
    lv_obj_set_style_text_color(page_lbl, UiColorForeground(), 0);
    lv_label_set_text(page_lbl, "1/1");
    lv_obj_align(page_lbl, LV_ALIGN_CENTER, 0, 0);
    Book_DisableScroll(page_lbl);
    st.shelf.list_footer = page_lbl;

    st.shelf.multi_bar = lv_obj_create(foot);
    lv_obj_remove_style_all(st.shelf.multi_bar);
    lv_obj_set_width(st.shelf.multi_bar, lv_pct(100));
    lv_obj_set_height(st.shelf.multi_bar, kShelfMultiBtnH);
    lv_obj_set_style_bg_opa(st.shelf.multi_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(st.shelf.multi_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st.shelf.multi_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(st.shelf.multi_bar, 10, 0);
    lv_obj_align(st.shelf.multi_bar, LV_ALIGN_CENTER, 0, 0);
    Book_DisableScroll(st.shelf.multi_bar);
    lv_obj_clear_flag(st.shelf.multi_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(st.shelf.multi_bar, LV_OBJ_FLAG_HIDDEN);

    auto make_multi_action = [](lv_obj_t* parent, const char* text, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_obj_create(parent);
        lv_obj_remove_style_all(btn);
        lv_obj_set_height(btn, kShelfMultiBtnH);
        lv_obj_set_width(btn, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_hor(btn, 4, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        Book_DisableScroll(btn);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(btn);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* text_wrap = lv_obj_create(btn);
        lv_obj_remove_style_all(text_wrap);
        lv_obj_set_size(text_wrap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_border_side(text_wrap, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(text_wrap, kShelfUnderlineH, 0);
        lv_obj_set_style_border_color(text_wrap, UiColorForeground(), 0);
        lv_obj_set_style_pad_bottom(text_wrap, 2, 0);
        lv_obj_set_style_pad_hor(text_wrap, kShelfUnderlinePadHor, 0);
        lv_obj_set_style_bg_opa(text_wrap, LV_OPA_TRANSP, 0);
        Book_DisableScroll(text_wrap);
        lv_obj_clear_flag(text_wrap, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* lbl = lv_label_create(text_wrap);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, Book_ItemFont(), 0);
        lv_obj_set_style_text_color(lbl, UiColorForeground(), 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        return btn;
    };
    auto make_multi_dot = [](lv_obj_t* parent) {
        lv_obj_t* dot = lv_obj_create(parent);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, kShelfMultiDotSize, kShelfMultiDotSize);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, UiColorForeground(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        Book_DisableScroll(dot);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        return dot;
    };
    make_multi_action(st.shelf.multi_bar, LangString_COMMON_CANCEL, OnShelfMultiCancel);
    make_multi_dot(st.shelf.multi_bar);
    make_multi_action(st.shelf.multi_bar, LangString_COMMON_SELECT_ALL, OnShelfMultiSelectAll);
    make_multi_dot(st.shelf.multi_bar);
    make_multi_action(st.shelf.multi_bar, LangString_COMMON_REMOVE, OnShelfMultiRemove);

    // 搜索框以下可用高度：封面 3×3 / 列表行数共用
    st.shelf.shelf_content_h = body_h - kListPad * 2 - kShelfHeaderH - kShelfSearchH - kShelfHeaderGap * 2;
    if (st.shelf.shelf_content_h < 72) {
        st.shelf.shelf_content_h = 72;
    }

    if (st.shelf.shelf_view == kBookReaderShelfViewList) {
        ApplyShelfListPageSize();
        ReloadShelfDirListing();
    } else {
        st.shelf.list_page_size = kShelfCols * kShelfRows;
    }
    RefreshShelfViewToggleUi();
    UpdateShelfTitleLabel();

    const lv_coord_t cell_w = ShelfBookWidth();
    const lv_coord_t title_h = ShelfBaseFrontHeight() + UiSy(6) + kShelfCoverCheckSize / 2;
    const lv_coord_t usable = st.shelf.shelf_content_h;
    const lv_coord_t cell_budget = (usable - kShelfRowGap * (kShelfRows - 1)) / kShelfRows;
    lv_coord_t cover_h = cell_budget - title_h;
    const lv_coord_t cover_max = cell_w * 7 / 5;
    if (cover_h > cover_max) {
        cover_h = cover_max;
    }
    if (cover_h < 72) {
        cover_h = 72;
    }
    st.shelf.shelf_cover_h = cover_h;

    if (!SdCardManager::GetInstance().IsMounted()) {
        Book_ShowMessage(list_body, LangString_BOOK_NO_SD, Book_ListFont());
        BookBindPageLifecycle(scr);
        VkKey_AttachScreen(scr, kScreenBookshelf, BookAiLongPressDesc(CreateBookshelfScreen));
        return scr;
    }

    // 当前目录按页加载，不依赖递归书库快照。
    ReloadShelfDirListing();
    RenderBookshelfPage();

    BookBindPageLifecycle(scr);
    VkKey_AttachScreen(scr, kScreenBookshelf, BookAiLongPressDesc(CreateBookshelfScreen));
    return scr;
}

void RenderBookshelfPage() {
    auto& st = Book_State();
    if (st.shelf.list_body == nullptr) {
        return;
    }
    CancelListCoverFill();

    if (!EnsureShelfDirectoryPage() || st.shelf.directory_failed) {
        if (st.shelf.directory_loading && !st.shelf.shelf_entries.empty() &&
            esp_timer_get_time() - st.shelf.directory_requested_us < 180000) {
            return;
        }
        lv_obj_clean(st.shelf.list_body);
        st.shelf.shelf_covers.clear();
        Book_ShowMessage(st.shelf.list_body,
                         st.shelf.directory_failed ? StorageErrorText(st.shelf.directory_error) : LangString_BOOK_LOADING,
                         Book_ListFont());
        return;
    }
    if (st.shelf.shelf_view == kBookReaderShelfViewList) {
        RenderBookshelfListPage();
        return;
    }

    lv_obj_set_style_layout(st.shelf.list_body, LV_LAYOUT_FLEX, 0);
    lv_obj_set_flex_flow(st.shelf.list_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(st.shelf.list_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(st.shelf.list_body, kShelfRowGap, 0);
    lv_obj_set_style_pad_column(st.shelf.list_body, kShelfColGap, 0);

    Book_ClampListPage();
    lv_obj_clean(st.shelf.list_body);
    st.shelf.shelf_covers.clear();
    Book_DisableScroll(st.shelf.list_body);

    if (st.shelf.books.empty()) {
        lv_obj_set_flex_flow(st.shelf.list_body, LV_FLEX_FLOW_COLUMN);
        Book_ShowMessage(st.shelf.list_body,
                         st.shelf.shelf_query.empty() ? LangString_BOOK_FOLDER_EMPTY : LangString_BOOK_SEARCH_EMPTY,
                         Book_ListFont());
        if (st.shelf.list_footer != nullptr) {
            lv_label_set_text(st.shelf.list_footer, "1/1");
        }
        RefreshShelfFooterMode();
        return;
    }

    const lv_coord_t content_w = Book_ContentWidth();
    const lv_coord_t cell_w = ShelfBookWidth();
    const lv_coord_t cover_h = st.shelf.shelf_cover_h > 0 ? st.shelf.shelf_cover_h : (cell_w * 4 / 3);

    const int start = 0;
    const int end = std::min(start + st.shelf.list_page_size, static_cast<int>(st.shelf.books.size()));
    const lv_coord_t front_h = ShelfBaseFrontHeight();
    PrepareShelfBase(content_w);
    for (int row_start = start; row_start < end; row_start += kShelfCols) {
        auto* row = lv_obj_create(st.shelf.list_body);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, content_w, cover_h + front_h + UiSy(6) + kShelfCoverCheckSize / 2);
        Book_DisableScroll(row);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        CreateShelfBase(row, cover_h + kShelfCoverCheckSize / 2);
        for (int col = 0; col < kShelfCols && row_start + col < end; ++col) {
            const int i = row_start + col;
            auto* cell = CreateBookCoverCell(row, st.shelf.books[static_cast<size_t>(i)], i, cell_w, cover_h,
                                             st.shelf.shelf_covers, true, 0, true,
                                             st.shelf.shelf_entries[i].kind == BookDirEntry::Kind::kFolder);
            lv_obj_set_style_layout(cell, LV_LAYOUT_NONE, 0);
            // The whole circle must lie inside the cell's native clip/hit bounds.
            const lv_coord_t circle_inset = kShelfCoverCheckSize / 2;
            lv_obj_set_size(cell, cell_w + circle_inset, cover_h + front_h + UiSy(6) + circle_inset);
            lv_obj_set_pos(cell, ShelfSideInset() + col * (cell_w + ShelfColumnGap()), 0);
            auto* cover = lv_obj_get_child(cell, 0);
            lv_obj_set_pos(cover, 0, circle_inset);
            auto* title = lv_obj_get_child(cell, 1);
            lv_obj_set_height(title, Book_ItemFont()->line_height);
            lv_obj_set_pos(title, 0, circle_inset + cover_h + UiSy(6) + (front_h - Book_ItemFont()->line_height) / 2);
        }
    }

    if (st.shelf.list_footer != nullptr) {
        char foot[48];
        std::snprintf(foot, sizeof(foot), "%d/%d", st.shelf.list_page + 1, Book_ListPageCount());
        lv_label_set_text(st.shelf.list_footer, foot);
    }
    RefreshShelfFooterMode();
    EpdAfterNextSubmit(ScheduleListCoverFill);
}

lv_coord_t ShelfBookWidth() {
    return (Book_ContentWidth() - 2 * ShelfSideInset() - ShelfColumnGap() * (kShelfCols - 1)) / kShelfCols;
}
