#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <esp_timer.h>
#include "chat_activation.h"
#include "conversation_requests.h"
#include "business/conversation/yunxin/yunxin_text_inbox.h"
#include "infra/protocols/protocol.h"
#include "sysmgmt/device_state.h"

enum class ImageQuestionResult {
    Sent,      // 问题已交付当前会话
    Replaced,  // 问题所属会话已失效
    Failed,    // 当前会话发送失败
};

class ConversationController {
public:
    /** @brief 获取对话会话的唯一所有者 */
    static ConversationController& GetInstance();

    /** @brief 在创建界面前绑定系统唤醒通知，资源创建失败返回 false */
    bool Initialize(void (*wake)());

    /** @brief 由系统分发器交付合并的会话请求，队列满时保持待处理标志 */
    void Poll();

    /** @brief 核心启动完成后兑现仍有效的页面请求 */
    void OnSystemReady();

    /** @brief 在内部 RAM 网络或启动线程刷新云配置并通知当前会话 */
    void RefreshConfiguration();

    /** @brief 查询云端配置是否可用于建立协议 */
    bool IsConfigured() const {
        return activation_.IsReady();
    }

    /** @brief 请求建立会话；重试保留聆听参数和代次 */
    void StartSession(bool enter_listening = false);

    /** @brief 使旧会话及照片请求失效，并请求串行收尾 */
    void StopSession();

    /** @brief 请求开始聆听，不在调用线程访问协议 */
    void StartListening();

    /** @brief 请求停止聆听，队列满时保留停止责任 */
    void StopListening();

    /** @brief 请求停止播报并回到空闲状态 */
    void AbortSpeakingToIdle();

    /** @brief 保存播报开关；会话线程返回提交结果，其他线程返回请求是否成功接纳 */
    bool SetLocalTtsEnabled(bool enabled);

    /** @brief 请求准备单张照片问题；返回是否成功接纳 */
    bool AskImage();

    /**
     * @brief 请求提交或取消当前照片问题
     * @note completed 在会话线程执行；入队失败时不调用，接纳后的失效请求返回 Replaced。
     */
    bool CompleteQuestion(uint32_t turn, bool recorded, const std::string& image_id,
                          std::function<void(ImageQuestionResult)> completed = {});

    /** @brief 将 MCP 响应发送到仍有效的会话 */
    void SendMcpMessage(const std::string& payload);

    /** @brief 升级或重启前同步收尾，仅由语音控制线程或未就绪的启动线程调用 */
    bool CloseForSystem();

private:
    enum Event : uint32_t {
        MAIN_EVENT_SEND_AUDIO = 1 << 1,        // 编码输出待发送
        MAIN_EVENT_ERROR = 1 << 4,             // 会话错误待处理
        MAIN_EVENT_STOP_VOICE = 1 << 8,        // 旧会话待收尾
        MAIN_EVENT_STOP_LISTENING = 1 << 9,    // 停止聆听意图
        MAIN_EVENT_PREPARE_VOICE = 1 << 10,    // 包含参数的准备请求
        MAIN_EVENT_START_LISTENING = 1 << 11,  // 开始聆听意图
        MAIN_EVENT_ABORT_SPEAKING = 1 << 12,   // 打断播报意图
    };
    ConversationController() = default;
    void PostEvents(uint32_t bits);
    void DispatchVoiceEvents();
    void StartSessionOnTask(bool enter_listening, uint32_t generation);
    void StopSessionOnTask();
    void StartListeningOnTask();
    void StopListeningOnTask();
    void AbortSpeakingOnTask();
    void AbortSpeaking();
    bool ApplyLocalTtsEnabled(bool enabled);
    bool RejectUnactivatedListening();
    void OnAudioModeReady(uint32_t generation, bool ready);
    void FlushPendingXiaozhiVoice();
    void EnsureProtocolReady();
    void TeardownProtocol();
    bool EnterContinuousListening();
    void SetListeningMode(ListeningMode mode);
    void SetDeviceState(DeviceState state);
    bool EnsureNetworkReady(const std::function<bool()>& still_needed = {});
    void ReceiveYunxinText(const char* text);
    void QueueYunxinTextUpdate();
    void QueueYunxinTextBoundary(std::function<void()> after);
    void ApplyYunxinText(const YunxinTextInbox::Snapshot& snapshot);

    ChatActivation activation_;           // 配置请求与绑定状态
    std::unique_ptr<Protocol> protocol_;  // 仅会话控制线程创建、访问及释放
    std::mutex error_mutex_;              // 保护网络回调交付的错误快照
    std::string last_error_message_;
    uint32_t last_error_generation_ = 0;  // 错误所属会话
    void (*wake_)() = nullptr;            // 启动时绑定，运行期间不更换
    esp_timer_handle_t yunxin_text_timer_handle_ = nullptr;
    YunxinTextInbox yunxin_text_inbox_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    struct PhotoQuestion {
        uint32_t turn = 0;          // 单个录音或照片问题的代次
        std::string image_id;       // 当前已提交图片标识
        bool stop_waiting = false;  // 已松手，等待上传后提交结束消息
    } question_;
    ConversationRequests requests_;                     // 启动、聆听参数与取消代次的复合所有者
    std::atomic<bool> audio_mode_wait_pending_{false};  // 合并硬件就绪等待
    uint32_t voice_session_generation_ = 0;             // 当前协议所属会话，控制线程访问
    std::atomic<uint32_t> voice_events_{0};             // 合并待处理命令
    std::atomic<bool> voice_events_queued_{false};      // 合并队列投递
};
