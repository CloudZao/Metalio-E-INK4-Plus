#pragma once
#include <lvgl.h>

enum class ScreenKeyboardEvent {
    kInsert, // 插入字符。
    kBackspace, // 删除末尾字符。
    kClear, // 清空输入。
    kConfirm, // 勾选确认。
    kLayout, // 键盘尺寸更新。
};
using ScreenKeyboardCallback = void (*)(ScreenKeyboardEvent, const char*, void*); // 输入事件回调。
/** @brief 创建与 Wi-Fi 输入一致的墨水屏键盘，失败返回空指针 */
lv_obj_t* ScreenKeyboardCreate(lv_obj_t* parent, ScreenKeyboardCallback callback, void* context = nullptr);
/** @brief 恢复英文小写布局并重建按键 */
void ScreenKeyboardReset(lv_obj_t* keyboard);
/** @brief 停止输入反馈并取消待处理确认，保留键盘对象 */
void ScreenKeyboardPause(lv_obj_t* keyboard);
