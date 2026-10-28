#pragma once

#include "lvgl.h"

/** @brief WorkBuddy应用，扫码授权后长按屏幕或BOOT录音，短点左右区域或实体键翻页。 */
class WorkBuddyScreen {
public:
    /**
     * @brief 创建整页对话界面并绑定500毫秒长按录音、短点分区翻页与实体按键。
     * @return 新建的 LVGL 屏幕，由统一页面导航管理生命周期。
     */
    static lv_obj_t* Create();

    /**
     * @brief 读取倾听波形状态，供显示驱动避免时钟覆盖录音提示。
     * @return 波形可见时为true；允许跨任务调用。
     */
    static bool IsPttWaveVisible();

    /** @brief 待机时停止录音和请求，唤醒后恢复页面交互；须在 LVGL 线程调用 */
    static void SetStandby(bool standby);
};
