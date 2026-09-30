#pragma once

#include "lvgl.h"

/** @brief 自动测试：按住说话 / 松开播放（对照 397） */
void SettingsTestAudio_BuildRow(lv_obj_t* parent, lv_obj_t* root_scr);
void SettingsTestAudio_OnLoad();
void SettingsTestAudio_Teardown();
