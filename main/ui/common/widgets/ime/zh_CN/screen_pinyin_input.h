#pragma once
#include <lvgl.h>

/**
 * @brief 为书籍搜索输入框创建中文拼音及英数键盘，失败返回空指针
 * @param textarea 由父页面持有的单行输入框，字符上限由该输入框设置
 * @param confirm 确认键成功提交待选内容后调用的函数；允许在回调中删除父页面
 * @note 键盘按屏幕比例计算尺寸并贴底显示，父页面删除时一并回收。
 */
lv_obj_t* ScreenPinyinInputCreate(lv_obj_t* parent, lv_obj_t* textarea, void (*confirm)());

/** @brief 获取候选栏收起时的底部键盘高度，包含四周留白 */
lv_coord_t ScreenPinyinInputPreferredHeight();

/** @brief 清空输入框与待选词，保持键盘展开 */
void ScreenPinyinInputClear(lv_obj_t* keyboard);
