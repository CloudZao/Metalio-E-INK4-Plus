#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include "business/weather/weather_cache.h"
#include "ui/common/widgets/weather_view.h"
#include "ui/apps/standby/standby_classic.h"
#include "ui/common/drawing/ui_scale.h"

constexpr const char* TAG = "StandbyClassic";
// 源值对齐 397 standby_classic；经 UiSx/UiSy 同比例到 470
constexpr lv_coord_t kSidePad = UiSx(28);
constexpr lv_coord_t kMetaGap = UiSx(16);
constexpr lv_coord_t kMetaSplitW = UiSx(2);
constexpr lv_coord_t kWeatherIcon = UiSx(70);
constexpr lv_coord_t kWeatherGap = UiSx(12);
constexpr lv_coord_t kCalendarIconW = UiSx(62);
constexpr lv_coord_t kCalendarIconH = UiSy(59);
constexpr lv_coord_t kCalendarGap = UiSx(12);
constexpr lv_coord_t kHomeBtnH = UiSy(60);
constexpr lv_coord_t kTodoRowPad = UiSx(10);
constexpr lv_coord_t kTodoGap = UiSy(8);
constexpr lv_coord_t kTodoLineGap = UiSy(6);
constexpr lv_coord_t kSectionGap = UiSy(48);
constexpr lv_coord_t kHeroSectionGap = UiSy(20);
constexpr lv_coord_t kHeroInnerGap = UiSy(4);
constexpr lv_coord_t kTopPad = UiSy(12);
constexpr lv_coord_t kBubbleW = UiSx(316);
constexpr lv_coord_t kBubbleH = UiSy(94);
constexpr lv_coord_t kMenuIcon = UiSx(30);
constexpr lv_coord_t kBorderW = UiSx(2);
constexpr int kMaxTodos = 3;
constexpr int kHttpTimeoutMs = 15000;
constexpr int kWorkerStack = 10 * 1024;
constexpr const char* kWeatherNs = "weather";
constexpr int kMaxWeatherListeners = 2;

struct TodoItem {
    char title[96] = {};
    char status[16] = {};
    char plan_date[16] = {};
    char plan_time[16] = {};
};

struct StandbyClassicUiState {
    lv_obj_t* date_lbl = nullptr;
    lv_obj_t* lunar_lbl = nullptr;
    lv_obj_t* meta_split = nullptr;
    lv_obj_t* weather_row = nullptr;
    lv_obj_t* weather_icon = nullptr;
    lv_obj_t* weather_text = nullptr;
    lv_obj_t* weather_temp = nullptr;
    lv_obj_t* todo_host = nullptr;
    lv_obj_t* home_btn = nullptr;
};

/** @brief 取经典待机 UI 状态 */
StandbyClassicUiState& StandbyClassic_State();
extern bool StandbyClassic_alive;                                                               // 经典待机是否存活
extern bool StandbyClassic_as_overlay;                                                          // 是否 Overlay 模式
extern std::vector<TodoItem> StandbyClassic_todos;                                              // 待办缓存

/** @brief 天气文案字体 */
const lv_font_t* StandbyClassic_WeatherFont();

/** @brief 待办区字体 */
const lv_font_t* StandbyClassic_TodoFont();

/** @brief 将天气应用到 UI */
void StandbyClassic_ApplyWeatherUi();

/** @brief 从 checklist_cache 填充待办 */
void StandbyClassic_ApplyTodosFromCache();
