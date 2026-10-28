#include "home_pixel.h"
#include "book_screen/book_screen.h"
#include "workbuddy_screen/workbuddy_screen.h"
#include "home_pixel_font.h"
#include "home_icon_source.h"
#include "home_screen/home_screen_ui.h"
#include "assets/lang_config.h"
#include "assets/lang_pixel.h"
#include "checklist_cache.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "reader/book_home_snapshot.h"
#include "reader/book_progress_cache.h"
#include "standby_screen/standby_screen.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace {
constexpr int kDesignW = 834; // 参考图宽度
constexpr int kDesignH = 1357; // 参考图高度
constexpr uint32_t kRefreshMs = 1000; // 检查时钟及内存缓存的周期

struct PixelUi {
    lv_obj_t* root = nullptr; // 当前主题根节点
    lv_obj_t* time = nullptr; // 时分
    lv_obj_t* date = nullptr; // 日期与星期
    lv_obj_t* recent = nullptr; // 最近阅读书名
    lv_obj_t* percent = nullptr; // 阅读百分比
    lv_obj_t* progress = nullptr; // 阅读进度条
    lv_obj_t* counts[3] = {}; // 今日、完成、未完成计数
    lv_timer_t* timer = nullptr; // 首页刷新定时器
    lv_coord_t width = 0; // 可绘宽度
    lv_coord_t height = 0; // 状态栏下方可绘高度
};
PixelUi s;
uint32_t s_app_child_start = 0; // 应用卡片、阴影和标题签的起始子节点

lv_coord_t X(int value) { return (value * s.width + kDesignW / 2) / kDesignW; }
lv_coord_t Y(int value) { return (value * s.height + kDesignH / 2) / kDesignH; }

const lv_font_t* TextFont() {
    const auto* font = fontpack_lv_font_get(25, 2);
    if (font == nullptr) font = fontpack_lv_font_ui();
    return font != nullptr ? font : LV_FONT_DEFAULT;
}

void SetText(lv_obj_t* label, const char* text) {
    if (std::strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

lv_obj_t* Rect(lv_obj_t* parent, int x, int y, int w, int h, bool black) {
    auto* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, black ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

lv_obj_t* Text(lv_obj_t* parent, const char* text, int x, int y,
               const lv_font_t* font, int width = 0) {
    auto* label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    if (width > 0) {
        lv_obj_set_width(label, width);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    }
    lv_label_set_text(label, text);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

void BindApp(lv_obj_t* card, const AppEntry& app) {
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(card);
    lv_obj_set_user_data(card, const_cast<AppEntry*>(&app));
    lv_obj_add_event_cb(card, Home_OnAppClicked, LV_EVENT_CLICKED, const_cast<AppEntry*>(&app));
}

lv_obj_t* Card(int x, int y, int width, int height, const AppEntry& app) {
    const int offset = std::max<int>(1, X(8));
    Rect(s.root, x + offset, y + offset, width, height, true);
    auto* card = Rect(s.root, x, y, width, height, false);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    BindApp(card, app);
    return card;
}

void BlackTab(lv_obj_t* card, const char* text, bool centered) {
    // 标题签跨越卡片边框，使用页面同级节点，避免父卡片裁剪。
    auto* tab = Text(s.root, text, 0, 0, TextFont());
    lv_obj_set_style_text_color(tab, lv_color_white(), 0);
    lv_obj_set_style_bg_color(tab, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(tab, X(centered ? 14 : 9), 0);
    lv_obj_set_style_pad_ver(tab, Y(3), 0);
    if (centered) {
        lv_obj_set_style_max_width(tab, X(148), 0);
        lv_label_set_long_mode(tab, LV_LABEL_LONG_DOT);
        lv_obj_align_to(tab, card, LV_ALIGN_BOTTOM_MID, 0, Y(19));
    } else {
        lv_obj_align_to(tab, card, LV_ALIGN_TOP_LEFT, 0, -Y(20));
    }
    const auto* app = static_cast<const AppEntry*>(lv_obj_get_user_data(card));
    if (app != nullptr) BindApp(tab, *app);
}

const char* Weekday(int day) {
    const char* names[] = {Lang::Strings::HOME_WDAY_SUN, Lang::Strings::HOME_WDAY_MON,
        Lang::Strings::HOME_WDAY_TUE, Lang::Strings::HOME_WDAY_WED,
        Lang::Strings::HOME_WDAY_THU, Lang::Strings::HOME_WDAY_FRI, Lang::Strings::HOME_WDAY_SAT};
    return day >= 0 && day < 7 ? names[day] : "";
}

void RefreshRecent() {
    const std::string path = reader::book_home_snapshot::LoadLatestAbsPath();
    reader::BookSession::ProgressPeek peek;
    const bool has_progress = !path.empty() && reader::book_progress_cache::TryGet(path.c_str(), peek);
    if (path.empty() || (reader::book_progress_cache::IsReady() && !has_progress)) {
        SetText(s.recent, Lang::Strings::BOOK_NO_RECENT);
    } else {
        std::string title = path.substr(path.find_last_of('/') + 1);
        const auto dot = title.find_last_of('.');
        if (dot != std::string::npos && dot > 0) title.resize(dot);
        SetText(s.recent, title.c_str());
    }
    const int progress = has_progress ? std::clamp(peek.progress_x10, 0, 1000) : 0;
    char percent[12] = "--";
    if (has_progress && peek.progress_x10 >= 0) {
        std::snprintf(percent, sizeof(percent), "%d%%", progress / 10);
    }
    SetText(s.percent, percent);
    if (lv_bar_get_value(s.progress) != progress) {
        lv_bar_set_value(s.progress, progress, LV_ANIM_OFF);
    }
}

void Refresh(bool force) {
    if (s.root == nullptr) return;
    if (!force && (StandbyScreen::IsActive() || lv_obj_get_screen(s.root) != lv_screen_active())) return;
    const time_t now = std::time(nullptr);
    tm local{};
    const bool valid = localtime_r(&now, &local) != nullptr && local.tm_year >= 120;
    char time_text[8] = "00:00";
    char day[16] = {};
    char date_text[160] = "--/--/--";
    if (valid) {
        std::snprintf(time_text, sizeof(time_text), "%02d:%02d", local.tm_hour, local.tm_min);
        std::strftime(day, sizeof(day), "%Y-%m-%d", &local);
        std::snprintf(date_text, sizeof(date_text), "%04d/%02d/%02d  |  %s  |  %s",
                      local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                      Weekday(local.tm_wday), Lang::Strings::HOME_PIXEL_DAY_NOTE);
    }
    SetText(s.time, time_text);
    SetText(s.date, date_text);
    size_t total = 0, done = 0;
    const bool has_tasks = valid && checklist_cache_day_counts(day, &total, &done);
    const size_t values[] = {total, done, total - done};
    for (int i = 0; i < 3; ++i) {
        char count[12] = "--";
        if (has_tasks) std::snprintf(count, sizeof(count), "%02u", static_cast<unsigned>(values[i]));
        SetText(s.counts[i], count);
    }
    RefreshRecent();
}
} // namespace

void HomePixel_Stop() {
    if (s.timer != nullptr) lv_timer_delete(s.timer);
    s = PixelUi{};
    s_app_child_start = 0;
}

void HomePixel_Build(lv_obj_t* screen, lv_coord_t status_height) {
    HomePixel_Stop();
    s.width = LV_HOR_RES;
    s.height = LV_VER_RES - status_height;
    s.root = Rect(screen, 0, status_height, s.width, s.height, false);
    lv_obj_add_event_cb(s.root, [](lv_event_t* e) {
        if (lv_event_get_target(e) == s.root) HomePixel_Stop();
    }, LV_EVENT_DELETE, nullptr);

    s.time = Text(s.root, "00:00", 0, Y(45), &kPixelClockFont, s.width);
    lv_obj_set_style_text_align(s.time, LV_TEXT_ALIGN_CENTER, 0);
    s.date = Text(s.root, "--/--/--", X(32), Y(322), TextFont(), s.width - X(64));
    lv_obj_set_style_text_align(s.date, LV_TEXT_ALIGN_CENTER, 0);

    const int panel_width = X(700);
    static const AppEntry recent_app = {nullptr, Home_AppNameBook, BookScreen::CreateLatest};
    auto* reading = Card(X(64), Y(454), panel_width, Y(144), recent_app);
    BlackTab(reading, Lang::Strings::BOOK_RECENT, false);
    s.recent = Text(reading, Lang::Strings::BOOK_NO_RECENT, X(30), Y(48), TextFont(), X(490));
    s.percent = Text(reading, "--", panel_width - X(155), Y(42), &kPixelCounterFont, X(130));
    lv_obj_set_style_text_align(s.percent, LV_TEXT_ALIGN_RIGHT, 0);
    const int progress_width = panel_width - X(60);
    const int progress_height = std::max<int>(3, Y(3));
    Rect(reading, X(30), Y(110) + progress_height / 2, progress_width, 1, true);
    s.progress = lv_bar_create(reading);
    lv_obj_remove_style_all(s.progress);
    lv_obj_set_pos(s.progress, X(30), Y(110));
    lv_obj_set_size(s.progress, progress_width, progress_height);
    lv_obj_set_style_radius(s.progress, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s.progress, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s.progress, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s.progress, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s.progress, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_clear_flag(s.progress, LV_OBJ_FLAG_CLICKABLE);
    lv_bar_set_range(s.progress, 0, 1000);
    lv_bar_set_value(s.progress, 0, LV_ANIM_OFF);

    auto* tasks = Card(X(64), Y(658), panel_width, Y(144), kSlashApps[3]);
    BlackTab(tasks, Lang::Strings::TASK_TITLE_TODO, false);
    const char* captions[] = {Lang::Strings::HOME_PIXEL_TODAY, Lang::Strings::TASK_TAB_DONE,
                              Lang::Strings::HOME_PIXEL_PENDING};
    for (int i = 0; i < 3; ++i) {
        auto* group = lv_obj_create(tasks);
        lv_obj_remove_style_all(group);
        lv_obj_set_pos(group, X(30) + i * X(225), Y(48));
        lv_obj_set_size(group, X(210), Y(65));
        lv_obj_set_flex_flow(group, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(group, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(group, X(10), 0);
        lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(group, LV_OBJ_FLAG_CLICKABLE);
        Text(group, captions[i], 0, 0, TextFont());
        s.counts[i] = Text(group, "--", 0, 0, &kPixelCounterFont);
        lv_obj_set_style_border_side(s.counts[i], LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(s.counts[i], 1, 0);
        lv_obj_set_style_border_color(s.counts[i], lv_color_black(), 0);
    }

    s_app_child_start = lv_obj_get_child_count(s.root);
    HomePixel_FillAppsPage();
    Refresh(true);
    s.timer = lv_timer_create([](lv_timer_t*) { Refresh(false); }, kRefreshMs, nullptr);
}

void HomePixel_FillAppsPage() {
    if (!s.root) return;
    while (lv_obj_get_child_count(s.root) > s_app_child_start) {
        lv_obj_delete(lv_obj_get_child(s.root, s_app_child_start));
    }
    const char* icons[] = {"A:ic_px_book.spng", "A:ic_px_wallpaper.spng", "A:ic_px_ai.spng",
                           "A:ic_px_task.spng", "A:ic_px_cloud.spng", "A:ic_px_setting.spng"};
    const int columns[] = {64, 334, 605};
    const int edge = X(160);
    const int row_step = edge + (Y(251) - edge) * 2 / 3;
    const int start = Home_page * kAppsPerPage;
    for (int i = 0; i < kAppsPerPage && start + i < kSlashAppCount; ++i) {
        const int app = start + i;
        const bool is_buddy = kSlashApps[app].create == WorkBuddyScreen::Create;
        auto* card = Card(X(columns[i % 3]), Y(882) + (i / 3) * row_step, edge, edge, kSlashApps[app]);
        auto* icon = lv_image_create(card);
        lv_image_set_src(icon, HomeIconSource(app < 6 ? icons[app] : kSlashApps[app].icon_path));
        lv_obj_set_style_image_recolor(icon, lv_color_black(), 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        const int icon_edge = X(100);
        lv_obj_set_size(icon, icon_edge, icon_edge);
        lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
        lv_image_set_antialias(icon, false);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, -Y(6));
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        const char* name = is_buddy ? "buddy" : app == 0 ? Lang::Strings::HOME_PIXEL_READ :
                           app == 3 ? Lang::Strings::HOME_PIXEL_NOTES : kSlashApps[app].name();
        BlackTab(card, name, true);
    }
}
