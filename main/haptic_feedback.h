#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 震动偏好：NVS namespace "display" / key "haptic"，默认开启。
// 读 RAM 缓存；写内存后由后台任务异步落盘，不要在 LVGL 任务直接写 flash。
bool HapticIsEnabled(void);
void HapticSetEnabled(bool enabled);

// 开关开启时触发 Board::PulseVibration()；无马达板为空操作。
void HapticPulseIfEnabled(void);

// 一次新的按下边沿：touch_feed 在 try/震前调用，递增 press_seq 并清理占位状态。
void HapticBeginPress(void);

// 当前按下已判定要震：占位并脉冲，避免重复触发。
void HapticPulseOnFingerDown(void);

// 屏内按下：命中热区才早震，不抢 LVGL 锁；须先 HapticBeginPress。
bool HapticTryPulseAtUiPoint(int16_t x, int16_t y);

// 热区早震总开关：全屏遮罩页可关闭，避免误震。
void HapticSetZoneEarlyPulseEnabled(bool enabled);

// 挂 PRESSED 兜底短震，并登记控件屏幕矩形供 touch_feed 早震。
void HapticAttachClick(lv_obj_t* obj);

// 解除 HapticAttachClick：撤热区与事件，避免整页残留误震。
void HapticDetachClick(lv_obj_t* obj);

#ifdef __cplusplus
}
#endif
