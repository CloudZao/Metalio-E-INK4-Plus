#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include "assets/lang_workbuddy.h"
#include "business/workbuddy/workbuddy_audio.h"
#include "business/workbuddy/workbuddy_client.h"
#include "business/workbuddy/workbuddy_voice_stream.h"
#include "sysmgmt/power/policy/power_policy.h"

enum class WorkBuddyPhase {
    Preparing,    // 准备实时语音连接
    Recording,    // 采集并上传音频
    Recognizing,  // 等待最终识别结果
    Sending,      // 提交指令或读取消息
    Waiting,      // 等待电脑回复
    Done,         // 当前结果可供阅读
    Error,        // 本轮失败
    Idle          // 尚无请求
};
enum class WorkBuddyAuthPhase {
    Checking,     // 查询绑定或创建二维码
    Waiting,      // 等待手机扫码
    Authorizing,  // 手机已提交授权
    Authorized,   // 设备绑定已确认
    Expired,      // 二维码已过期
    Error         // 授权请求失败
};

enum class WorkBuddyConnectionState {
    Unknown,   // 尚未获得连接状态
    Querying,  // 正在查询授权或助理连接
    Failed,    // 网络、授权或响应校验失败
    Online,    // 服务明确报告助理在线
    Offline    // 服务明确报告助理离线
};

struct WorkBuddyAuthorization {
    DownloadGate gate;                                                                // 请求取消与 HTTP 绑定门闩
    std::atomic<WorkBuddyAuthPhase> phase{WorkBuddyAuthPhase::Checking};  // 当前处理阶段
    std::atomic<bool> refresh_requested{false};                                       // 请求刷新当前扫码会话
    std::atomic<bool> switch_account{false};                                          // 本轮是否允许更换绑定账号
    std::atomic<bool> running{false};                                                 // 授权检查或扫码是否仍在运行
    std::atomic<int> expires_in{600};                                                 // 当前扫码会话的有效秒数
    std::atomic<WorkBuddyConnectionState> connection{
        WorkBuddyConnectionState::Unknown};       // 当前连接查询状态
    std::atomic<bool> binding_changed{false};           // 扫码成功后需要清空旧账号消息
    std::atomic<bool> reauthorization_required{false};  // 当前凭证或权限需要重新绑定
    std::mutex view_mutex;                              // 保护授权显示快照
    uint32_t revision = 1;                              // 授权显示快照版本
    std::string verification_url;                       // 用于二维码显示的手机授权地址
    std::string message = LangString_WB_AUTH_CHECKING;  // 可直接显示的授权状态说明
};

/** @brief 一轮对话的共享状态；音频和网络任务不持有任何 LVGL 对象。 */
struct WorkBuddyTurn {
    std::function<bool()> input_held; // 入口提供即时按压快照，不持有页面对象
    uint32_t ui_id = 0;                                                        // 页面内的对话轮次，仅由 LVGL 线程设置
    bool is_local_refresh = false;                                             // 是否仅手动读取助理消息
    std::atomic<WorkBuddyPhase> phase{WorkBuddyPhase::Preparing};  // 当前处理阶段
    std::atomic<uint32_t> result_revision{0};                                  // 识别或回复结果的版本
    std::atomic<bool> local_accepted{false};                                   // 电脑是否已受理本轮指令
    std::atomic<bool> watch_running{false};                                    // 是否仍在自动读取回复
    std::atomic<bool> held{true};                                              // 屏触或 BOOT 是否保持录音意图
    std::atomic<bool> finish_requested{false};                                 // 采集端请求结束本轮录音
    std::atomic<bool> finish_scheduled{false};                                 // 是否已经安排串行收尾
    std::atomic<size_t> samples{0};  // 已接收的单声道采样数，仅累计到最短录音门槛
    DownloadGate gate;               // 请求取消与 HTTP 绑定门闩
    // 音频任务先创建、再启动网络任务，此后只读该引用，录音数据由内部有界队列承载。
    std::shared_ptr<WorkBuddyVoiceStream> stream;  // 本轮实时语音连接及有界缓冲
    std::atomic<bool> stream_running{false};       // 识别工作任务是否仍使用音频缓冲
    std::atomic<bool> transcript_final{false};     // 识别文字是否已最终确认
    std::mutex result_mutex;                       // 保护识别和回复结果
    WorkBuddyTurnResult result;                    // 当前识别或电脑请求结果
    WorkBuddyAudioCapture audio;                   // 仅由音频任务启停的音频占用
};


/** @brief 发布授权快照，不访问界面。 */
void WorkBuddySetAuthorizationView(const std::shared_ptr<WorkBuddyAuthorization>& authorization,
                                  WorkBuddyAuthPhase phase, const std::string& message,
                                  const std::string& url = {});

/** @brief 接纳独立授权会话，返回未接纳时的建议重试间隔，单位毫秒。 */
int WorkBuddyStartAuthorization(const std::shared_ptr<WorkBuddyAuthorization>& authorization);

/** @brief 启动实时识别及一次文字提交，接纳失败写入请求结果。 */
void WorkBuddyBeginTurn(const std::shared_ptr<WorkBuddyTurn>& turn);

/** @brief 读取电脑消息，不发送新指令。 */
void WorkBuddyReadMessages(const std::shared_ptr<WorkBuddyTurn>& turn);

/** @brief 发布一轮请求的失败结果。 */
void WorkBuddyFailTurn(const std::shared_ptr<WorkBuddyTurn>& turn, const char* message);

/** @brief 停止采集并发送最终识别标记。 */
void WorkBuddyFinishTurn(const std::shared_ptr<WorkBuddyTurn>& turn);

/** @brief 取消授权并在后台解除网络占用。 */
void WorkBuddyCancelAuthorization(const std::shared_ptr<WorkBuddyAuthorization>& authorization);

/** @brief 取消对话，业务任务负责采音、网络和缓冲收尾。 */
void WorkBuddyCancelTurn(const std::shared_ptr<WorkBuddyTurn>& turn);
