#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include "checklist_cache.h"

using ChecklistItem = checklist_cache_item_t;

enum class ChecklistOperation {
    Refresh, // 拉取完整待办
    Delete, // 删除指定条目
    Complete, // 完成指定条目
};

struct ChecklistResult {
    ChecklistOperation operation = ChecklistOperation::Refresh; // 本次操作
    bool batch = false; // 批量操作保留独立重试和汇总语义
    std::atomic<bool> completed{false}; // 发布后可读取以下结果字段
    int succeeded = 0; // 成功数量，空列表刷新成功也记为1
    int failed = 0; // 失败数量
    char error[80] = {}; // 可展示的错误说明
};

using ChecklistRequest = std::shared_ptr<const ChecklistResult>; // 页面只读结果，离页不终止在途操作

/**
 * @brief 提交待办操作，同一时刻只执行一个请求。
 * @param ids 更新对象；刷新时为空，单项操作仅传一个ID。
 * @param batch 批量更新启用原有重试和部分成功处理。
 * @return 返回只读结果引用；忙碌、参数无效或启动失败返回空。
 */
ChecklistRequest ChecklistSubmit(ChecklistOperation operation, std::vector<std::string> ids = {}, bool batch = false);

/** @brief 查询待办能力是否有在途请求，不依赖页面存在。 */
bool ChecklistIsBusy();

/** @brief 在已联网的启动线程同步更新共享缓存，与异步请求共用互斥和提交逻辑。 */
bool ChecklistSyncCache();
