#pragma once

#include <lvgl.h>

/** @brief 构建设置→关于页（系统信息列表与固件更新检测） */
void SettingsAboutTab_Build(lv_obj_t* page);

/** @brief 重置关于页 */
void SettingsAboutTab_Reset();

/** @brief 构建设置→震动页选项 */
void SettingsHapticTab_Build(lv_obj_t* page);

/** @brief 重置震动页 UI 指针 */
void SettingsHapticTab_Reset();

/** @brief 构建设置→语言页（运行时中/英热切换） */
void SettingsLanguageTab_Build(lv_obj_t* page);

/** @brief 重置语言页 UI 指针 */
void SettingsLanguageTab_Reset();

/** @brief 构建设置→日志页（开关 + 上传入口） */
void SettingsLogTab_Build(lv_obj_t* page);

/** @brief 重置日志页；若在上传则中断并拆全屏进度 */
void SettingsLogTab_Reset();

/** @brief 构建系统设置中的翻页器页面 */
void SettingsPagerTab_Build(lv_obj_t* page);

/** @brief 释放翻页器页面的 UI 引用，保留已建立的 BLE 连接 */
void SettingsPagerTab_Reset();

/** @brief 构建设置→电源页（空闲降频、保网、待机、自动关机及充电电流） */
void SettingsPowerTab_Build(lv_obj_t* page);

/** @brief 重置电源页 UI 指针 */
void SettingsPowerTab_Reset();

/** @brief 构建设置→主题页选项 */
void SettingsThemeTab_Build(lv_obj_t* page);

/** @brief 重置主题页 UI 指针 */
void SettingsThemeTab_Reset();
