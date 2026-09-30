#pragma once

#include "lvgl.h"

// 百问 AI 页面：管理会话内容、PPT 状态和按键语义。
class AssistantScreen {
public:
    /** @brief 创建百问页 */
    static lv_obj_t* Create();

    /** @brief 当前页面是否在前台；后台消息会被忽略 */
    static bool IsActive();

    /** @brief PTT 是否仍保持按下；用于统一监听和松手判定 */
    static bool IsPttHeld();

    /** @brief 状态栏是否显示录音波形 */
    static bool IsPttWaveVisible();

    /** @brief 同步当前 PTT/监听状态到状态栏 */
    static void SyncPttOverlay();

    /** @brief 异步打开页面；已在前台则忽略 */
    static void RequestOpen();

    /** @brief 向会话流追加消息；须在持 LVGL 锁时调用 */
    static void AddMessage(const char* role, const char* content);

    /** @brief 设置情绪表情（当前可为空操作） */
    static void SetEmotion(const char* emotion);
};
