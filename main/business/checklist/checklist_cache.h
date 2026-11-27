/*
 * 每日清单内存缓存：开机联网 / 百问进页拉取后写入；
 * 页面只读展示，软件能力负责更新缓存。
 * 条目缓冲放 SPIRAM，避免占内部 DRAM。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
#include <vector>
#endif

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
 * @brief 一次加锁替换完整缓存，超出容量的条目截断
 * @return 分配或参数无效时返回 false，保留原缓存
 */
bool checklist_cache_replace(const checklist_cache_item_t* items, size_t count);

/**
 * @brief 是否已有过写入（含空列表成功拉取）
 * @return true 已 ready
 */
bool checklist_cache_ready(void);

/** @brief 将服务端已成功的删除或完成操作原子应用到缓存，不依赖页面存活。 */
void checklist_cache_apply_action(const char* id, bool remove);

/**
 * @brief 拷贝未完成条目到 out（供待机页）
 * @param out 输出缓冲
 * @param max_out 最大条数
 * @return 实际拷贝条数
 */
size_t checklist_cache_copy_pending(checklist_cache_item_t* out, size_t max_out);

/**
 * @brief 持锁统计当日事项及全部已完成、未完成数；无缓存返回 false
 * @param date 当日日期（YYYY-MM-DD），为空或无效时当日事项为 0，不影响全量统计
 */
bool checklist_cache_counts(const char* date, size_t* today, size_t* done, size_t* pending);

/** @brief 获取缓存提交版本，用于界面按需更新。 */
uint32_t checklist_cache_revision(void);

#ifdef __cplusplus
}

/** @brief 拷贝完整快照及其版本；分配失败返回false并保留调用方原列表。 */
bool ChecklistCopyItems(std::vector<checklist_cache_item_t>& out, uint32_t& revision);
#endif
