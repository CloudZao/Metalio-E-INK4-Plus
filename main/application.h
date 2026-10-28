#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <memory>
#include <atomic>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state_event.h"
#include "yunxin_text_inbox.h"


#define MAIN_EVENT_SEND_AUDIO (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED (1 << 2)
#define MAIN_EVENT_VAD_CHANGE (1 << 3)
#define MAIN_EVENT_ERROR (1 << 4)
#define MAIN_EVENT_CHECK_NEW_VERSION_DONE (1 << 5)
#define MAIN_EVENT_CLOCK_TICK (1 << 6)
#define MAIN_EVENT_RELEASE_AUDIO (1 << 7) // 重试共享音频释放请求
#define MAIN_EVENT_STOP_VOICE (1 << 8) // 重试语音会话停止请求
#define MAIN_EVENT_STOP_LISTENING (1 << 9) // 重试停止聆听请求
#define MAIN_EVENT_PREPARE_VOICE (1 << 10) // 重试语音会话准备请求


enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // 删除拷贝构造函数和赋值运算符
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Start();
    void MainEventLoop();
    DeviceState GetDeviceState() const { return device_state_; }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    enum class TaskDomain : uint8_t {
        Voice, // 协议和语音会话状态，保持串行
        Network, // 联网、激活及固件请求
        Storage, // 内部 RAM 栈上的存储操作
        Audio, // 本地录音服务的申请、启动和停止
        Cleanup, // 网络取消及资源回收
    };
    /** @brief 投递业务任务，不等待业务执行或队列空位；失败返回 false，捕获数据随回调释放 */
    bool Schedule(std::function<void()> callback, TaskDomain domain = TaskDomain::Voice);
    void SetDeviceState(DeviceState state);
    void Alert(const char* status, const char* message, const char* emotion = "");
    void DismissAlert();
    void AbortSpeaking(AbortReason reason);
    // 百问 AI BOOT 页内分工：StartListening/StopListening/AbortSpeakingToIdle。
    void StartListening();
    void StopListening();
    void AbortSpeakingToIdle();
    void Reboot();
    /** @brief 由语音任务保存当前云服务的播报开关，并在静音时清理待播语音 */
    void SetLocalChatTtsEnabled(bool enabled);
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(Ota& ota, const std::string& url = "");
    bool UpgradeFirmwareUrl(const std::string& url, const std::string& current_version,
                            const std::string& new_version);
    void SendMcpMessage(const std::string& payload);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    AudioService& GetAudioService() { return audio_service_; }

    // 屏级语音会话：进页激活，离页关闭音频与协议，归还任务栈占用。
    void StartXiaozhiVoice(bool enter_listening = false);
    void StopXiaozhiVoice();
    bool IsXiaozhiVoiceActive() const { return xiaozhi_voice_active_; }
    // 开机主流程是否已结束（联网/OTA 等）。
    bool IsBootReady() const { return boot_ready_.load(std::memory_order_acquire); }
    bool IsBootWorkActive() const { return boot_work_active_.load(std::memory_order_acquire); }

    // 按需启动音频服务；不启动小智语音会话。
    void EnsureAudioServiceRunning();
    enum class AudioClient : uint32_t {
        Assistant = 1, // AI 语音会话
        FactoryTest = 2, // 录音测试
        AgingTest = 4, // 老化测试
        WorkBuddy = 8, // WorkBuddy 独占录音
    };
    /** @brief 联网应用是否仍持有或正在归还音频资源；供待机任务等待硬件收尾 */
    bool HasNetworkAudioWork() const {
        constexpr uint32_t clients = static_cast<uint32_t>(AudioClient::Assistant) |
                                     static_cast<uint32_t>(AudioClient::WorkBuddy);
        return (audio_clients_.load(std::memory_order_acquire) & clients) != 0;
    }
    /** @brief 申请共享音频服务，可通过回调取消已失效的页面请求 */
    bool AcquireAudioService(AudioClient client, std::function<bool()> still_needed = {});
    /** @brief 归还调用方的音频占用，最后一个调用方退出时释放资源 */
    void ReleaseAudioService(AudioClient client);
    /** @brief 非阻塞请求归还音频占用；队列暂满时保留请求并由调度节拍重试 */
    void RequestAudioServiceRelease(AudioClient client);
    // 关闭按需拉起的音频任务；百问会话路径走 StopXiaozhiVoice。
    void StopAudioServiceIfRunning();

    // 待机浅睡唤醒后：恢复激活 UI 并继续 CheckVersion/Activate。
    void ResumeActivationAfterStandby();
    // 仍有未绑定验证码时，Idle 顶栏显示验证码而非时钟。
    /** @brief 是否仍有待绑定验证码 */
    bool HasPendingActivationCode() const;
    /** @brief 获取可跨任务使用的验证码副本 */
    std::string GetPendingActivationCode() const;

    /**
     * @brief 暂停激活轮询（日志上传、传输热点或 U 盘）
     * @note 与 ResumeActivationPolling 成对；暂停期间不发起 Activate/CheckVersion HTTP
     */
    void PauseActivationPolling();
    bool IsActivationPollingBusy();
    /** @brief 传输热点或 U 盘占用期间为 true */
    bool IsActivationPollingPaused() const {
        return activation_paused_.load(std::memory_order_acquire);
    }
    /** @brief 恢复激活轮询；网络回来后由时钟节拍继续 */
    void ResumeActivationPolling();

private:
    Application();
    ~Application();
    // Start() safe points: pause startup work while standby owns hardware.
    void BootStandbyCheckpoint();

    std::mutex activation_poll_mutex_;
    unsigned activation_pause_count_ = 0;
    bool activation_poll_busy_ = false;
    mutable std::mutex activation_state_mutex_; // 跨域文案、验证码和版本快照
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    esp_timer_handle_t yunxin_text_timer_handle_ = nullptr;
    YunxinTextInbox yunxin_text_inbox_;
    std::atomic<DeviceState> device_state_{kDeviceStateUnknown};
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    std::atomic<AecMode> aec_mode_{kAecOff};
    std::string last_error_message_;
    uint32_t last_error_generation_ = 0; // activation_state_mutex_ 保护，错误所属语音会话
    AudioService audio_service_;

    bool has_server_time_ = false;
    std::string current_version_;
    bool audio_initialized_ = false;
    std::recursive_mutex audio_service_mutex_; // 串行化服务启动、占用和释放
    std::atomic<uint32_t> audio_clients_{0}; // 当前音频调用方位掩码，联网应用收尾前保持置位
    std::atomic<bool> audio_running_{false};
    bool voice_models_ready_ = false;
    bool protocol_ready_ = false;
    std::atomic<bool> chat_config_loading_{false};
    std::atomic<bool> chat_config_ready_{false};
    std::atomic<bool> boot_version_checked_{false};
    std::atomic<bool> chat_activation_pending_{false};
    std::atomic<bool> xiaozhi_voice_active_{false};
    bool aborted_ = false;
    std::atomic<bool> boot_ready_{false}; // Start() 跑完后置 true
    std::atomic<bool> boot_work_active_{true};
    // 开机未完成时进百问：记录意图，boot_ready 后自动 StartXiaozhiVoice。
    std::atomic<bool> pending_xiaozhi_voice_{false};
    std::atomic<bool> pending_xiaozhi_listen_{false};
    std::atomic<bool> audio_mode_wait_pending_{false}; // 已安排后台等待音频模式就绪
    std::atomic<uint32_t> voice_generation_{0}; // 离页或待机时失效旧启动和音频就绪回调
    uint32_t voice_session_generation_ = 0; // 仅语音任务访问，防止迟到停止关闭新会话
    std::atomic<int> clock_ticks_{0};
    std::atomic<EventBits_t> voice_events_{0}; // 待语音任务处理的合并事件
    std::atomic<bool> voice_events_queued_{false}; // 同时只投递一个语音事件处理器
    std::atomic<bool> activation_work_queued_{false}; // 激活任务在途标记
    std::atomic<bool> status_work_queued_{false}; // 状态栏更新合并标记
    std::atomic<bool> storage_retry_queued_{false}; // Wi-Fi 配置持久化重试标记
    std::atomic<uint32_t> audio_release_pending_{0}; // 未交付的音频调用方释放请求
    std::atomic<bool> audio_release_queued_{false}; // 同时只投递一个音频释放处理器
    std::string activation_code_;
    std::atomic<bool> activation_kick_{false};
    std::atomic<bool> activation_paused_{false}; // 日志上传等：挡 Activate/CheckVersion

    void DispatchVoiceEvents();
    void DispatchClockTick();
    void DispatchAudioReleases();
    void ReleaseAudioClients(uint32_t clients);
    bool IsVoiceTask() const;
    void StartXiaozhiVoiceOnTask(bool enter_listening, uint32_t generation);
    /** @brief 拦截未激活设备的聆听请求并显示提示 */
    bool RejectUnactivatedListening();
    void StopXiaozhiVoiceOnTask();
    void OnAudioModeReady(uint32_t generation, bool ready);
    void ClearActivationCode();
    void RestoreActivationStatus();

    void OnWakeWordDetected();
    void CheckNewVersion(Ota& ota);
    /** @brief 开机检测 SD 根目录固件并优先升级；成功则重启，取消/失败返回 false */
    bool TrySdCardFirmwareUpgrade();
    void CheckAssetsVersion();
    void ShowActivationCode(const std::string& code);
    void SetListeningMode(ListeningMode mode);
    // 屏级会话：打开音频通道并进入 AutoStop/Realtime 聆听；失败会保持 Idle。
    bool EnterContinuousListening();
    // Idle 状态栏：允许时钟页面（含百问）刷 HH:MM；固定文案页改用 GetIdleStatusFixedText。
    void ApplyIdleStatusBar();
    // 可中断延迟；Idle 时返回 false，activation_kick 时返回 true；paused 时原地等待。
    bool DelayMsInterruptible(int ms);
    /** @brief Activating 且 paused 时阻塞等待恢复（或变 Idle） */
    void WaitWhileActivationPaused();

    void EnsureVoiceModelsReady();
    void ReceiveYunxinText(const char* text);
    void QueueYunxinTextUpdate();
    void QueueYunxinTextBoundary(std::function<void()> after);
    void ApplyYunxinText(const YunxinTextInbox::Snapshot& snapshot);
    void EnsureProtocolReady();
    void CheckChatActivation();
    void TeardownProtocol();
    // 开机完成后：若仍在百问页，则兑现挂起的进页/聆听意图。
    void FlushPendingXiaozhiVoice();
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
