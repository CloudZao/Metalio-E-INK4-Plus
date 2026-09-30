#pragma once

#include <lvgl.h>

/** @brief 是否为 TTF/OTF 文件名 */
bool IsTtfFileName(const char* name);
/** @brief TTF 列表总页数 */
int TtfPageCount();
/** @brief 扫描 SD 上 TTF/OTF */
void TtfScanFiles();
/** @brief 关闭并清理 TTF 面板 */
void TtfPanelClose();
/** @brief 停止 TTF 轮询定时器 */
void StopTtfPollTimer();
/** @brief 确保 TTF 面板已创建 */
void TtfEnsurePanel();
/** @brief 重建 TTF 面板内容 */
void TtfRebuildContent();
