#pragma once

#include "lvgl.h"

/** @brief 自动测试：按住说话 / 松开播放 */
void SettingsTestAudio_BuildRow(lv_obj_t* parent, lv_obj_t* root_scr);

/** @brief 进入测试页时重置录音状态并按需初始化音频资源。 */
void SettingsTestAudio_OnLoad();

/** @brief 离开测试页时停止录放音，清除控件引用并释放测试音频占用。 */
void SettingsTestAudio_Teardown();
