#pragma once

/** @brief 显示全局快捷控制面板，OTA 升级及待机期间忽略；须在 LVGL 上下文调用 */
void ScreenFrontlightShow();
/** @brief 关闭全局快捷控制面板；须在 LVGL 上下文调用 */
void ScreenFrontlightHide();
/** @brief 查询全局快捷控制面板是否显示；须在 LVGL 上下文调用 */
bool ScreenFrontlightIsVisible();
/** @brief 判断触点是否由竖向调节滑条处理，避免与上滑收起手势冲突；须在 LVGL 上下文调用 */
bool ScreenFrontlightOwnsDrag(int x, int y);
