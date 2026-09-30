#pragma once

#include "home_screen/home_hero.h"
#include "standby_screen/standby_classic.h"
#include "ui_scale.h"

#include <cstddef>
#include <cstdint>

#include <lvgl.h>

LV_FONT_DECLARE(font_misans_regular_160_2);

enum class HeroKind : uint8_t { Classic = 0, Slash = 1 };

constexpr const char* TAG = "HomeHero";
// 源值对齐 397 home_hero；经 UiSx/UiSy 同比例到 470
constexpr lv_coord_t kHeroPadTop = UiSy(20);
constexpr lv_coord_t kTimeDateGap = UiSy(24);
constexpr lv_coord_t kSidePad = UiSx(28);
constexpr lv_coord_t kMetaGap = UiSx(16);
constexpr lv_coord_t kMetaSplitW = UiSx(2);
constexpr lv_coord_t kWeatherIcon = UiSx(70);
constexpr lv_coord_t kWeatherGap = UiSx(12);
constexpr lv_coord_t kCalendarIconW = UiSx(62);
constexpr lv_coord_t kCalendarIconH = UiSy(59);
constexpr lv_coord_t kCalendarGap = UiSx(12);
constexpr uint32_t kClockTickMs = 1000;

constexpr lv_coord_t kSlashSidePad = UiSx(28);
constexpr lv_coord_t kSlashTimeDateGap = UiSx(20);
constexpr lv_coord_t kSlashDateRuleW = UiSx(64);
constexpr lv_coord_t kSlashDateRuleH = UiSy(3);
constexpr lv_coord_t kSlashDateGap = UiSy(10);
constexpr lv_coord_t kSlashTimeW = UiSx(212);
constexpr lv_coord_t kSlashSlashGapH = UiSy(44);
constexpr lv_coord_t kSlashLineHalfW = UiSx(72);
constexpr lv_coord_t kSlashLinePadY = UiSy(6);
constexpr lv_coord_t kSlashWeatherIcon = UiSx(48); // 397 斜切天气小于经典 70
constexpr lv_coord_t kSlashWeatherGap = UiSx(8);
constexpr lv_coord_t kSlashMinuteNudgeX = UiSx(20);

struct HomeHeroUiState {
    HeroKind kind = HeroKind::Classic;
    lv_obj_t* root = nullptr;
    lv_obj_t* time_lbl = nullptr;
    lv_obj_t* hour_lbl = nullptr;
    lv_obj_t* minute_lbl = nullptr;
    lv_obj_t* date_lbl = nullptr;
    lv_obj_t* lunar_lbl = nullptr;
    lv_obj_t* weekday_lbl = nullptr;
    lv_obj_t* meta_split = nullptr;
    lv_obj_t* weather_row = nullptr;
    lv_obj_t* weather_icon = nullptr;
    lv_obj_t* weather_text = nullptr;
    lv_obj_t* weather_temp = nullptr;
    lv_timer_t* timer = nullptr;
    char last_time[8] = {};
    char last_date[48] = {};
    char last_lunar[24] = {};
    char last_weekday[24] = {};
};

/** @brief 取 HomeHero UI 状态 */
HomeHeroUiState& HomeHero_State();
extern bool HomeHero_alive; // Hero 是否存活

/** @brief 斜切大号时间字体 */
const lv_font_t* HomeHero_SlashTimeFont();
/** @brief 天气文案字体 */
const lv_font_t* HomeHero_WeatherFont();
/** @brief 星期显示名 */
const char* HomeHero_WeekdayName(int wday);
/** @brief 格式化经典时钟日期串 */
void HomeHero_FormatClassicClock(char* time_buf, size_t time_len, char* date_buf, size_t date_len);
/** @brief 格式化斜切时钟各字段 */
void HomeHero_FormatSlashClock(char* hour_buf, size_t hour_len, char* min_buf, size_t min_len,
                               char* date_buf, size_t date_len, char* wday_buf, size_t wday_len);
/** @brief 刷新时钟显示 */
void HomeHero_RefreshClock(bool force);
/** @brief 用天气快照填充控件 */
void HomeHero_FillWeatherWidgets(const StandbyClassicWeatherView& snap);
/** @brief 应用天气 UI */
void HomeHero_ApplyWeatherUi();
/** @brief 天气变更监听回调 */
void HomeHero_OnWeatherListener();
/** @brief 时钟定时器回调 */
void HomeHero_OnClockTimer(lv_timer_t* timer);
/** @brief 斜切间隙自定义绘制 */
void HomeHero_OnSlashGapDraw(lv_event_t* e);
/** @brief 斜切时间标签样式 */
void HomeHero_StyleSlashTimeLabel(lv_obj_t* lbl);
/** @brief 构建经典 Hero UI */
void HomeHero_BuildClassicUi(lv_obj_t* parent, lv_coord_t y_offset);
/** @brief 构建斜切 Hero UI */
void HomeHero_BuildSlashUi(lv_obj_t* parent, lv_coord_t y_offset);
