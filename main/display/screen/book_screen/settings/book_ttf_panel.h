#pragma once

#include <lvgl.h>

/** @brief 是否为 TTF/OTF 文件名 */
bool IsTtfFileName(const char* name);
/** @brief TTF 列表总页数 */
int TtfPageCount();
/** @brief 根据字体列表可用高度计算每页行数。 */
int TtfRowsPerPage();
/** @brief 扫描 SD 上 TTF/OTF */
void TtfScanFiles();
/** @brief 关闭并清理 TTF 面板 */
void TtfPanelClose();
/** @brief 停止 TTF 轮询并请求取消在途转换 */
void StopTtfPollTimer();
/** @brief 确保 TTF 面板已创建 */
void TtfEnsurePanel();
/** @brief 重建 TTF 面板内容 */
void TtfRebuildContent();

/** @brief 显示带提示图标的字体转换成功通知，三秒后关闭。 */
void TtfShowSuccessNotice();
