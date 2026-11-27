#pragma once

/** @brief 查询当前阅读会话是否启用自动翻页。 */
bool ReaderAutoTurnEnabled();

/** @brief 在 LVGL 上下文启停本会话的自动翻页定时器。 */
void SetReaderAutoTurnEnabled(bool enabled);

/** @brief 在 LVGL 上下文停止自动翻页，销毁定时器并取消待消费请求。 */
void StopReaderAutoTurn();

/** @brief 按已保存的翻页间隔重设自动翻页截止时间。 */
void ResetReaderAutoTurnDeadline();

/** @brief 自动翻页启用时重设截止时间并登记用户活动。 */
void ReaderAutoTurnPageMoved();

/** @brief 立即重设截止时间并取消尚未消费的自动翻页请求，可由输入任务调用。 */
void ReaderAutoTurnManualInput();

/** @brief 在 LVGL 上下文消费一次仍可执行的自动翻页请求。 */
bool ConsumeReaderAutoTurnRequest();
