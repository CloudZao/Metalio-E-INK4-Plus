#pragma once

#include <lvgl.h>

/** @brief 钳制 TTF 转换字号 */
void ClampTtfSizePx();
/** @brief 刷新 TTF 确认区文案 */
void RefreshTtfConfirmTexts();
/** @brief 启动 TTF→epdfont 转换 */
void TtfStartConvert();
/** @brief TTF 转换进度轮询 */
void TtfPollTimerCb(lv_timer_t* t);
/** @brief 点击导入字体 */
void OnFontImportClick(lv_event_t* e);
/** @brief TTF 列表行点击 */
void OnTtfRowClick(lv_event_t* e);
/** @brief 长按源字体进入多选或切换选中状态。 */
void OnTtfRowLongPressed(lv_event_t* e);
/** @brief 获取已选源字体数量。 */
int TtfSelectedCount();
/** @brief 退出源字体多选。 */
void OnTtfMultiCancel(lv_event_t* e);
/** @brief 全选或清空源字体选择。 */
void OnTtfMultiSelectAll(lv_event_t* e);
/** @brief 删除所选源字体，先替换正在使用的字体。 */
void OnTtfMultiRemove(lv_event_t* e);
/** @brief 减小 TTF 字号 */
void OnTtfSizeDec(lv_event_t* e);
/** @brief 增大 TTF 字号 */
void OnTtfSizeInc(lv_event_t* e);
/** @brief TTF 列表上一页 */
void OnTtfPagePrev(lv_event_t* e);
/** @brief TTF 列表下一页 */
void OnTtfPageNext(lv_event_t* e);
/** @brief 确认并启动转换 */
void OnTtfConfirmYes(lv_event_t* e);
/** @brief 按选定字号直接加载 TTF/OTF 并应用于阅读。 */
void OnTtfApply(lv_event_t* e);
/** @brief 取消转换确认 */
void OnTtfConfirmNo(lv_event_t* e);
/** @brief 返回上一级字体页面并取消在途转换 */
void OnTtfClose(lv_event_t* e);

/** @brief 取消转换并返回 TTF 文件列表。 */
void ReturnToTtfList();
