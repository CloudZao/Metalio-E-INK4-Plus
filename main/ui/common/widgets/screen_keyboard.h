#pragma once
#include <lvgl.h>

enum class ScreenKeyboardEvent {
    kInsert,     // 插入字符。
    kBackspace,  // 删除末尾字符。
    kClear,      // 清空输入。
    kConfirm,    // 勾选确认。
    kLayout,     // 键盘尺寸更新。
};
using ScreenKeyboardCallback = void (*)(ScreenKeyboardEvent, const char*, void*);  // 输入事件回调。

/** @brief 按当前屏幕高度比例获取四行键盘的首选高度。 */
lv_coord_t ScreenKeyboardPreferredHeight();

/** @brief 创建与 Wi-Fi 输入一致的墨水屏键盘，失败返回空指针 */
lv_obj_t* ScreenKeyboardCreate(lv_obj_t* parent, ScreenKeyboardCallback callback, void* context = nullptr);

/** @brief 恢复英文小写布局并重建按键 */
void ScreenKeyboardReset(lv_obj_t* keyboard);

/** @brief 停止输入反馈并取消待处理确认，保留键盘对象 */
void ScreenKeyboardPause(lv_obj_t* keyboard);

/** @brief 将大小写键替换为语言切换键，按键布局及反馈沿用原键盘 */
void ScreenKeyboardSetLanguageKey(lv_obj_t* keyboard, const char* label, bool uppercase,
                                  void (*switch_language)(void*));
