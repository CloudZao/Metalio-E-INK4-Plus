#pragma once

#include "standby_classic_priv.h"

/** @brief 点击待办项打开清单 */
void StandbyClassic_OnTodoItemClicked(lv_event_t* e);
/** @brief 异步打开清单页 */
void StandbyClassic_OpenTaskScreenAsync(void* arg);
/** @brief 设置待办区提示文案 */
void StandbyClassic_SetTodoHint(const char* text);
/** @brief 重建待办列表 */
void StandbyClassic_RebuildTodoList();
/** @brief 点击回首页按钮 */
void StandbyClassic_OnHomeBtnClicked(lv_event_t* e);
/** @brief 格式化待办计划时间文案 */
void StandbyClassic_FormatTodoPlanMeta(char* out, size_t out_sz, const TodoItem& item);
/** @brief 从 checklist_cache 填充待办 */
void StandbyClassic_ApplyTodosFromCache();
