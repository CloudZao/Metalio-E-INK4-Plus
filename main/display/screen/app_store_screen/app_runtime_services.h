#pragma once
#include "app_store/app_api.h"
#include <array>
#include <optional>
#include "power_policy.h"
#include <string>
#include <lvgl.h>

namespace AppRuntimeServices {
struct Timer {
    lv_timer_t* handle = nullptr; // LVGL 托管定时器
    uint32_t id = 0; // 应用可见的非零标识
    xz_app_click_t callback = nullptr; // ELF 回调
    void* context = nullptr; // 应用回调数据
    bool repeat = false; // 是否重复执行
    lv_obj_t* root = nullptr; // 用于屏蔽离页后的回调
};
struct State {
    lv_obj_t* root = nullptr; // 应用内容根容器
    lv_obj_t* keyboard = nullptr; // 当前托管键盘
    std::string storage_key; // 已校验的应用名称与 ID 组成的私有数据目录名
    std::array<Timer, 8> timers {}; // 前台定时器固定上限
    uint32_t next_id = 0; // 当前运行期间不复用的定时器编号
    std::optional<PowerNeedHold> awake; // 仅运行期间持有自动待机禁止引用
    bool open = false; // 是否接受新的服务请求
    bool closing = false; // 是否正在执行退出回调
    xz_app_click_t on_exit = nullptr; // 退出时执行一次的回调
    void* exit_context = nullptr; // 退出回调数据
};
/** @brief 绑定运行期基础服务并填充 ABI 尾部接口 */
void Bind(State& state, xz_app_api_t& api, lv_obj_t* root, const std::string& app_id, const std::string& installed_name);
/** @brief 停止所有回调并释放托管资源，须在释放 ELF 和界面前调用 */
void Close(State& state);
}
