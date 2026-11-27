#pragma once

#include "lvgl.h"

// 分组设置首页及独立功能页；各功能沿用对应模块的状态和生命周期。
class SettingsScreen {
public:
    /** @brief 创建分组设置首页 */
    static lv_obj_t* Create();

    /** @brief 创建设置→网络子页入口 */
    static lv_obj_t* CreateNetwork();

    /** @brief 创建设置→翻页器子页入口 */
    static lv_obj_t* CreatePager();

    /** @brief 创建设置→蓝牙子页入口 */
    static lv_obj_t* CreateBluetooth();

    /** @brief 创建设置→测试子页入口 */
    static lv_obj_t* CreateTest();

    /** @brief 创建设置页并打开按键与手势。 */
    static lv_obj_t* CreateKeys();

    /** @brief 切换语言或明暗模式后重建当前设置页 */
    static void ReloadAppearance();

    /** @brief 更新当前设置子页的导航标题。 */
    static void SetTitle(const char* title);

    /** @brief 从 Wi-Fi 页打开连接方式与 SIM 卡设置，返回时恢复 Wi-Fi 页。 */
    static void OpenNetworkMode();

    /** @brief 切换 Wi-Fi 列表与详情的导航标题和首页控件，空标题表示返回列表。 */
    static void SetWifiDetail(const char* title);
};

/** @brief 进入待机时暂停设置页实时刷新（前光等） */
void SettingsScreen_OnEnterStandby();

/** @brief 退出待机后恢复设置页实时刷新（前光等） */
void SettingsScreen_OnResumeFromStandby();
