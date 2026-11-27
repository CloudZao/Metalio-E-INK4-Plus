#pragma once

#include <cstddef>
#include "business/weather/weather_cache.h"

struct WeatherUiView {
    bool valid = false; // 天气快照是否有效
    bool has_temp = false; // 是否包含温度
    int temp = 0; // 摄氏温度
    char text[32] = {}; // 当前语言的天气文案
    char icon_code[8] = {}; // 天气图标编码
    char lunar[24] = {}; // 农历日期
};

/** @brief 持锁拷贝当日天气 RAM；无有效缓存返回 false。 */
bool WeatherView_Copy(WeatherUiView* out);

using WeatherUiListener = void (*)(); // 在界面任务通知当前订阅者，不持有页面对象

/** @brief 在界面任务注册天气更新监听，最多两个订阅者。 */
void WeatherView_AddListener(WeatherUiListener cb);

/** @brief 在界面任务移除监听，须在页面销毁前调用。 */
void WeatherView_RemoveListener(WeatherUiListener cb);

/** @brief 若当日未缓存则调度 core0 worker 拉天气（可重复调用，内部去重）。 */
void WeatherView_RequestEnsure();

/**
 * @brief 农历副行文案：中文用 lunar_zh；英文等为当日星期短名
 * @param lunar_zh 接口农历（可空）
 */
void WeatherView_FormatLunarOrWeekday(char* out, size_t out_len, const char* lunar_zh);

/** @brief 返回天气快照的本地化文案，不修改业务缓存。 */
const char* WeatherView_LocalizedPhenomText(const WeatherSnap& snap);

/** @brief 将最新天气状态交付UI监听者，不发起网络请求。 */
void WeatherView_Refresh();

/** @brief 返回本地化星期短名，索引0为周日，越界返回空串。 */
const char* WeatherView_WeekdayShortName(int day);
