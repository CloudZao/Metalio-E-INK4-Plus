#pragma once
#include <cstddef>

struct WeatherPhenom {
    const char* code;      // 现象码
    const char* zh;        // 中文描述
    const char* en_day;    // 英文日间描述
    const char* en_night;  // 英文夜间描述
};

struct WeatherSnap {
    bool valid = false;      // 缓存是否有效
    bool has_temp = false;   // 是否包含温度
    int temp = 0;            // 温度，摄氏度
    char text[32] = {};      // 原始天气描述
    char icon_code[8] = {};  // 天气图标现象码
    char lunar[24] = {};     // 农历文本
};

/** @brief 当日天气缓存已就绪 */
bool WeatherCacheIsToday();

/** @brief 拷贝天气快照，不暴露共享缓冲 */
WeatherSnap WeatherCacheSnapshot();

/** @brief 按日恢复或更新天气缓存，后台调用，内部串行去重 */
void WeatherEnsureCached();

/** @brief 天气现象码是否已知 */
bool WeatherCodeKnown(const char* code);

/** @brief 将天气文本匹配到现象码，无匹配返回空 */
const char* WeatherMapTextToCode(const char* text);

/** @brief 查找现象码的中英文显示信息 */
const WeatherPhenom* WeatherFindPhenom(const char* code);

/** @brief 将数值现象码格式化为约定文本 */
void WeatherFormatCode(char* out, size_t capacity, int value);

/** @brief 按当前系统时区格式化当日日期 */
bool WeatherFormatToday(char* out, size_t capacity);

/**
 * @brief 请求后台刷新天气；当日已有缓存或已有请求时复用当前结果。
 * @param completed 实际执行刷新后在后台调用的完成通知，不传递UI对象。
 * @return 请求已接收返回true，任务创建失败返回false。
 */
bool WeatherRequestEnsure(void (*completed)());
