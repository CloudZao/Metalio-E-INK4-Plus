#pragma once

/** @brief BOOT 键按下 */
void BootKey_OnPressDown();
/** @brief BOOT 键抬起 */
void BootKey_OnPressUp();
/** @brief BOOT 键短按单击 */
void BootKey_OnClick();
/** @brief BOOT 键双击 */
void BootKey_OnDoubleClick();
/** @brief BOOT 键长按 */
void BootKey_OnLongPress();

/** @brief 电源键短按：进入低功耗待机 Overlay */
void PowerKey_OnClick();
/** @brief 电源键长按：请求关机 */
void PowerKey_OnLongPress();

/** @brief 当前 BOOT 键是否仍按住 */
bool BootKey_IsHeld();
/** @brief 本轮按下是否已触发过长按（用于抑制短按） */
bool BootKey_DidLongPress();
