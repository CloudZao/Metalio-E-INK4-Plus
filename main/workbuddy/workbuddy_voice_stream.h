#pragma once

#include "workbuddy_client.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace workbuddy {

/** @brief 独占一轮实时识别；录音回调仅写有界环形缓冲，所有网络操作在工作任务执行。 */
class VoiceStream {
public:
    /** @brief 创建独立实时语音会话；@param error 配置、凭证或内存错误；@return 使用本机已保存凭证创建的会话，失败返回空。 */
    static std::shared_ptr<VoiceStream> Create(std::string& error);
    /** @brief 释放尚未发送的音频缓冲；@return 无，调用方须先停止录音回调和网络任务。 */
    ~VoiceStream();
    /**
     * @brief 将本帧音频完整写入有界发送缓冲。
     * @param pcm 本帧单声道采样。
     * @param samples 采样数。
     * @param error 失败原因；正常取消或输入结束时为空，不覆盖网络任务的结果。
     * @return 成功返回true；false表示应停止采音。
     */
    bool PushPcm(const int16_t* pcm, size_t samples, const char*& error);
    /** @brief 停止录音回调后通知网络任务排空缓冲并发送结束帧；@return 无，不执行网络等待。 */
    void FinishInput();
    /** @brief 请求网络工作任务取消，不在UI/录音回调中关闭套接字；@return 无。 */
    void Cancel();
    /**
     * @brief 回收本轮音频缓存，让等待电脑回复和阅读历史不再持有录音内存。
     * @return 无；只能在Recognize返回且录音回调停止后，或工作任务创建失败时调用；允许重复调用。
     */
    void ReleaseAudioBuffer();
    /**
     * @brief 连通Java实时ASR并持续转发采样，最终结果返回后由调用方另行提交一次电脑指令。
     * @param gate 页面取消门闩。
     * @param on_ready 上游就绪后通知Application任务启动麦克风。
     * @param on_text 最新全文和是否最终确认，回调不得操作LVGL。
     * @return 最终识别文字或明确错误，断线不会使用临时文字代替最终结果。
     */
    TurnResult Recognize(DownloadGate& gate, const std::function<void()>& on_ready,
                         const std::function<void(const std::string&, bool)>& on_text);

private:
    /** @param url 实时入口；@param token 本设备秘密，仅放在请求头。 */
    VoiceStream(std::string url, std::string token);
    /** @brief 输出限频采音与发送快照，日志输出不持有音频队列锁。 */
    void LogAudioState(const char* event, bool force = false);
    static constexpr size_t kCapacity = 64000; // 待发音频队列容量，字节
    static constexpr size_t kFrameBytes = 5120; // 单次发送暂存区容量，字节
    std::string url_; // 实时语音服务地址
    std::string token_; // 当前设备凭证，仅用于请求头
    uint8_t* ring_ = nullptr; // PSRAM 中的音频环形队列和发送区
    std::mutex mutex_; // 保护音频队列与结束状态
    size_t read_ = 0; // 环形队列下一次读取位置
    size_t used_ = 0; // 队列内尚未发送的字节数
    uint64_t bytes_ = 0; // 本轮累计接收的音频字节数，独立于有界队列容量
    uint64_t sent_bytes_ = 0; // 已成功返回发送调用的 PCM 字节数，不代表服务端确认
    size_t peak_used_ = 0; // 本轮待发送队列占用峰值
    size_t sending_bytes_ = 0; // 当前发送调用持有的字节数
    const char* sending_kind_ = "idle"; // 当前发送类型：idle、pcm 或 finish
    int64_t sending_since_us_ = 0; // 当前发送调用开始时间
    int64_t last_send_us_ = 0; // 上一次发送调用耗时
    int64_t max_send_us_ = 0; // 本轮发送调用最大耗时
    int64_t network_tick_us_ = 0; // 网络循环上次处理队列的时间
    int64_t last_log_us_ = 0; // 上一次统计快照时间
    uint64_t logged_bytes_ = 0; // 上次快照累计采集字节数
    uint64_t logged_sent_bytes_ = 0; // 上次快照累计发送字节数
    uint32_t crc_ = 0xffffffffU; // 尚未执行末尾异或的 PCM CRC32
    bool finished_ = false; // 采集是否结束并允许发送 finish
    const char* input_error_ = nullptr; // 首次采音写入错误，正常结束与取消不记为错误
    std::atomic<bool> cancelled_{false}; // 是否停止接受本轮采样
};
}  // namespace workbuddy
