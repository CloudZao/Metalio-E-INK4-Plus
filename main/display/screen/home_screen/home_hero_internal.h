#pragma once

#include "home_hero_priv.h"

/** @brief 取 HomeHero UI 状态 */
HomeHeroUiState& HomeHero_State();
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
/** @brief 应用天气 UI（图标与文案） */
void HomeHero_ApplyWeatherUi();
/** @brief 天气变更监听回调 */
void HomeHero_OnWeatherListener();
/** @brief 时钟定时器回调 */
void HomeHero_OnClockTimer(lv_timer_t* timer);
