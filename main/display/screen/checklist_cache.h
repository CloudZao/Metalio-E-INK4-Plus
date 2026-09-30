/*
 * 每日清单内存缓存：开机联网 / 百问进页拉取后写入；
 * TaskScreen / StandbyScreen 只读展示，禁止再 HTTP。
 * 条目缓冲放 SPIRAM，避免占内部 DRAM。
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHECKLIST_CACHE_TITLE_LEN 96   // 标题缓冲长度
#define CHECKLIST_CACHE_STATUS_LEN 16  // 状态串缓冲长度
#define CHECKLIST_CACHE_DATE_LEN 16    // planDate 缓冲长度
#define CHECKLIST_CACHE_TIME_LEN 16    // planTime 缓冲长度
#define CHECKLIST_CACHE_ID_LEN 48      // 条目 id（完成/删除 API 用）
#define CHECKLIST_CACHE_MAX_ITEMS 64   // 最大缓存条数

typedef struct {
    char id[CHECKLIST_CACHE_ID_LEN];           // 服务端条目 id
    char title[CHECKLIST_CACHE_TITLE_LEN];     // 标题
    char status[CHECKLIST_CACHE_STATUS_LEN];   // pending / done 等
    char plan_date[CHECKLIST_CACHE_DATE_LEN];  // 计划日期
    char plan_time[CHECKLIST_CACHE_TIME_LEN];  // 计划时间
} checklist_cache_item_t;

/**
 * @brief 清空并开始写入（条目落在 SPIRAM）
 */
void checklist_cache_begin(void);

/**
 * @brief 追加一条；超过 MAX 忽略
 * @param id 条目 id，可为 nullptr（无 id 则无法完成/删除）
 * @param title 标题，可为 nullptr
 * @param status 状态串，可为 nullptr
 * @param plan_date 计划日期，可为 nullptr
 * @param plan_time 计划时间，可为 nullptr
 * @return true 已写入
 */
bool checklist_cache_append(const char* id, const char* title, const char* status,
                            const char* plan_date, const char* plan_time);

/**
 * @brief 结束写入，标记 ready（允许 n=0）
 */
void checklist_cache_end(void);

/**
 * @brief 是否已有过写入（含空列表成功拉取）
 * @return true 已 ready
 */
bool checklist_cache_ready(void);

/**
 * @brief 拷贝全部条目到 out（供清单 App）
 * @param out 输出缓冲
 * @param max_out 最大条数
 * @return 实际拷贝条数
 */
size_t checklist_cache_copy_all(checklist_cache_item_t* out, size_t max_out);

/**
 * @brief 拷贝未完成条目到 out（供待机页）
 * @param out 输出缓冲
 * @param max_out 最大条数
 * @return 实际拷贝条数
 */
size_t checklist_cache_copy_pending(checklist_cache_item_t* out, size_t max_out);

#ifdef __cplusplus
}
#endif
