#pragma once

#include "lvgl.h"

// 传输页：App 待收、热点文件页、模拟 U 盘。
class CloudScreen {
public:
    /** @brief 创建云传输页 */
    static lv_obj_t* Create();

    /** @brief 停止当前传输并收起进度 UI；须在UI线程调用 */
    static void StopTransfer();

    /** @brief 电源键进待机前关掉 AP 热点，传输页保留 */
    static void ParkApForStandby();

    /** @brief 退出待机后若仍在 AP 传输页，重新打开热点 */
    static void ReloadApAfterStandby();
};
