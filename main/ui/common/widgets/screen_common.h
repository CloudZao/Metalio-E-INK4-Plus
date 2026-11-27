#pragma once

#include <cstddef>
#include <functional>
#include "lvgl.h"

struct EpdStatusBar {
    lv_obj_t* bar = nullptr;
    lv_obj_t* overlay = nullptr;
    lv_obj_t* network_label = nullptr;
    lv_obj_t* mute_label = nullptr;
    lv_obj_t* battery_pct_label = nullptr;
    lv_obj_t* battery_label = nullptr;
    lv_obj_t* status_label = nullptr;
    lv_obj_t* notification_label = nullptr;
    lv_obj_t* low_battery_popup = nullptr;
    lv_coord_t height = 0;
};

/** @brief 获取通用顶部状态栏高度 */
lv_coord_t ScreenStatusBarHeight();

/** @brief 创建通用状态栏 */
EpdStatusBar ScreenCreateStatusBar(lv_obj_t* scr);

/**
 * @brief 创建统一的顶部居中提示，由调用方管理内容和生命周期
 * @note 须在 LVGL 线程或持有 UI 锁时调用
 */
lv_obj_t* ScreenCreateToast(lv_obj_t* parent, const char* text);

/**
 * @brief 在顶部状态栏下方显示黑底白字圆角提示，3 秒后自动消失
 * @param text 提示文本，调用时复制；空指针或空字符串不显示
 * @note 须在 LVGL 线程或持有 UI 锁时调用；连续调用替换提示并重新计时
 */
void ScreenShowToast(const char* text);

/** @brief 应用网点背景样式；尺寸与位置由调用方布局管理，全局显示入口负责边界裁剪 */
void ScreenApplyDotBackdrop(lv_obj_t* obj);

/** @brief 应用公共弹窗卡片样式并限制可视高度；溢出时纵向滚动，padding 为卡片内边距 */
void ScreenStyleDialogCard(lv_obj_t* card, lv_coord_t padding);

struct ScreenDialogAction {
    const char* text; // 按钮文案
    std::function<void()> run; // 选择后执行的操作，空表示取消
};

/** @brief 取消公共居中选项窗，不执行操作 */
void ScreenDismissChoiceDialog();

/**
 * @brief 创建无网点遮罩的居中选项窗，按钮竖排；父对象销毁时取消回调，最多三个选项
 * @note 在 LVGL 线程调用；接纳后接管 actions 中的回调
 */
lv_obj_t* ScreenShowChoiceDialog(lv_obj_t* parent, const char* title, ScreenDialogAction* actions, size_t count);

/** @brief 显示删除确认窗；仅点击删除后执行回调 */
void ScreenConfirmDelete(lv_obj_t* parent, std::function<void()> confirmed);
