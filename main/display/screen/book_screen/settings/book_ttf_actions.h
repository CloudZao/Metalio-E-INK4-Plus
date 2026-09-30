#pragma once

#include <lvgl.h>

/** @brief 钳制 TTF 预览字号 */
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
/** @brief 减小 TTF 字号 */
void OnTtfSizeDec(lv_event_t* e);
/** @brief 增大 TTF 字号 */
void OnTtfSizeInc(lv_event_t* e);
/** @brief TTF 列表上一页 */
void OnTtfPagePrev(lv_event_t* e);
/** @brief TTF 列表下一页 */
void OnTtfPageNext(lv_event_t* e);
/** @brief 确认转换 */
void OnTtfConfirmYes(lv_event_t* e);
/** @brief 取消转换确认 */
void OnTtfConfirmNo(lv_event_t* e);
/** @brief 关闭 TTF 面板 */
void OnTtfClose(lv_event_t* e);
/** @brief 转换结果确认 */
void OnTtfResultOk(lv_event_t* e);
