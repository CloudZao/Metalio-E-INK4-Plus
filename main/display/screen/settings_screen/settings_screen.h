#pragma once

#include "lvgl.h"

// 设置主壳：网络 / 主题 / 语言 / 震动 / 前光 / 对话 / 存储 / 蓝牙 / 测试 / 关于
class SettingsScreen {
public:
    /** @brief 创建设置主页面（默认网络 Tab） */
    static lv_obj_t* Create();
    /** @brief 创建设置→网络子页入口 */
    static lv_obj_t* CreateNetwork();
    /** @brief 创建设置→测试子页入口 */
    static lv_obj_t* CreateTest();
    /** @brief 切换语言后重建设置页文案与布局 */
    static void ReloadAfterLanguageChange();
};

/** @brief 进入待机时暂停设置页实时刷新（前光等） */
void SettingsScreen_OnEnterStandby();
/** @brief 退出待机后恢复设置页实时刷新（前光等） */
void SettingsScreen_OnResumeFromStandby();

