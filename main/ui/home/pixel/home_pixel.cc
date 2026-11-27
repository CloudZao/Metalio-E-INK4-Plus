#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <esp_log.h>
#include <esp_timer.h>
#include "ui/common/widgets/weather_view.h"
#include "ui/home/pixel/home_pixel.h"
#include "assets/lang_config.h"
#include "assets/lang_pixel.h"
#include "boards/common/storage/sd_access.h"
#include "business/checklist/checklist_cache.h"
#include "business/reader/book_home_snapshot.h"
#include "business/reader/book_progress_cache.h"
#include "business/reader/book_session.h"
#include "sysmgmt/tasks/task_executor.h"
#include "ui/home/pixel/home_pixel_font.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/apps/book/book_screen.h"
#include "ui/apps/book/book_text_util.h"
#include "ui/home/home_screen_ui.h"
#include "ui/common/widgets/screen_text.h"
#include "ui/apps/standby/standby_screen.h"
#include "ui/common/drawing/ui_theme.h"

static constexpr int kDesignW = 834;          // 参考图宽度
static constexpr const char* kPixelTag = "HomePixel";
static constexpr int64_t kProgressRetryUs = 5 * 1000000LL; // SD 不可用或任务投递失败后的重试间隔
static constexpr int kDesignH = 1357;         // 参考图高度
static constexpr uint32_t kRefreshMs = 1000;  // 检查时钟及内存缓存的周期

struct HomePixelPixelUi {
    AppEntry task_app{};           // 固定卡片自持入口，不引用可能扩容的应用列表
    lv_obj_t* root = nullptr;      // 当前主题根节点
    lv_obj_t* time = nullptr;      // 时分
    lv_obj_t* date = nullptr;      // 日期与星期
    lv_obj_t* recent = nullptr;    // 最近阅读书名
    std::string recent_filename;   // 已排版的完整文件名，避免每秒重复测宽
    lv_obj_t* percent = nullptr;   // 阅读百分比
    lv_obj_t* progress = nullptr;  // 阅读进度条
    lv_obj_t* counts[3] = {};      // 今日、完成、未完成计数
    lv_timer_t* timer = nullptr;   // 首页刷新定时器
    lv_coord_t width = 0;          // 可绘宽度
    lv_coord_t height = 0;         // 状态栏下方可绘高度
};

static HomePixelPixelUi s;
static uint32_t s_app_child_start = 0;  // 应用卡片、阴影和标题签的起始子节点

lv_coord_t X(int value) {
    return (value * s.width + kDesignW / 2) / kDesignW;
}

lv_coord_t Y(int value) {
    return (value * s.height + kDesignH / 2) / kDesignH;
}

static const lv_font_t* TextFont() {
    return UiFont(UiFontRole::Caption);
}

static int FitNumber(lv_obj_t* label, int width, int height) {
    lv_point_t extent;
    lv_text_get_size(&extent, lv_label_get_text(label), lv_obj_get_style_text_font(label, LV_PART_MAIN), 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int scale = std::max(
        1, std::min({256, width * 256 / std::max<int>(1, extent.x), height * 256 / std::max<int>(1, extent.y)}));
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(label, extent.x, extent.y);
    lv_obj_set_style_transform_pivot_x(label, 0, 0);
    lv_obj_set_style_transform_pivot_y(label, 0, 0);
    lv_obj_set_style_transform_scale_x(label, scale, 0);
    lv_obj_set_style_transform_scale_y(label, scale, 0);
    return (extent.x * scale + 255) / 256;
}

static void LayoutNumber(lv_obj_t* label) {
    if (label == s.time) {
        const int width = FitNumber(label, X(700), Y(250));
        lv_obj_set_pos(label, (s.width - width) / 2, Y(45));
    } else if (label == s.percent) {
        const int width = FitNumber(label, X(130), Y(60));
        lv_obj_set_pos(label, X(700) - X(25) - width, Y(42));
    } else {
        for (auto* count : s.counts) {
            if (label != count) {
                continue;
            }
            const int width = FitNumber(label, X(210), Y(48));
            lv_obj_set_pos(label, (X(210) - width) / 2, TextFont()->line_height + 4);
            break;
        }
    }
}

static void SetText(lv_obj_t* label, const char* text) {
    if (std::strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
        LayoutNumber(label);
    }
}

static lv_obj_t* Rect(lv_obj_t* parent, int x, int y, int w, int h, bool black) {
    auto* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, black ? UiColorForeground() : UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t* Text(lv_obj_t* parent, const char* text, int x, int y, const lv_font_t* font, int width = 0) {
    auto* label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, UiColorForeground(), 0);
    if (width > 0) {
        lv_obj_set_width(label, width);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    }
    lv_label_set_text(label, text);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

static void BindApp(lv_obj_t* card, const AppEntry& app) {
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(card);
    lv_obj_set_user_data(card, const_cast<AppEntry*>(&app));
    lv_obj_add_event_cb(card, Home_OnAppClicked, LV_EVENT_CLICKED, const_cast<AppEntry*>(&app));
}

static lv_obj_t* Card(int x, int y, int width, int height, const AppEntry& app) {
    const int offset = std::max<int>(1, X(8));
    Rect(s.root, x + offset, y + offset, width, height, true);
    auto* card = Rect(s.root, x, y, width, height, false);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, UiColorForeground(), 0);
    BindApp(card, app);
    return card;
}

static void BlackTab(lv_obj_t* card, const char* text, bool centered) {
    // 标题签跨越卡片边框，使用页面同级节点，避免父卡片裁剪。
    auto* tab = Text(s.root, text, 0, 0, TextFont());
    lv_obj_set_style_text_color(tab, UiColorBackground(), 0);
    lv_obj_set_style_bg_color(tab, UiColorForeground(), 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(tab, X(centered ? 8 : 9), 0);
    lv_obj_set_style_pad_ver(tab, Y(3), 0);
    if (centered) {
        const int padding = 2 * X(8);
        const int card_width = lv_obj_get_style_width(card, LV_PART_MAIN);
        lv_point_t extent;
        lv_text_get_size(&extent, text, TextFont(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        const int width = std::clamp<int>(extent.x + padding, card_width - X(16), card_width + X(24));
        ScreenFitLabel(tab, width - padding, TextFont()->line_height, TextFont());
        lv_obj_set_width(tab, width);
        lv_obj_align_to(tab, card, LV_ALIGN_BOTTOM_MID, 0, Y(19));
    } else {
        lv_obj_align_to(tab, card, LV_ALIGN_TOP_LEFT, 0, -Y(20));
    }
    const auto* app = static_cast<const AppEntry*>(lv_obj_get_user_data(card));
    if (app != nullptr) {
        BindApp(tab, *app);
    }
}

static std::atomic<bool> s_progress_loading{false};
static std::atomic<int64_t> s_progress_retry_at{0};

static void RefreshRecent() {
    const std::string path = BookHomeSnapshotLoadLatestAbsPath();
    BookSession::ProgressPeek peek;
    const bool has_progress = !path.empty() && BookProgressCacheTryGet(path.c_str(), peek);
    if (!path.empty() && !has_progress && esp_timer_get_time() >= s_progress_retry_at.load() &&
        !s_progress_loading.exchange(true)) {
        if (!TaskExecutor::Schedule(
                [path]() {
                    struct ProgressLoadGuard {
                        ~ProgressLoadGuard() {
                            s_progress_retry_at.store(esp_timer_get_time() + kProgressRetryUs);
                            s_progress_loading.store(false);
                        }
                    } guard;
                    SdAccessTransaction transaction;
                    if (transaction && !SdAccessIsQuiescing()) {
                        BookProgressCacheUpsert(path.c_str(), BookSession::PeekProgress(path.c_str()));
                    }
                },
                TaskDomain::Storage)) {
            s_progress_retry_at.store(esp_timer_get_time() + kProgressRetryUs);
            s_progress_loading.store(false);
            ESP_LOGE(kPixelTag, "recent progress task scheduling failed");
        }
    }
    const std::string filename = path.empty() ? LangString_BOOK_NO_RECENT : path.substr(path.find_last_of('/') + 1);
    if (filename != s.recent_filename) {
        Book_SetFilenameMiddle(s.recent, filename.c_str());
        s.recent_filename = filename;
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

static void Refresh(bool force) {
    if (s.root == nullptr) {
        return;
    }
    if (!force && (StandbyPresentation::IsActive() || lv_obj_get_screen(s.root) != lv_screen_active())) {
        return;
    }
    const time_t now = std::time(nullptr);
    tm local{};
    const bool valid = localtime_r(&now, &local) != nullptr && local.tm_year >= 120;
    char time_text[8] = "00:00";
    char day[16] = {};
    char date_text[160] = "--/--/--";
    if (valid) {
        std::snprintf(time_text, sizeof(time_text), "%02d:%02d", local.tm_hour, local.tm_min);
        std::strftime(day, sizeof(day), "%Y-%m-%d", &local);
        std::snprintf(date_text, sizeof(date_text), "%04d/%02d/%02d  |  %s  |  %s", local.tm_year + 1900,
                      local.tm_mon + 1, local.tm_mday, WeatherView_WeekdayShortName(local.tm_wday), LangString_HOME_PIXEL_DAY_NOTE);
    }
    SetText(s.time, time_text);
    SetText(s.date, date_text);
    size_t values[3] = {};
    const bool has_tasks = checklist_cache_counts(valid ? day : nullptr, &values[0], &values[1], &values[2]);
    for (int i = 0; i < 3; ++i) {
        char count[12] = "--";
        if (has_tasks && (i != 0 || valid)) {
            std::snprintf(count, sizeof(count), "%02u", static_cast<unsigned>(values[i]));
        }
        SetText(s.counts[i], count);
    }
    RefreshRecent();
}

void HomePixel_Build(lv_obj_t* screen, lv_coord_t status_height) {
    HomePixel_Stop();
    s.width = LV_HOR_RES;
    s.height = LV_VER_RES - status_height;
    s.root = Rect(screen, 0, status_height, s.width, s.height, false);
    lv_obj_add_event_cb(
        s.root,
        [](lv_event_t* e) {
            if (lv_event_get_target(e) == s.root) {
                HomePixel_Stop();
            }
        },
        LV_EVENT_DELETE, nullptr);

    s.time = Text(s.root, "00:00", 0, Y(45), &kPixelClockFont, s.width);
    lv_obj_set_style_text_align(s.time, LV_TEXT_ALIGN_CENTER, 0);
    LayoutNumber(s.time);
    s.date = Text(s.root, "--/--/--", X(32), Y(322), TextFont(), s.width - X(64));
    lv_obj_set_style_text_align(s.date, LV_TEXT_ALIGN_CENTER, 0);

    const int panel_width = X(700);
    static const AppEntry recent_app = {nullptr, BookScreen::CreateLatest, {}, {}, {}, UiAppId::Book};
    auto* reading = Card(X(64), Y(454), panel_width, Y(144), recent_app);
    BlackTab(reading, LangString_BOOK_RECENT, false);
    s.recent = Text(reading, LangString_BOOK_NO_RECENT, X(30), Y(48), TextFont(), X(490));
    lv_obj_set_height(s.recent, lv_font_get_line_height(TextFont()));
    s.percent = Text(reading, "--", panel_width - X(155), Y(42), &kPixelCounterFont, X(130));
    lv_obj_set_style_text_align(s.percent, LV_TEXT_ALIGN_RIGHT, 0);
    LayoutNumber(s.percent);
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
    lv_obj_set_style_bg_color(s.progress, UiColorForeground(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s.progress, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_clear_flag(s.progress, LV_OBJ_FLAG_CLICKABLE);
    lv_bar_set_range(s.progress, 0, 1000);
    lv_bar_set_value(s.progress, 0, LV_ANIM_OFF);

    s.task_app = kSlashApps[3];
    auto* tasks = Card(X(64), Y(658), panel_width, Y(184), s.task_app);
    BlackTab(tasks, LangString_TASK_TITLE_TODO, false);
    const char* captions[] = {LangString_HOME_PIXEL_TODAY, LangString_TASK_TAB_DONE, LangString_HOME_PIXEL_PENDING};
    for (int i = 0; i < 3; ++i) {
        auto* group = lv_obj_create(tasks);
        lv_obj_remove_style_all(group);
        lv_obj_set_pos(group, X(30) + i * X(225), Y(40));
        lv_obj_set_size(group, X(210), TextFont()->line_height + 4 + Y(48));
        lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(group, LV_OBJ_FLAG_CLICKABLE);
        auto* caption = Text(group, captions[i], 0, 0, TextFont(), X(210));
        ScreenFitLabel(caption, X(210), TextFont()->line_height, TextFont());
        s.counts[i] = Text(group, "--", 0, 0, &kPixelCounterFont);
        lv_obj_set_style_border_side(s.counts[i], LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(s.counts[i], 1, 0);
        lv_obj_set_style_border_color(s.counts[i], UiColorForeground(), 0);
        LayoutNumber(s.counts[i]);
    }

    s_app_child_start = lv_obj_get_child_count(s.root);
    HomePixel_FillAppsPage();
    Refresh(true);
    s.timer = lv_timer_create([](lv_timer_t*) { Refresh(false); }, kRefreshMs, nullptr);
}

void HomePixel_Stop() {
    if (s.timer != nullptr) {
        lv_timer_delete(s.timer);
    }
    s = HomePixelPixelUi{};
    s_app_child_start = 0;
}

void HomePixel_FillAppsPage() {
    if (!s.root) {
        return;
    }
    while (lv_obj_get_child_count(s.root) > s_app_child_start) {
        lv_obj_delete(lv_obj_get_child(s.root, s_app_child_start));
    }
    const char* icons[] = {"A:ic_px_book.spng", "A:ic_px_wallpaper.spng", "A:ic_px_ai.spng",
                           "A:ic_px_task.spng", "A:ic_px_cloud.spng",     "A:ic_px_setting.spng"};
    const int grid_left = X(64), grid_width = X(700);
    const int caption_height = TextFont()->line_height;
    const int row_gap = std::max<int>(Y(28), caption_height);
    const int bottom_reserve = caption_height + Y(20);
    const int edge = std::max<int>(1, std::min<int>(X(184), (s.height - Y(882) - bottom_reserve - row_gap) / 2));
    const int row_step = edge + row_gap;
    const int start = Home_page * kAppsPerPage;
    for (int i = 0; i < kAppsPerPage && start + i < kSlashAppCount; ++i) {
        const int app = start + i;
        const bool is_buddy = kSlashApps[app].builtin_id == UiAppId::WorkBuddy;
        auto* card = Card(grid_left + (grid_width - edge) * (i % 3) / 2, Y(882) + (i / 3) * row_step, edge, edge,
                          kSlashApps[app]);
        auto* icon = lv_image_create(card);
        const int icon_edge = std::max<int>(1, std::min<int>(X(104), edge - X(16)));
        Home_SetAppIcon(icon, kSlashApps[app], icon_edge, app < 6 ? icons[app] : nullptr);
        lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_set_style_image_recolor(icon, UiColorForeground(), 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        lv_image_set_antialias(icon, false);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, -Y(6));
        const char* name = is_buddy   ? "buddy"
                           : app == 0 ? LangString_HOME_PIXEL_READ
                           : app == 3 ? LangString_HOME_PIXEL_NOTES
                                      : Home_AppLabel(kSlashApps[app]);
        BlackTab(card, name, true);
    }
}
