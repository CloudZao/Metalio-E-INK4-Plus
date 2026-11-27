#pragma once

#include "lvgl.h"

/** @brief 构建设置→对话页（TTS 开关） */
void SettingsConversationTab_Build(lv_obj_t* page);

/** @brief 解除页面更新与结果接收，业务操作独立完成。 */
void SettingsConversationTab_Reset();

/** @brief 进入对话 Tab 时拉取服务端 TTS 状态 */
void SettingsConversationTab_OnActivated();
