#pragma once

/**
 * @file assistant_chat_store.h
 * @brief 启动周期内可选的 SD A2UI JSON 会话日志
 *
 * 架构
 * ----
 * - 职责：仅存储（目录 / 开机清空 / 序号文件 / FIFO）。解析与 LVGL 仍在
 *   assistant_screen.cc — 本模块不碰 UI。
 * - 路径：`SD_PATH_CHAT_LOG`（`/sdcard/metalio/e-ink/chat_log`）。
 *   无 SD → 禁用（纯 RAM 会话，与原先行为一致）。
 *   有 SD → 开机清空，再追加每条已接受的下行 JSON。
 * - 布局：每轮一文件 `mNNNNN.json`（原始 UTF-8 A2UI 对象）。序号即回放顺序。
 *   不跨开机持久化。
 * - 预算（日常聊天，非无限归档）：
 *     单文件 ≤ 24KB，≤ 40 个文件，合计 ≤ 192KB。
 *   超限先删最旧再写（不碎片保留）。
 * - 失败策略：不拖垮开机或聊天 UI。
 *
 * 线程：互斥保护。典型调用方：Application::Start（init）、
 * AssistantScreen::AddMessage / Create（已在 LVGL 任务；载荷较小）。
 */

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sd_paths.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ASSISTANT_CHAT_STORE_BASE_PATH SD_PATH_CHAT_LOG

/**
 * @brief 若 SD 已挂载：确保目录、清空上一次开机残留并标记就绪
 * @note 无 SD 时返回 ESP_ERR_NOT_FOUND，缓存保持禁用。幂等；不致命。
 */
esp_err_t assistant_chat_store_init(void);

/**
 * @brief 存储是否已就绪（有 SD 且 init 成功）
 */
bool assistant_chat_store_is_ready(void);

/**
 * @brief 尽力追加一条 A2UI JSON 对象（NUL 结尾或显式长度）
 * @note 超大记录跳过。预算溢出 → 删最旧再写。
 */
esp_err_t assistant_chat_store_append(const char *json, size_t len);

/**
 * @brief 删除全部记录（deleteSurface / 显式清空）。未就绪则为空操作。
 */
esp_err_t assistant_chat_store_wipe(void);

/**
 * @brief 按序号遍历记录。回调内不得重入本模块。
 * @note json 为 NUL 结尾，仅在回调期间有效。
 *       零记录仍返回 ESP_OK；未启用返回 ESP_ERR_INVALID_STATE。
 */
typedef bool (*assistant_chat_store_visit_fn)(const char *json, size_t len, void *ctx);

/** @brief 按序遍历当日会话记录 */
esp_err_t assistant_chat_store_for_each(assistant_chat_store_visit_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif
