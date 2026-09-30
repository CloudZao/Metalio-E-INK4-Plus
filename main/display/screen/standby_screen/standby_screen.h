#pragma once

#include "lvgl.h"

// 待机 Overlay：叠加在当前页面之上，不销毁底层页面。
class StandbyScreen {
public:
    /** @brief 创建待机页根对象（供导航工厂） */
    static lv_obj_t* Create();

    /** @brief 在当前 active screen 上叠加全屏待机页；须在 LVGL 线程调用 */
    static void Show();

    /** @brief 关闭待机 Overlay，并恢复进入前页面 */
    static void Dismiss();

    /** @brief Overlay 是否处于显示状态 */
    static bool IsActive();

    /** @brief 待机内容是否已就绪（经典页：至少完成一帧 flush；壁纸：解码就绪） */
    static bool IsPaintReady();

    /** @brief LVGL flush 末包完成时调用（供 epdiy flush_cb） */
    static void NotifyFlushCompleted();

    /** @brief 待机 UI 就绪后，冻结 EPD flush 并全刷当前画面 */
    static void EnterEpdSleep();

    /** @brief 退出待机前恢复 EPD flush；未冻结则为空操作 */
    static void ExitEpdSleep();

    /** @brief BOOT 短按：待机中忽略，不退出 Overlay */
    static bool HandleBootClick();

    /** @brief BOOT 长按：满时长后摘掉 Overlay 并打开百问 */
    static bool HandleBootLongPress();

    /** @brief 确保当日天气缓存存在；缺失时异步补全 */
    static void EnsureWeatherCached();
};
