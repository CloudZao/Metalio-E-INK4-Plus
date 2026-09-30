#pragma once

#include "lvgl.h"

/** @brief 页面级虚拟键：HOME/PREV/NEXT 与 BOOT 分发及生命周期 */
typedef enum {
    VK_SCREEN_LIFECYCLE_LOAD = 0, // 页面加载：压入返回栈
    VK_SCREEN_LIFECYCLE_UNLOAD,   // 页面卸载：弹出返回栈
} vk_screen_lifecycle_t;

using ScreenFactory = lv_obj_t* (*)(); // 返回栈页面工厂；须为无捕获静态函数
using VkKeyHandler = bool (*)(const char* key_name); // true=页面已消费，不再走默认策略
using BootKeyAction = bool (*)(); // BOOT 回调；true=页面已处理

struct VkKeyScreenDesc {
    ScreenFactory factory = nullptr;                 // 返回栈工厂；如 ResumeDetail 回书详情
    VkKeyHandler on_key = nullptr;                   // 短按虚拟键
    BootKeyAction on_boot_click = nullptr;           // BOOT 单击
    BootKeyAction on_boot_long_press = nullptr;      // BOOT 长按
    BootKeyAction on_boot_press_down = nullptr;      // BOOT 按下
    BootKeyAction on_boot_press_up = nullptr;        // BOOT 抬起
    VkKeyHandler on_key_long_press = nullptr;        // 虚拟键长按
    VkKeyHandler on_key_press_up = nullptr;          // 虚拟键抬起
    BootKeyAction on_boot_double_click = nullptr;    // BOOT 双击
};

/**
 * @brief 页面生命周期：压栈、出栈
 * @param name 页面名
 * @param event LOAD / UNLOAD
 */
void VkKey_OnScreenLifecycle(const char* name, vk_screen_lifecycle_t event);

/** @brief 当前前台页面名；无则返回空串或 nullptr（实现约定） */
const char* VkKey_ActiveScreen();

/** @brief 查询页面工厂 */
ScreenFactory VkKey_GetScreenFactory(const char* name);

/** @brief 注册页面工厂 */
void VkKey_SetScreenFactory(const char* name, ScreenFactory factory);

/** @brief 查询 BOOT 单击回调 */
BootKeyAction VkKey_GetBootClick(const char* name);
/** @brief 查询 BOOT 长按回调 */
BootKeyAction VkKey_GetBootLongPress(const char* name);
/** @brief 查询 BOOT 按下回调 */
BootKeyAction VkKey_GetBootPressDown(const char* name);
/** @brief 查询 BOOT 抬起回调 */
BootKeyAction VkKey_GetBootPressUp(const char* name);
/** @brief 查询 BOOT 双击回调 */
BootKeyAction VkKey_GetBootDoubleClick(const char* name);

/** @brief 挂接页面名到虚拟键表（仅生命周期，无自定义回调） */
void VkKey_AttachScreen(lv_obj_t* scr, const char* name);
/**
 * @brief 挂接页面并注册按键/BOOT 回调
 * @note factory=ResumeDetail 时：进百问后可回到当前书详情
 */
void VkKey_AttachScreen(lv_obj_t* scr, const char* name, const VkKeyScreenDesc& desc);

/** @brief 分发虚拟键短按 */
void VkKey_Dispatch(const char* key_name);
/** @brief 分发虚拟键长按；true=已消费 */
bool VkKey_OnLongPress(const char* key_name);
/** @brief 分发虚拟键抬起；true=已消费 */
bool VkKey_OnPressUp(const char* key_name);
