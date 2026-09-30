#pragma once

#include <string>

#include <lvgl.h>

/** @brief 壁纸页 UI 字体 */
const lv_font_t* Wallpaper_UiFont();
/** @brief 叠放子行须写死高度：创建时 get_height(parent) 常为 0，会导致整行不可见 */
lv_coord_t Wallpaper_ContentWidth();
/** @brief 按像素尽量排成两行，避免长文件名导致首行留白。 */
std::string Wallpaper_LayoutTitleTwoLines(const char* text, const lv_font_t* font, lv_coord_t max_w);
/** @brief 禁用滚动 */
void Wallpaper_DisableScroll(lv_obj_t* obj);
/** @brief 忽略大小写后缀判断 */
bool EndsWithIgnoreCase(const char* name, const char* ext);
/** @brief 是否为可列入列表的图片 */
bool IsListableFile(const char* name);
/** @brief 动作按钮样式 */
void StyleActionBtn(lv_obj_t* btn, lv_obj_t* lbl, bool filled, bool enabled);
/** @brief 刷新预览元信息 */
void UpdatePreviewMeta();
/** @brief 刷新启用按钮态 */
void UpdateEnableButtonUi();
/** @brief 刷新删除按钮态 */
void UpdateDeleteButtonUi();
/** @brief 创建角标底座；外框与内框留白后用于绘制图标。 */
lv_obj_t* MakeTipBadge(lv_obj_t* parent);
/** @brief 关机壁纸的角标：电源键。 */
void DrawPowerTipIcon(lv_obj_t* parent);
/** @brief 待机壁纸的角标：月牙。 */
void DrawMoonTipIcon(lv_obj_t* parent);
/** @brief 壁纸页居中提示 */
void Wallpaper_ShowMessage(lv_obj_t* host, const char* text);
