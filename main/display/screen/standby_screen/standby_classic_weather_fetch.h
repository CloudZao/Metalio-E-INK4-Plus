#pragma once

#include "standby_classic_priv.h"

/** @brief 通知天气 UI 监听者 */
void StandbyClassic_NotifyWeatherListeners();
/** @brief 解析天气 JSON */
bool StandbyClassic_ParseWeatherJson(const std::string& body, WeatherSnap& out, std::string& err);
/** @brief HTTP 拉取天气 body */
bool StandbyClassic_HttpGetWeather(std::string& body_out, std::string& err_out);
/** @brief 持锁确保当日天气已缓存 */
void StandbyClassic_EnsureWeatherCachedLocked();
/** @brief 调度天气拉取 worker */
void StandbyClassic_ScheduleWeatherEnsure();
/** @brief 投递天气 UI 刷新到 LVGL */
void StandbyClassic_PostWeatherUi();
