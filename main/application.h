#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>
#include <atomic>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state_event.h"


#define MAIN_EVENT_SCHEDULE (1 << 0)
#define MAIN_EVENT_SEND_AUDIO (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED (1 << 2)
#define MAIN_EVENT_VAD_CHANGE (1 << 3)
#define MAIN_EVENT_ERROR (1 << 4)
#define MAIN_EVENT_CHECK_NEW_VERSION_DONE (1 << 5)
#define MAIN_EVENT_CLOCK_TICK (1 << 6)


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
    void Schedule(std::function<void()> callback);
    // 仅在主循环任务内调用：清空已排队的 MainEventLoop 任务，避免占死主循环。
    void DrainScheduledTasks();
    void SetDeviceState(DeviceState state);
    void Alert(const char* status, const char* message, const char* emotion = "");
    void DismissAlert();
    void AbortSpeaking(AbortReason reason);
    // 百问 AI BOOT 页内分工：StartListening/StopListening/AbortSpeakingToIdle。
    void StartListening();
    void StopListening();
    void AbortSpeakingToIdle();
    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(Ota& ota, const std::string& url = "");
    bool UpgradeFirmwareUrl(const std::string& url, const std::string& current_version,
                            const std::string& new_version);
    bool CanEnterSleepMode();
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

    // 按需启动音频服务；不启动小智语音会话。
    void EnsureAudioServiceRunning();
    // 关闭按需拉起的音频任务；百问会话路径走 StopXiaozhiVoice。
    void StopAudioServiceIfRunning();

    // 待机浅睡唤醒后：恢复激活 UI 并继续 CheckVersion/Activate。
    void ResumeActivationAfterStandby();
    // 仍有未绑定验证码时，Idle 顶栏显示验证码而非时钟。
    bool HasPendingActivationCode() const { return !activation_code_.empty(); }

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    volatile DeviceState device_state_ = kDeviceStateUnknown;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;

    bool has_server_time_ = false;
    std::string current_version_;
    bool audio_initialized_ = false;
    bool audio_running_ = false;
    bool voice_models_ready_ = false;
    bool protocol_ready_ = false;
    bool xiaozhi_voice_active_ = false;
    bool aborted_ = false;
    std::atomic<bool> boot_ready_{false}; // Start() 跑完后置 true
    // 开机未完成时进百问：记录意图，boot_ready 后自动 StartXiaozhiVoice。
    std::atomic<bool> pending_xiaozhi_voice_{false};
    std::atomic<bool> pending_xiaozhi_listen_{false};
    int clock_ticks_ = 0;
    std::string activation_code_;
    std::string activation_message_;
    std::atomic<bool> activation_kick_{false};
    TaskHandle_t check_new_version_task_handle_ = nullptr;
    TaskHandle_t main_event_loop_task_handle_ = nullptr;

    void OnWakeWordDetected();
    void CheckNewVersion(Ota& ota);
    /** @brief 开机检测 SD 根目录固件并优先升级；成功则重启，取消/失败返回 false */
    bool TrySdCardFirmwareUpgrade();
    void CheckAssetsVersion();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    // 屏级会话：打开音频通道并进入 AutoStop/Realtime 聆听；失败会保持 Idle。
    bool EnterContinuousListening();
    // Idle 状态栏：允许时钟页面（含百问）刷 HH:MM；固定文案页改用 GetIdleStatusFixedText。
    void ApplyIdleStatusBar();
    // 可中断延迟；Idle 时返回 false，activation_kick 时返回 true。
    bool DelayMsInterruptible(int ms);

    void EnsureVoiceModelsReady();
    void EnsureProtocolReady();
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
