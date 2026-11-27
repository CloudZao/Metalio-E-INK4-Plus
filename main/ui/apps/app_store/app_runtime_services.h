#pragma once
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <lvgl.h>
#include "business/app_store/app_api.h"
#include "business/app_store/app_store.h"
#include "sysmgmt/power/policy/power_policy.h"

struct AppNetwork;
struct AppAudioPlayer;
struct AppAudioRecorder;
struct AppRuntimeImages;
struct AppCameraPreview;

struct AppRuntimeServicesState;

struct AppRuntimeServicesTimer {
    lv_timer_t* handle = nullptr;       // LVGL 托管定时器
    uint32_t id = 0;                    // 应用可见的非零标识
    xz_app_click_t callback = nullptr;  // ELF 回调
    void* context = nullptr;            // 应用回调数据
    bool repeat = false;                // 是否重复执行
    lv_obj_t* root = nullptr;           // 用于屏蔽离页后的回调
    AppRuntimeServicesState* state = nullptr;  // 所属服务，用于推白期间暂停回调
};

struct AppRuntimeServicesState {
    lv_obj_t* root = nullptr;                         // 应用内容根容器
    lv_obj_t* keyboard = nullptr;                     // 当前托管键盘
    std::string storage_key;                          // 已校验的应用名称与 ID 组成的私有数据目录名
    std::array<AppRuntimeServicesTimer, 8> timers{};  // 前台定时器固定上限
    uint32_t next_id = 0;                             // 当前运行期间不复用的定时器编号
    std::optional<PowerNeedHold> awake;               // 仅运行期间持有自动待机禁止引用
    std::optional<PowerNeedHold> vibration_hold;      // 单次震动完成前阻止无操作待机
    uint64_t vibration_until = 0;                     // 本次应用震动的单调毫秒截止时间
    uint32_t vibration_id = 0;                        // 仅取消本应用仍持有的驱动脉冲
    lv_timer_t* foreground_timer = nullptr;           // 前台计时与传感器订阅管理
    uint64_t foreground_elapsed = 0;                  // 前台累计毫秒
    uint64_t foreground_last = 0;                     // 上次宿主单调时间
    bool foreground = false;                          // 上次检查是否处于前台
    bool acceleration_requested = false;              // 应用采样请求
    bool acceleration_running = false;                // 已应用的驱动订阅状态
    uint32_t transition_sequence = 0;                 // 推白请求前已提交的最后一帧
    bool transition_pending = false;                  // 等待目标画面物理刷新完成
    bool full_refresh_pending = false; // 合并尚未完成的显式全刷请求
    uint32_t full_refresh_sequence = 0; // 全刷请求前的提交序号
    bool open = false;                                // 是否接受新的服务请求
    bool closing = false;                             // 是否正在执行退出回调
    xz_app_click_t on_exit = nullptr;                 // 退出时执行一次的回调
    void* exit_context = nullptr;                     // 退出回调数据
    xz_app_back_t on_back = nullptr;                  // 应用内部返回处理
    void* back_context = nullptr;                     // 返回回调数据
    std::shared_ptr<AppNetwork> network; // 后台只持有宿主会话，退出先撤销所有句柄
    std::shared_ptr<AppAudioRecorder> recorder; // 宿主录音会话，离页后后台保存
    std::shared_ptr<AppAudioPlayer> audio;            // 宿主音频会话，后台不访问 ELF 内存
    std::shared_ptr<AppRuntimeImages> images; // 托管图片与异步资源解码
    std::shared_ptr<AppCameraPreview> camera; // 不借用 ELF 内存的摄像头会话
    void* camera_widget = nullptr; // 仅 UI 线程使用，交付前检查控件仍然有效
};

/** @brief 绑定运行期基础服务并填充 ABI 尾部接口 */
void AppRuntimeServicesBind(AppRuntimeServicesState& state, xz_app_api_t& api, lv_obj_t* root,
                            const AppStoreItem& item);

/** @brief 停止所有回调并释放托管资源，须在释放 ELF 和界面前调用 */
void AppRuntimeServicesClose(AppRuntimeServicesState& state);

/** @brief 返回键交应用内部处理；返回 true 表示已消费，不再执行宿主默认返回 */
bool AppRuntimeServicesHandleBack(AppRuntimeServicesState& state);
