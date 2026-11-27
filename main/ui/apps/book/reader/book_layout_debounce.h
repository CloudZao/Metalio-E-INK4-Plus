#pragma once

/** @brief 即时应用排版预览（可重载字体） */
void ApplyReaderLayoutLive(bool reload_font);

/** @brief 取消排版防抖定时器 */
void CancelLayoutDebounce();

/** @brief 提交尚未应用的排版设置，当前章节由既有排版任务重建 */
void FlushLayoutDebounce();

/** @brief 防抖调度排版应用 */
void ScheduleLayoutApply(bool reload_font);

/** @brief 排版提示是否忙碌 */
bool IsLayoutHintBusy();

/** @brief 标记排版提示忙碌 */
void MarkLayoutHintBusy();

/** @brief 标记排版提示完成 */
void MarkLayoutHintDone();

/** @brief 确保排版提示定时器存在 */
void EnsureLayoutHintTimer();

/** @brief 停止排版提示定时器 */
void StopLayoutHintTimer();
