#pragma once

#include <esp_err.h>
#include <lvgl.h>

class YunxinBinding {
public:
    /** @brief 当前板型和云服务是否使用国际版扫码绑定 */
    static bool Enabled();
    /** @brief 当前国际版网易设备是否仍需完成扫码绑定 */
    static bool NeedsBinding();
    /** @brief 在页面内容区创建绑定面板；已绑定时返回空，随父容器销毁，须在 UI 线程调用 */
    static lv_obj_t* CreatePanel(lv_obj_t* parent, void (*on_bound)() = nullptr);
    /** @brief 是否已到下一次查询时间 */
    static bool IsDue();
    /** @brief 查询绑定状态并更新二维码；仅由串行网络任务调用，成功时写入 active */
    static esp_err_t Check(bool& active);
};
