#pragma once
#include "boards/common/input/input.h"
#include "infra/storage/key_bindings.h"

/** @brief 在界面线程执行自定义按键事件；按下、松开及取消须成对交付。 */
void BindingDispatch(BindingKey key, InputAction action);

/** @brief 取消所有在途按住操作，队列溢出或切换设置时调用。 */
void BindingDispatchCancelAll();
