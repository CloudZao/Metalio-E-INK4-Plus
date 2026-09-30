#pragma once

#include "standby_classic_priv.h"

/** @brief 天气文案字体 */
const lv_font_t* StandbyClassic_WeatherFont();
/** @brief 天气文本映射为现象码 */
const char* StandbyClassic_MapTextToCode(const char* text);
/** @brief 星期短名 */
const char* StandbyClassic_WeekdayShortName(int wday);
/** @brief 格式化今日日期串 */
bool StandbyClassic_FormatToday(char* out, size_t out_len);
/** @brief 按现象码查找天气现象表项 */
const WeatherPhenom* StandbyClassic_FindPhenomByCode(const char* code);
/** @brief 取经典待机 UI 状态 */
StandbyClassicUiState& StandbyClassic_State();
/** @brief 待办区字体 */
const lv_font_t* StandbyClassic_TodoFont();
/** @brief 拷贝 RAM 中天气快照 */
WeatherSnap StandbyClassic_WeatherRamCopy();
/** @brief 格式化现象码显示 */
void StandbyClassic_FormatPhenomCode(char* out, size_t out_len, int n);
/** @brief 本地化天气现象文案 */
const char* StandbyClassic_LocalizedPhenomText(const WeatherSnap& snap);
/** @brief RAM 天气是否为今日 */
bool StandbyClassic_WeatherRamIsToday();
/** @brief 从 NVS 加载天气 */
bool StandbyClassic_LoadWeatherFromNvs(WeatherSnap& out, char* day_out, size_t day_len);
/** @brief 写入 RAM 天气快照 */
void StandbyClassic_WeatherRamStore(const WeatherSnap& snap, const char* day);
/** @brief 天气写入 NVS */
void StandbyClassic_SaveWeatherToNvs(const WeatherSnap& snap, const char* day);
/** @brief 现象码是否已知 */
bool StandbyClassic_CodeKnown(const char* code);
