#pragma once

#include <cstdint>
#include <lvgl.h>
#include "ui/common/drawing/ui_scale.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"

constexpr const char* TAG = "ScreenCommon";
// 源值对齐 397 screen_common；经 UiSx/UiSy 同比例到 470
constexpr lv_coord_t kStatusPadV = UiSy(8);
constexpr lv_coord_t kStatusPadH = UiSx(8);
constexpr lv_coord_t kStatusTextNudgeY = UiSy(4);  // 顶栏文字垂直微调
constexpr lv_coord_t kStatusIconSlotW = UiSx(56);
constexpr lv_coord_t kStatusIconGap = UiSx(4);
constexpr lv_coord_t kStatusNetHitMinH = UiSy(44);
constexpr lv_coord_t kBatteryPctNudgeY = UiSy(2);
constexpr lv_coord_t kLowBatteryBottom = UiSy(8);
constexpr lv_coord_t kLowBatteryRadius = UiSx(4);
