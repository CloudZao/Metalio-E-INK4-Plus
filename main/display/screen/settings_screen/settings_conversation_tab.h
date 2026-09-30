#pragma once

#include "lvgl.h"

/** @brief 构建设置→对话页（TTS 开关） */
void SettingsConversationTab_Build(lv_obj_t* page);
/** @brief 重置对话页 UI（不强制清 busy，避免离 Tab 重入） */
void SettingsConversationTab_Reset();
/** @brief 进入对话 Tab 时拉取服务端 TTS 状态 */
void SettingsConversationTab_OnActivated();
