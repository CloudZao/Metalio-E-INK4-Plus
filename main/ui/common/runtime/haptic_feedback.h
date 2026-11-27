#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "business/haptics/haptic_preferences.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 记录新的按下边沿，清除本次按压的震动占位；须在尝试早震前调用。 */
void HapticBeginPress(void);

/** @brief 为当前按压占位并触发短震，避免同次按压重复震动。 */
void HapticPulseOnFingerDown(void);

/** @brief 命中屏内热区时尝试早震，不获取 LVGL 锁；须先记录按下边沿。 */
bool HapticTryPulseAtUiPoint(int16_t x, int16_t y);

/** @brief 启用或关闭热区早震，全屏遮罩可关闭以避免误触发。 */
void HapticSetZoneEarlyPulseEnabled(bool enabled);

/** @brief 在 UI 线程刷新可见、未禁用且裁剪后的热区快照 */
void HapticRefreshZones(void);

/** @brief 绑定按压短震事件，并登记控件矩形供触摸任务早震。 */
void HapticAttachClick(lv_obj_t* obj);

/** @brief 撤销按压短震事件及控件热区登记。 */
void HapticDetachClick(lv_obj_t* obj);

#ifdef __cplusplus
}
#endif
