#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <opus_decoder.h>
#include <opus_encoder.h>
#include <opus_resampler.h>
#include "processors/pcm_audio_processor.h"
#include "boards/common/audio/audio_codec.h"
#include "infra/audio/audio_stream_packet.h"

// 音频分两条链路：MIC -> 处理器 -> 编码队列 -> Opus -> 发送队列 -> 服务器，
// 以及服务器 -> 解码队列 -> Opus -> 播放队列 -> 喇叭。
// 其中 MIC/喇叭/处理器共用一组任务，Opus 编解码另用一组任务。

#define MAX_ENCODE_TASKS_IN_QUEUE 2
#define MAX_PLAYBACK_TASKS_IN_QUEUE 2
#define MAX_DECODE_PACKETS_IN_QUEUE (2400 / kOpusFrameDurationMs)
#define MAX_SEND_PACKETS_IN_QUEUE (2400 / kOpusFrameDurationMs)
#define AUDIO_TESTING_MAX_DURATION_MS 10000

#define AUDIO_POWER_TIMEOUT_MS 15000
#define AUDIO_POWER_CHECK_INTERVAL_MS 1000

#define AS_EVENT_AUDIO_TESTING_RUNNING (1 << 0)
#define AS_EVENT_VOICE_CAPTURE_RUNNING (1 << 2)
#define AS_EVENT_PLAYBACK_NOT_EMPTY (1 << 3)
#define AS_EVENT_OPUS_CAPTURE_RUNNING (1 << 7)  // 独占 Opus 采集
#define AS_EVENT_INPUT_EXITED (1 << 4)
#define AS_EVENT_OUTPUT_EXITED (1 << 5)
#define AS_EVENT_OPUS_EXITED (1 << 6)

struct AudioServiceCallbacks {
    std::function<void(void)> on_send_queue_available;  // 编码包可从发送队列取出时通知
};

enum AudioTaskType {
    kAudioTaskTypeEncodeToSendQueue,      // 编码后进入语音协议发送队列。
    kAudioTaskTypeEncodeToTestingQueue,   // 编码后保存到音频测试队列。
    kAudioTaskTypeEncodeToCapture,        // 共享编码任务输出至独占 Opus 接收器。
    kAudioTaskTypeDecodeToPlaybackQueue,  // 解码后进入播放队列。
};

struct AudioTask {
    AudioTaskType type;               // 处理方式及输出归属。
    uint32_t send_generation = 0;     // 所属发送队列代号，清队列后丢弃迟到编码结果。
    uint32_t capture_generation = 0;  // 独占 Opus 采集轮次，避免迟到结果串轮。
    std::vector<int16_t> pcm;         // 待处理的 PCM 采样。
};

struct DebugStatistics {
    uint32_t input_count = 0;     // 已完成的输入读取次数
    uint32_t decode_count = 0;    // 已处理的解码包数
    uint32_t encode_count = 0;    // 已完成的编码包数
    uint32_t playback_count = 0;  // 已提交的播放块数
};

class AudioService {
public:
    /** @brief 创建音频服务，工作资源由 Initialize 分配 */
    AudioService();

    /** @brief 停止工作任务并释放音频资源 */
    ~AudioService();

    /** @brief 初始化音频运行资源；创建失败时返回 false，不允许启动工作线程 */
    bool Initialize(AudioCodec* codec);

    /** @brief 创建音频输入、输出和编解码任务；已启动时不重复创建 */
    void Start();

    /** @brief 停止采集和工作任务，等待队列处理退出后释放硬件资源 */
    void Stop();

    /** @brief 查询编解码、播放与测试队列是否均已排空 */
    bool IsIdle();

    /** @brief 查询音频工作任务是否已启动 */
    bool IsStarted() const {
        return !service_stopped_;
    }

    /** @brief 查询对话 PCM 采集是否启用 */
    bool IsVoiceCaptureRunning() const {
        return event_group_ != nullptr && (xEventGroupGetBits(event_group_) & AS_EVENT_VOICE_CAPTURE_RUNNING);
    }

    /** @brief 启停对话 PCM 采集及编码投递 */
    void EnableVoiceCapture(bool enable);

    /** @brief 启停录音测试，停止时将测试录音转入播放队列 */
    void EnableAudioTesting(bool enable);

    /** @brief 设置发送队列通知，须在启动音频任务前调用 */
    void SetCallbacks(AudioServiceCallbacks& callbacks);

    /** @brief 校验并接收 Opus 包；服务停止、参数无效、内存不足或非等待队列已满时返回 false */
    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait = false);

    /** @brief 取出一个已编码音频包，发送队列为空时返回空指针 */
    std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue();

    /** @brief 读取不超过 120 毫秒的输入音频；支持硬件原采样率或 16 kHz，保留输入通道布局 */
    bool ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples);

    /**
     * @brief 独占采集 16 kHz 单声道音频，通过共享 Opus 编码任务输出；启停由 AudioRuntime 的音频任务串行调用。
     * @param callback 完整 Opus 包、包字节数和对应采样数；数据仅在回调期间有效，空包表示采音或编码失败。回调返回 false 停止，不可在回调中调用启停。
     * @param error 可选失败说明，区分占用、未就绪和内存不足。
     * @return 音频服务已启动、麦克风空闲且直通处理器就绪时返回 true。
     */
    bool StartOpusCapture(std::function<bool(const uint8_t*, size_t, size_t)> callback, std::string* error = nullptr);

    /**
     * @brief 停止采音，等待已排队及正在执行的 Opus 编码和回调完成。
     * @return 无返回值；返回后可安全释放回调使用的录音缓冲。
     */
    void StopOpusCapture();

    /** @brief 重置解码状态并清空解码、播放和测试队列 */
    void ResetDecoder();

    /**
     * @brief 清空 PCM 和 Opus 队列并唤醒等待方，保留工作任务
     */
    void FlushQueues();

    /** @brief 解析嵌入式 OGG/Opus 并入队解码播放（老化厂测音效用） */
    void PlaySound(const std::string_view& ogg);

private:
    // opus_codec: large stack in PSRAM; TCB in INTERNAL.
    static constexpr size_t kOpusCodecStackWords = 2048 * 13;
    AudioCodec* codec_ = nullptr;
    bool codec_started_ = false;
    AudioServiceCallbacks callbacks_;
    std::mutex opus_capture_mutex_;                                              // 保护接收器与采集路由归属
    std::function<bool(const uint8_t*, size_t, size_t)> opus_capture_callback_;  // 当前独占 Opus 包接收器
    std::atomic<uint32_t> opus_capture_generation_{0};                           // 识别停止前已经开始的输入读取
    // 停止先封闭输入，再等待编码及回调排空；回调锁不能覆盖 Feed 或队列等待。
    std::mutex capture_feed_mutex_;                         // 串行化采集输入与处理器启停
    bool capture_encoding_ = false;                         // 由音频队列锁保护，标识正在执行的独占编码。
    std::unique_ptr<PcmAudioProcessor> capture_processor_;  // 仅在独占采集期间分配的直通处理器
    std::unique_ptr<PcmAudioProcessor> audio_processor_;
    std::unique_ptr<OpusEncoderWrapper> opus_encoder_;
    std::unique_ptr<OpusDecoderWrapper> opus_decoder_;
    OpusResampler input_resampler_;
    OpusResampler reference_resampler_;
    OpusResampler output_resampler_;
    DebugStatistics debug_statistics_;
    uint32_t rejected_decode_packets_ = 0;           // 解码入队参数错误的限频计数，由队列锁保护
    uint32_t decode_queue_allocation_failures_ = 0;  // 解码入队内存不足的限频计数，由队列锁保护

    EventGroupHandle_t event_group_ = nullptr;  // 初始化成功后才用于线程协作

    // Audio encode / decode
    TaskHandle_t audio_input_task_handle_ = nullptr;
    TaskHandle_t audio_output_task_handle_ = nullptr;
    TaskHandle_t opus_codec_task_handle_ = nullptr;
    StackType_t* opus_codec_stack_ = nullptr;
    StaticTask_t* opus_codec_tcb_ = nullptr;
    std::mutex audio_queue_mutex_;
    uint32_t send_generation_ = 0;  // 由 audio_queue_mutex_ 保护。
    std::condition_variable audio_queue_cv_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_decode_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_send_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_testing_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_encode_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_playback_queue_;

    bool audio_processor_initialized_ = false;
    std::atomic<bool> service_stopped_{true};
    std::atomic<bool> audio_input_need_warmup_{false};  // 跨音频控制任务与输入任务交接预热请求

    esp_timer_handle_t audio_power_timer_ = nullptr;
    std::atomic<int64_t> last_input_time_us_{0};   // 最近一次输入完成时间，供电源定时器读取
    std::atomic<int64_t> last_output_time_us_{0};  // 最近一次输出完成时间，供电源定时器读取

    /** @brief 分发独占采集的单声道音频帧 */
    void HandleCapturedAudio(std::vector<int16_t>&& data);

    /** @brief 通知当前采集输入失败 */
    void FailOpusCapture(uint32_t generation);

    /** @brief 在共享编解码任务分发完整 Opus 包，空包表示本轮失败 */
    void HandleCapturedOpus(const std::vector<uint8_t>& packet, size_t samples, uint32_t generation);
    void AudioInputTask();
    void ParkStoppedTask(EventBits_t bit);
    void AudioOutputTask();
    void OpusCodecTask();
    void PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm);
    bool SetDecodeSampleRate(int sample_rate, int frame_duration);
    void CheckAndUpdateAudioPowerState();
};

#endif
