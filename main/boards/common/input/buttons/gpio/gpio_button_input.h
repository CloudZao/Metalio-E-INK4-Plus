#pragma once

#include <memory>
#include "boards/common/input/buttons/button.h"

/** 电源键开机残留按住：屏蔽/吞 Click 状态（由门面持有） */
struct PowerKeyGestureState {
    bool suppress = false;       // 屏蔽电源键手势直至松手
    bool swallow_click = false;  // 吞掉松手后的 Click
};

/**
 * @brief 注册 BOOT/POWER 回调，并在 IOExpander 就绪时创建音量键
 * @param boot BOOT 按键
 * @param power 电源键
 * @param volume_up 音量+（出参，可为空）
 * @param volume_down 音量-（出参，可为空）
 * @param power_key 电源键 suppress/swallow 状态（门面成员）
 */
void GpioButtonInput_Init(Button& boot, Button& power, std::unique_ptr<Button>& volume_up,
                          std::unique_ptr<Button>& volume_down, PowerKeyGestureState& power_key);
