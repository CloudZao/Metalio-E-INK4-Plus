#pragma once
#include <lvgl.h>

using DisplayUiScreenFactory = lv_obj_t* (*)();

struct DisplayUiBindings {
    lv_obj_t* (*create_startup)(bool touch_ready, bool defer_restore, DisplayUiScreenFactory& pending) = nullptr; // 页面装配持有恢复策略
    bool (*prepare_power_off)() = nullptr; // 页面保存与资源交还，不负责显示硬件断电
    bool (*wave_visible)() = nullptr; // 页面是否使用状态栏录音波形
};

/** @brief 在显示初始化前绑定页面协作，运行期保持不变。 */
void DisplayUiConfigure(const DisplayUiBindings& bindings);

/** @brief 创建启动页并返回需在系统就绪后执行的恢复工厂。 */
lv_obj_t* DisplayUiCreateStartup(bool touch_ready, bool defer_restore, DisplayUiScreenFactory& pending);

/** @brief 在关机显示前请求页面交还资源，失败时不得继续断电。 */
bool DisplayUiPreparePowerOff();

/** @brief 查询当前页面是否占用状态栏波形区域。 */
bool DisplayUiWaveVisible();
