#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <memory>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <mutex>
#include <atomic>
#include <string_view>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>
#include <model_path.h>

#include <opus_encoder.h>
#include <opus_decoder.h>
#include <opus_resampler.h>

#include "audio_codec.h"
#include "audio_processor.h"
#include "processors/audio_debugger.h"
#include "wake_word.h"
#include "protocol.h"


// 音频分两条链路：MIC -> 处理器 -> 编码队列 -> Opus -> 发送队列 -> 服务器，
// 以及服务器 -> 解码队列 -> Opus -> 播放队列 -> 喇叭。
// 其中 MIC/喇叭/处理器共用一组任务，Opus 编解码另用一组任务。

#define OPUS_FRAME_DURATION_MS 60
#define MAX_ENCODE_TASKS_IN_QUEUE 2
#define MAX_PLAYBACK_TASKS_IN_QUEUE 2
// 原 2400ms≈40 包；Opus 包常 <512B 偏内部堆，压到 ~0.9s 减碎片与峰值
#define MAX_DECODE_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define MAX_SEND_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define AUDIO_TESTING_MAX_DURATION_MS 10000
#define MAX_TIMESTAMPS_IN_QUEUE 3

#define AUDIO_POWER_TIMEOUT_MS 15000
#define AUDIO_POWER_CHECK_INTERVAL_MS 1000


#define AS_EVENT_AUDIO_TESTING_RUNNING      (1 << 0)
#define AS_EVENT_WAKE_WORD_RUNNING          (1 << 1)
#define AS_EVENT_AUDIO_PROCESSOR_RUNNING    (1 << 2)
#define AS_EVENT_PLAYBACK_NOT_EMPTY         (1 << 3)
#define AS_EVENT_PCM_CAPTURE_RUNNING        (1 << 7) // 独占直通 PCM 采集
#define AS_EVENT_INPUT_EXITED               (1 << 4)
#define AS_EVENT_OUTPUT_EXITED              (1 << 5)
#define AS_EVENT_OPUS_EXITED                (1 << 6)

struct AudioServiceCallbacks {
    std::function<void(void)> on_send_queue_available;
    std::function<void(const std::string&)> on_wake_word_detected;
    std::function<void(bool)> on_vad_change;
    std::function<void(void)> on_audio_testing_queue_full;
};


enum AudioTaskType {
    kAudioTaskTypeEncodeToSendQueue,
    kAudioTaskTypeEncodeToTestingQueue,
    kAudioTaskTypeDecodeToPlaybackQueue,
};

struct AudioTask {
    AudioTaskType type;
    std::vector<int16_t> pcm;
    uint32_t timestamp = 0;
};

struct DebugStatistics {
    uint32_t input_count = 0;
    uint32_t decode_count = 0;
    uint32_t encode_count = 0;
    uint32_t playback_count = 0;
};

class AudioService {
public:
    AudioService();
    ~AudioService();

    void Initialize(AudioCodec* codec);
    /** @brief 创建音频输入、输出和编解码任务；已启动时不重复创建 */
    void Start();
    /** 拆任务并销毁 AFE（离百问 / reboot / OTA / 开机清理）。 */
    void Stop();
    void EncodeWakeWord();
    std::unique_ptr<AudioStreamPacket> PopWakeWordPacket();
    const std::string& GetLastWakeWord() const;
    bool IsVoiceDetected() const { return voice_detected_; }
    bool IsIdle();
    bool IsWakeWordRunning() const { return xEventGroupGetBits(event_group_) & AS_EVENT_WAKE_WORD_RUNNING; }
    bool IsStarted() const { return !service_stopped_; }
    bool IsAudioProcessorRunning() const { return xEventGroupGetBits(event_group_) & AS_EVENT_AUDIO_PROCESSOR_RUNNING; }
    bool IsAfeWakeWord();

    void EnableWakeWordDetection(bool enable);
    void EnableVoiceProcessing(bool enable);
    void EnableAudioTesting(bool enable);
    void EnableDeviceAec(bool enable);

    void SetCallbacks(AudioServiceCallbacks& callbacks);

    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait = false);
    std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue();
    bool ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples);
    /**
     * @brief 独占采集 16 kHz 单声道直通 PCM，不改变原有语音前处理器；启停须由 Application 任务串行调用。
     * @param callback 音频块回调；数据仅在调用期间有效，返回 false 表示结束采集，不可在回调内调用启停。
     * @param error 可选失败说明，区分占用、未就绪和内存不足。
     * @return 音频服务已启动、麦克风空闲且直通处理器就绪时返回 true。
     */
    bool StartPcmCapture(std::function<bool(const int16_t*, size_t)> callback, std::string* error = nullptr);
    /**
     * @brief 停止 PCM 采集并等待正在执行的数据回调退出。
     * @return 无返回值；返回后可安全释放回调使用的录音缓冲。
     */
    void StopPcmCapture();
    void ResetDecoder();
    /**
     * @brief 清空 PCM 和 Opus 队列并唤醒等待方，保留工作任务
     */
    void FlushQueues();
    /** @brief 解析嵌入式 OGG/Opus 并入队解码播放（老化厂测音效用） */
    void PlaySound(const std::string_view& ogg);
    void SetModelsList(srmodel_list_t* models_list);

private:
    // opus_codec: large stack in PSRAM (wake_word encode pattern); TCB in INTERNAL.
    static constexpr size_t kOpusCodecStackWords = 2048 * 13;
    AudioCodec* codec_ = nullptr;
    bool codec_started_ = false;
    AudioServiceCallbacks callbacks_;
    std::mutex pcm_capture_mutex_; // 保护接收器与采集路由归属
    std::function<bool(const int16_t*, size_t)> pcm_capture_callback_; // 当前独占 PCM 接收器
    std::atomic<uint32_t> pcm_capture_generation_{0}; // 识别停止前已经开始的输入读取
    // 录音停止先封闭回调，再等待本轮 Feed 结束；不可持有回调锁调用处理器 Stop。
    std::mutex pcm_feed_mutex_; // 串行化采集输入与处理器启停
    std::unique_ptr<AudioProcessor> capture_processor_; // 仅在独占采集期间分配的直通处理器
    std::unique_ptr<AudioProcessor> audio_processor_;
    std::unique_ptr<WakeWord> wake_word_;
    std::unique_ptr<AudioDebugger> audio_debugger_;
    std::unique_ptr<OpusEncoderWrapper> opus_encoder_;
    std::unique_ptr<OpusDecoderWrapper> opus_decoder_;
    OpusResampler input_resampler_;
    OpusResampler reference_resampler_;
    OpusResampler output_resampler_;
    DebugStatistics debug_statistics_;
    srmodel_list_t* models_list_ = nullptr;

    EventGroupHandle_t event_group_;

    // Audio encode / decode
    TaskHandle_t audio_input_task_handle_ = nullptr;
    TaskHandle_t audio_output_task_handle_ = nullptr;
    TaskHandle_t opus_codec_task_handle_ = nullptr;
    StackType_t* opus_codec_stack_ = nullptr;
    StaticTask_t* opus_codec_tcb_ = nullptr;
    std::mutex audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_decode_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_send_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_testing_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_encode_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_playback_queue_;
    // For server AEC
    std::deque<uint32_t> timestamp_queue_;

    bool wake_word_initialized_ = false;
    bool audio_processor_initialized_ = false;
    bool voice_detected_ = false;
    std::atomic<bool> service_stopped_{true};
    bool audio_input_need_warmup_ = false;

    esp_timer_handle_t audio_power_timer_ = nullptr;
    std::chrono::steady_clock::time_point last_input_time_;
    std::chrono::steady_clock::time_point last_output_time_;

    /** @brief 分发独占采集的单声道音频帧 */
    void HandleCapturedAudio(std::vector<int16_t>&& data);
    /** @brief 通知当前采集输入失败 */
    void FailPcmCapture(uint32_t generation);
    void AudioInputTask();
    void ParkStoppedTask(EventBits_t bit);
    void AudioOutputTask();
    void OpusCodecTask();
    void PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm);
    void SetDecodeSampleRate(int sample_rate, int frame_duration);
    void CheckAndUpdateAudioPowerState();
};

#endif
