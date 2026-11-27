#pragma once

#include "ui/common/runtime/input/vk_key_handler.h"

/** @brief 清空返回栈 */
void ScreenCommon_ClearBackStack();

/** @brief 压入返回工厂 */
void ScreenCommon_PushBackFactory(ScreenFactory factory);

/** @brief 弹出返回工厂（空则返回 nullptr） */
ScreenFactory ScreenCommon_PopBackFactory();

/** @brief 异步回首页（lv_async 入口） */
void ScreenCommon_GoHomeAsync(void* user_data);

/** @brief 异步返回上一屏（lv_async 入口） */
void ScreenCommon_NavigateBackAsync(void* user_data);

/** @brief 取消尚未执行的建屏导航，保留当前页面。 */
void ScreenCommon_CancelPendingNavigation();

struct ScreenNavigationConfig {
    ScreenFactory create_home = nullptr; // 首页工厂，构建前由应用入口绑定
    void (*prepare)() = nullptr; // 切页前释放应用独占的显示资源
    bool (*is_standby)() = nullptr; // 待机状态查询，不暴露页面对象
    void (*open_network)() = nullptr; // 状态栏网络操作入口
    void (*poll_navigation)() = nullptr; // 应用装配提供的延后导航入口
};

/** @brief 在系统启动创建页面前绑定应用导航入口。 */
void ScreenConfigureNavigation(const ScreenNavigationConfig& config);

/** @brief 经应用入口处理状态栏网络操作。 */
void ScreenOpenNetworkSettings();

/** @brief 替换加载新屏 */
void ScreenLoadReplace(lv_obj_t* new_scr);

/** @brief 导航到工厂创建的新屏；落墨期间延后建屏，可指定来源页面的返回工厂。 */
void ScreenNavigateTo(lv_obj_t* (*create)(), lv_obj_t* (*return_to)() = nullptr);

/** @brief 返回上一屏 */
void ScreenNavigateBack();

/** @brief 标记当前是否为首页 */
void ScreenSetIsHome(bool is_home);

/** @brief 当前是否为首页 */
bool ScreenIsHome();

/** @brief 请求回到首页；落墨结束后建屏 */
void ScreenGoHome();

/** @brief 异步请求回首页 */
void ScreenRequestHome();


/** @brief 在UI上下文处理导航、异步交付、页面更新和公共触摸反馈。 */
void ScreenPollPendingWork();
