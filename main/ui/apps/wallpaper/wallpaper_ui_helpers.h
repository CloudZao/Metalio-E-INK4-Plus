#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <lvgl.h>
#include "infra/storage/storage_error.h"

enum class ImageDecodeError : uint8_t;

/** @brief 壁纸页 UI 字体 */
const lv_font_t* Wallpaper_UiFont();

/** @brief 叠放子行须写死高度：创建时 get_height(parent) 常为 0，会导致整行不可见 */
lv_coord_t Wallpaper_ContentWidth();



/** @brief 禁用滚动 */
void Wallpaper_DisableScroll(lv_obj_t* obj);

/** @brief 关闭当前壁纸弹窗，返回预览。 */
void Wallpaper_CloseDialog();

/** @brief 格式化壁纸加载失败提示，优先呈现存储原因。 */
void Wallpaper_FormatDecodeFailure(char* out, size_t capacity, ImageDecodeError error,
                                  const StorageError& storage_error = {});

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
