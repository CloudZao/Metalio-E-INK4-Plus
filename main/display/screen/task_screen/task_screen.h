#pragma once

#include "lvgl.h"

// ---------------------------------------------------------------------------
// TaskScreen（墨水屏 · 每日清单）
//
// - 展示 checklist_cache；进页只读缓存，无缓存显示空态（不在本页主动拉取）
// - 缓存：开机 EnsureCacheSynced()（若已在本页则 lv_async 刷列表）；进百问等 RequestCacheRefresh()
// - 页码行右侧「刷新」：未联网则 EnsureNetworkReady；GET 刷新 cache（节流 3s）；结果上屏后再放网启保网计时
// - 点击 item 操作框：进行中「完成待办」+「删除待办」；已完成仅「删除待办」
// - 长按 item 进多选并勾选该项；点行或框切换勾选；框内「取消/移除/完成」批量（已完成 Tab 无完成）
// - 顶部 Tab；列表无滚动，vk_prev/vk_next 翻页（长按连翻）；多选时首页 vk_prev 取消多选；vk_home 回首页
// - 操作提示与标题同行稍后（不盖列表、不占页码），成功/错误约 2s 收起；空列表错误只显示一条
// - 无返回按钮；点弹框外网点遮罩关闭
// - 底栏：外框包住待办列表+刷新/批量；页码在框外
// ---------------------------------------------------------------------------
class TaskScreen {
public:
    static lv_obj_t* Create();
    /**
     * @brief 当前是否在每日清单页
     */
    static bool IsActive();
    /**
     * @brief 后台 GET 清单并写入 checklist_cache（可不在清单页调用）
     * @note 须在 DRAM 栈上下文触发（如 Application::Schedule），勿在 LVGL/PSRAM 栈直接 Ensure 网络
     */
    static void RequestCacheRefresh();
    /**
     * @brief 同步 GET 清单写入 cache（开机联网成功后调用）
     */
    static void EnsureCacheSynced();
    /**
     * @brief 从 checklist_cache 刷新本页列表（无网络）
     */
    static void ReloadFromCache();
    /**
     * @brief 待机唤醒后：若仍在清单页则从缓存重载
     */
    static void OnResumeFromStandby();
};
