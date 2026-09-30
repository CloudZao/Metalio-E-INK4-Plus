#pragma once

#include "screen_common.h"
#include "ui_scale.h"
#include "vk_key_handler.h"

#include "lv_adapter_display.h"

#include <cstdint>

constexpr const char* TAG = "ScreenCommon";
constexpr const char* kHomeScreen = "home"; // 首页屏名
constexpr int kMaxBack = 8; // 返回栈深度
// 源值对齐 397 screen_common；经 UiSx/UiSy 同比例到 470
constexpr lv_coord_t kStatusPadV = UiSy(8);
constexpr lv_coord_t kStatusPadH = UiSx(8);
constexpr lv_coord_t kStatusTextNudgeY = UiSy(4); // 顶栏文字垂直微调
constexpr lv_coord_t kStatusNetHitW = UiSx(56);
constexpr lv_coord_t kStatusNetHitMinH = UiSy(44);
constexpr lv_coord_t kStatusTextReserveW = UiSx(200);
constexpr lv_coord_t kBatteryPctMarginL = UiSx(6);
constexpr lv_coord_t kBatteryPctNudgeY = UiSy(2);
constexpr lv_coord_t kBatteryIconMarginL = UiSx(-2);
constexpr lv_coord_t kLowBatteryBottom = UiSy(8);
constexpr lv_coord_t kLowBatteryRadius = UiSx(4);
constexpr uint64_t kPaintCoalescePendingFallbackUs = 120000; // 合并重绘兜底超时

extern bool ScreenCommon_on_home; // 是否在首页
extern ScreenFactory ScreenCommon_back_stack[kMaxBack]; // 返回栈
extern int ScreenCommon_back_depth; // 返回栈深度

/** @brief 清空返回栈 */
void ScreenCommon_ClearBackStack();
/** @brief 压入返回工厂 */
void ScreenCommon_PushBackFactory(ScreenFactory factory);
