#pragma once

#include <cstddef>

#include "lvgl.h"

// 经典待机 UI（日期 / 天气 / 待办）。仅由 StandbyScreen 入口在判定无待机壁纸时调用。
// 天气 RAM/NVS 可被首页 Hero 等只读复用（StandbyClassic_CopyWeatherView + listener），勿共享 UI 指针。

struct StandbyClassicWeatherView {
    bool valid = false;
    bool has_temp = false;
    int temp = 0;
    char text[32] = {};
    char icon_code[8] = {};
    char lunar[24] = {};
};

/** @brief 在 root 上构建经典待机控件；as_overlay=true 时隐藏回首页按钮、待办不可点。 */
void StandbyClassic_Build(lv_obj_t* root, bool as_overlay);

/** @brief 启动日期填充 + 天气/清单刷新（须在 Build 之后）。 */
void StandbyClassic_StartRuntime();

/** @brief 停运行时刷新；可重复调用。 */
void StandbyClassic_StopRuntime();

/** @brief 清 UI 指针与 todos（Overlay 删除或页卸载后）。 */
void StandbyClassic_Teardown();

/** @brief 确保当日天气已缓存（可读 NVS；缺则 HTTP）；勿在 LVGL 任务调用。 */
void StandbyClassic_EnsureWeatherCached();

/** @brief 持锁拷贝当日天气 RAM；无有效缓存返回 false。 */
bool StandbyClassic_CopyWeatherView(StandbyClassicWeatherView* out);

/** @brief 天气缓存更新后在 LVGL 任务回调（与 classic ApplyWeatherUi 同一次 async）。 */
using StandbyClassicWeatherUiListener = void (*)();
/** @brief 注册天气 UI 更新监听 */
void StandbyClassic_AddWeatherUiListener(StandbyClassicWeatherUiListener cb);
/** @brief 移除天气 UI 更新监听 */
void StandbyClassic_RemoveWeatherUiListener(StandbyClassicWeatherUiListener cb);

/** @brief 若当日未缓存则调度 core0 worker 拉天气（可重复调用，内部去重）。 */
void StandbyClassic_RequestWeatherEnsure();

/**
 * @brief 农历副行文案：中文用 lunar_zh；英文等为当日星期短名
 * @param lunar_zh 接口农历（可空）
 */
void StandbyClassic_FormatLunarOrWeekday(char* out, size_t out_len, const char* lunar_zh);
