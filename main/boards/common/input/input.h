#pragma once

#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
#include "config.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    InputKeyBoot,        // 逻辑BOOT键
    InputKeyPower,       // 逻辑电源键
    InputKeyVolumeUp,    // 逻辑音量加键
    InputKeyVolumeDown,  // 逻辑音量减键
    InputKeyCount,       // 逻辑按键数量
} InputKey;

typedef enum {
    InputPressDown,    // 按下
    InputPressUp,      // 松开
    InputClick,        // 单击
    InputDoubleClick,  // 双击
    InputLongPress,    // 长按
    InputCancel,       // 采样异常，取消当前手势
} InputAction;

typedef struct {
    InputKey key;        // 逻辑按键
    InputAction action;  // 已识别的手势
} InputEvent;

/** @brief 将采样任务识别的手势提交给界面线程，不用于ISR。 */
bool InputPostEvent(InputKey key, InputAction action);

/** @brief 在界面线程领取一个手势事件。 */
bool InputTakeEvent(InputEvent* event);

/** @brief 领取队列溢出标记，调用方须取消在途长按操作。 */
bool InputTakeOverflow(void);

/** @brief 查询逻辑按键当前状态，隐藏GPIO及板级按键编号。 */
bool InputKeyIsDown(InputKey key);

/** @brief 领取一次电源短按事件。 */
bool InputTakePowerClick(void);

/** @brief 领取一次电源长按关机请求。 */
bool InputTakeShutdownRequest(void);

/** @brief 查询作用域暂停状态，供输入采样任务调用，不用于ISR。 */
bool InputSamplingPaused(void);
#ifdef __cplusplus
}

static_assert(BOARD_BOOT_LONG_PRESS_MS > 0 && BOARD_BOOT_LONG_PRESS_MS <= UINT16_MAX,
              "BOOT long-press threshold must fit a positive uint16_t millisecond value");
static_assert(BOARD_POWER_LONG_PRESS_MS > 0 && BOARD_POWER_LONG_PRESS_MS <= UINT16_MAX,
              "Power long-press threshold must fit a positive uint16_t millisecond value");
inline constexpr uint16_t kBootLongPressMs = BOARD_BOOT_LONG_PRESS_MS;    // 所选板型的BOOT与侧键长按阈值
inline constexpr uint16_t kPowerLongPressMs = BOARD_POWER_LONG_PRESS_MS;  // 所选板型的电源键关机阈值

/** @brief 在作用域内暂停触摸采样，支持嵌套；Flash操作另由访问门闩排空在途事务。 */
class InputSamplingPause {
public:
    /** @brief 为当前作用域登记一次触摸采样暂停请求 */
    InputSamplingPause();

    /** @brief 归还暂停请求，最后一层请求退出后恢复采样 */
    ~InputSamplingPause();
    InputSamplingPause(const InputSamplingPause&) = delete;
    InputSamplingPause& operator=(const InputSamplingPause&) = delete;
};
#endif
