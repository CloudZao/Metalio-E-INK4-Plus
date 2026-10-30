#include "assets/lang_workbuddy.h"
#include "assets/lang_config.h"
#include "workbuddy_voice_stream.h"
#include "board.h"
#include "power_policy.h"
#include "system_info.h"
#include "web_socket.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <utility>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace workbuddy {
namespace {
constexpr const char* kTag = "WorkBuddyStream";
constexpr int64_t kAudioLogIntervalUs = 1000000; // 常规音频统计最多每秒一次

/** @brief TCP回调只发布状态，shared_ptr使关闭期间迟到的回调仍有有效存储。 */
struct ReceiveState {
    const void* stream = nullptr; // 日志关联标识，仅输出地址，不解引用
    std::mutex mutex; // 保护回调与识别工作任务共享的状态
    bool ready = false; // 上游已允许发送音频
    bool final = false; // 已收到最终识别结果
    bool disconnected = false; // 底层连接已断开
    int code = 0; // 业务或 HTTP 状态码
    uint32_t revision = 0; // 识别文本快照版本
    std::string session; // 上游会话编号，用于校验消息归属
    std::string text; // 当前识别全文
    std::string error; // 可直接显示的错误信息
};

/** @param root JSON对象；@param name 字段名；@return 字符串字段或空串。 */
std::string Field(const cJSON* root, const char* name) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}

/** @param state 当前接收状态；@param data 完整消息；@param size 字节数；@param binary 是否二进制；@return 无。 */
void Receive(const std::shared_ptr<ReceiveState>& state, const char* data, size_t size, bool binary) {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->final || !state->error.empty()) return;
    if (binary || size == 0 || size > 8192) {
        state->error = Lang::Strings::WB_VOICE_RESPONSE_FORMAT;
        return;
    }
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(cJSON_ParseWithLength(data, size), cJSON_Delete);
    if (!json || !cJSON_IsObject(json.get())) { state->error = Lang::Strings::WB_VOICE_RESPONSE_INVALID; return; }
    const std::string type = Field(json.get(), "type");
    const std::string session = Field(json.get(), "sessionId");
    if (session.empty() || session.size() > 64 || (!state->session.empty() && state->session != session)) {
        state->error = Lang::Strings::WB_VOICE_SESSION_MISMATCH;
        return;
    }
    state->session = session;
    if (type == "error") {
        const cJSON* code = cJSON_GetObjectItemCaseSensitive(json.get(), "code");
        state->code = cJSON_IsNumber(code) ? code->valueint : 502;
        state->error = Field(json.get(), "message");
        if (state->error.empty() || state->error.size() > 600) state->error = Lang::Strings::WB_VOICE_RECOGNITION_FAILED;
        ESP_LOGW(kTag, "ASR receive: stream=%p event=server_error code=%d", state->stream, state->code);
    } else if (type == "ready") {
        if (!state->ready) ESP_LOGI(kTag, "ASR receive: stream=%p event=ready", state->stream);
        state->ready = true;
    } else if (type == "partial" || type == "final") {
        std::string text = Field(json.get(), "text");
        if (!state->ready || text.size() > 2000 || (type == "final" && text.empty())) {
            state->error = Lang::Strings::WB_VOICE_TEXT_INVALID;
            return;
        }
        if (state->text != text || type == "final") {
            state->text = std::move(text);
            ++state->revision;
        }
        state->final = type == "final";
        if (state->final) ESP_LOGI(kTag, "ASR receive: stream=%p event=final", state->stream);
    } else if (type != "recognizing") {
        state->error = Lang::Strings::WB_VOICE_VERSION;
    }
}
}  // namespace

/** @param url 已配置的实时入口；@param token 本机授权秘密；申请两秒PCM队列及发送帧，均放PSRAM。 */
VoiceStream::VoiceStream(std::string url, std::string token) : url_(std::move(url)), token_(std::move(token)) {
    // 队列与发送暂存区分开使用同一块分配，避免5KB局部数组挤占网络调用栈。
    ring_ = static_cast<uint8_t*>(heap_caps_malloc(kCapacity + kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!ring_) {
        ESP_LOGE(kTag, "ASR buffer allocation failed: requested_bytes=%u psram_free=%u psram_largest=%u",
                 static_cast<unsigned>(kCapacity + kFrameBytes),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    }
}

/** @return 无；销毁前网络工作任务已退出，录音接收器已停止。 */
VoiceStream::~VoiceStream() { heap_caps_free(ring_); }

void VoiceStream::LogAudioState(const char* event, bool force) {
    struct {
        size_t queued, peak, sending;
        uint64_t captured, sent, pcm_rate, send_rate;
        int64_t send_ms, last_ms, max_ms, loop_gap_ms;
        const char* kind;
        bool finished, cancelled;
    } stats;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int64_t now = esp_timer_get_time();
        const int64_t interval = now - last_log_us_;
        if (!force && interval < kAudioLogIntervalUs) return;
        stats.queued = used_;
        stats.peak = peak_used_;
        stats.sending = sending_bytes_;
        stats.captured = bytes_;
        stats.sent = sent_bytes_;
        stats.pcm_rate = last_log_us_ && interval > 0 ? (bytes_ - logged_bytes_) * 1000000 / interval : 0;
        stats.send_rate = last_log_us_ && interval > 0 ? (sent_bytes_ - logged_sent_bytes_) * 1000000 / interval : 0;
        stats.send_ms = sending_since_us_ ? (now - sending_since_us_) / 1000 : 0;
        stats.last_ms = last_send_us_ / 1000;
        stats.max_ms = max_send_us_ / 1000;
        stats.loop_gap_ms = network_tick_us_ ? (now - network_tick_us_) / 1000 : -1;
        stats.kind = sending_kind_;
        stats.finished = finished_;
        stats.cancelled = cancelled_.load();
        last_log_us_ = now;
        logged_bytes_ = bytes_;
        logged_sent_bytes_ = sent_bytes_;
    }
    ESP_LOGI(kTag, "ASR audio: stream=%p event=%s q=%u/%u peak=%u captured=%llu sent=%llu pcm_Bps=%llu tx_Bps=%llu "
             "send=%s inflight=%u send_ms=%lld last_ms=%lld max_ms=%lld loop_gap_ms=%lld finished=%d cancelled=%d",
             this, event, static_cast<unsigned>(stats.queued), static_cast<unsigned>(kCapacity),
             static_cast<unsigned>(stats.peak), static_cast<unsigned long long>(stats.captured),
             static_cast<unsigned long long>(stats.sent), static_cast<unsigned long long>(stats.pcm_rate),
             static_cast<unsigned long long>(stats.send_rate), stats.kind, static_cast<unsigned>(stats.sending),
             static_cast<long long>(stats.send_ms), static_cast<long long>(stats.last_ms),
             static_cast<long long>(stats.max_ms), static_cast<long long>(stats.loop_gap_ms), stats.finished, stats.cancelled);
}

/** @return 无；识别与采音均已退出才可调用，取消标志阻止迟到写入，清空指针避免重复释放。 */
void VoiceStream::ReleaseAudioBuffer() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_.store(true);
    heap_caps_free(ring_);
    ring_ = nullptr;
    used_ = 0;
    read_ = 0;
}

/** @param pcm 前处理后的单声道数据；@param samples 采样数；@param error 明确的写入错误；@return 是否完整入队，绝不丢半帧。 */
bool VoiceStream::PushPcm(const int16_t* pcm, size_t samples, const char*& error) {
    std::unique_lock<std::mutex> lock(mutex_);
    error = nullptr;
    if (cancelled_.load() || finished_) return false;
    if (!input_error_) {
        const char* event = nullptr;
        if (!ring_) { input_error_ = Lang::Strings::WB_VOICE_BUFFER_UNAVAILABLE; event = "buffer_unavailable"; }
        else if (!pcm || samples == 0) { input_error_ = Lang::Strings::WB_VOICE_PCM_INVALID; event = "pcm_invalid"; }
        else if (samples > (kCapacity - used_) / sizeof(int16_t)) {
            input_error_ = Lang::Strings::WB_VOICE_OVERFLOW;
            event = "queue_full";
        }
        if (event) {
            error = input_error_;
            lock.unlock();
            ESP_LOGW(kTag, "ASR PCM rejected: stream=%p reason=%s frame_samples=%u", this, event, static_cast<unsigned>(samples));
            LogAudioState(event, true);
            return false;
        }
    }
    if (input_error_) { error = input_error_; return false; }
    const bool first_frame = bytes_ == 0;
    const size_t size = samples * sizeof(int16_t);
    const auto* data = reinterpret_cast<const uint8_t*>(pcm);
    const size_t write = (read_ + used_) % kCapacity;
    const size_t first = std::min(size, kCapacity - write);
    std::memcpy(ring_ + write, data, first);
    std::memcpy(ring_, data + first, size - first);
    used_ += size;
    peak_used_ = std::max(peak_used_, used_);
    bytes_ += size;
    // 与Java CRC32一致，确认整段音频没有在队列或传输中被截断，不保存完整录音。
    for (size_t i = 0; i < size; ++i) {
        crc_ ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc_ = (crc_ >> 1) ^ (0xedb88320U & (0U - (crc_ & 1U)));
    }
    lock.unlock();
    LogAudioState(first_frame ? "capture_begin" : "pcm", first_frame);
    return true;
}

/** @return 无；只有录音回调退出后调用，确保finish排在最后一帧之后。 */
void VoiceStream::FinishInput() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
    }
    LogAudioState("input_finished", true);
}

/** @return 无；可从任何线程调用，实际断开由网络工作任务执行。 */
void VoiceStream::Cancel() { cancelled_.store(true); }

/**
 * @param gate 页面取消门闩；@param on_ready 请求开始采音；@param on_text 发布全文快照。
 * @return 只有收到服务端final且本地已发送finish时才返回成功。
 */
TurnResult VoiceStream::Recognize(DownloadGate& gate, const std::function<void()>& on_ready,
                                const std::function<void(const std::string&, bool)>& on_text) {
    TurnResult result;
    PowerNeedHold network_hold(PowerNeed::ForegroundNetwork);
    if (!ring_) { result.error = Lang::Strings::WB_VOICE_BUFFER_FAILED; return result; }
    if (gate.IsCancelled() || cancelled_.load()) return result;
    if (!EnsureNetworkReady(gate)) { result.error = Lang::Strings::WB_VOICE_NETWORK_HINT; return result; }
    if (gate.IsCancelled() || cancelled_.load()) return result;
    auto* network = Board::GetInstance().GetNetwork();
    // 与电脑消息HTTP的连接号4隔离，避免蜂窝网络上旧查询收尾关闭新语音连接。
    auto socket = network ? network->CreateWebSocket(5) : nullptr;
    if (!socket) { result.error = Lang::Strings::WB_VOICE_CONNECTION_CREATE; return result; }
    auto state = std::make_shared<ReceiveState>();
    state->stream = this;
    socket->SetReceiveBufferSize(8192);
    socket->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    socket->SetHeader("WorkBuddy-Device-Token", token_.c_str());
    socket->OnData([state](const char* data, size_t size, bool binary) { Receive(state, data, size, binary); });
    socket->OnDisconnected([state]() {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->disconnected = true;
        ESP_LOGW(kTag, "ASR receive: stream=%p event=disconnected", state->stream);
    });
    socket->OnError([state](int code) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->final && state->error.empty()) state->error = Lang::Strings::WB_VOICE_TRANSFER_FAILED;
        ESP_LOGW(kTag, "ASR receive: stream=%p event=transport_error code=%d", state->stream, code);
    });
    const int64_t started = esp_timer_get_time();
    LogAudioState("connect_begin", true);
    if (!socket->Connect(url_.c_str())) {
        result.error = Lang::Strings::WB_VOICE_CONNECTION_FAILED;
        Cancel();
        LogAudioState("connect_failed", true);
        socket->Close();
        return result;
    }
    LogAudioState("connected", true);
    bool notified_ready = false;
    bool finish_sent = false;
    int64_t finish_at = 0;
    uint32_t painted_revision = 0;
    uint8_t* const frame = ring_ + kCapacity;
    const auto send = [&](const void* data, size_t size, bool binary) {
        const int64_t before = esp_timer_get_time();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sending_since_us_ = before;
            sending_bytes_ = size;
            sending_kind_ = binary ? "pcm" : "finish";
        }
        if (!binary) LogAudioState("finish_send_begin", true);
        const bool ok = socket->Send(data, size, binary);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_send_us_ = esp_timer_get_time() - before;
            max_send_us_ = std::max(max_send_us_, last_send_us_);
            sending_since_us_ = 0;
            sending_bytes_ = 0;
            sending_kind_ = "idle";
            if (ok && binary) sent_bytes_ += size;
        }
        LogAudioState(ok ? (binary ? "send_return" : "finish_send_return") : "send_failed", !ok || !binary);
        return ok;
    };
    while (!gate.IsCancelled() && !cancelled_.load()) {
        bool ready;
        bool final;
        bool disconnected;
        std::string text;
        uint32_t revision;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            ready = state->ready;
            final = state->final;
            disconnected = state->disconnected;
            text = state->text;
            revision = state->revision;
            result.error = state->error;
            result.error_code = state->code;
        }
        if (!result.error.empty()) break;
        if (final) {
            if (!finish_sent) { result.error = Lang::Strings::WB_VOICE_EARLY_END; break; }
            result.ok = true;
            result.transcript = std::move(text);
            on_text(result.transcript, true);
            break;
        }
        if (disconnected) { result.error = Lang::Strings::WB_VOICE_DISCONNECTED; break; }
        if (revision != painted_revision) {
            on_text(text, false);
            painted_revision = revision;
        }
        if (ready && !notified_ready) {
            notified_ready = true;
            on_ready();
        }
        const int64_t now = esp_timer_get_time();
        if (!ready && now - started > 25000000) { result.error = Lang::Strings::WB_VOICE_READY_TIMEOUT; break; }
        if (finish_sent && now - finish_at > 35000000) { result.error = Lang::Strings::WB_VOICE_FINAL_TIMEOUT; break; }
        size_t count = 0;
        bool finished;
        const char* input_error;
        uint64_t total;
        uint64_t sent_bytes;
        uint32_t crc;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished = finished_;
            input_error = input_error_;
            total = bytes_;
            sent_bytes = sent_bytes_;
            network_tick_us_ = now;
            crc = crc_ ^ 0xffffffffU;
            if (ready && !finish_sent && !input_error) {
                count = std::min(used_, kFrameBytes);
                const size_t first = std::min(count, kCapacity - read_);
                std::memcpy(frame, ring_ + read_, first);
                std::memcpy(frame + first, ring_, count - first);
                read_ = (read_ + count) % kCapacity;
                used_ -= count;
            }
        }
        if (input_error) { result.error = input_error; break; }
        if (count > 0) {
            if (!send(frame, count, true)) { result.error = Lang::Strings::WB_VOICE_SEND_FAILED; break; }
        } else if (ready && finished && !finish_sent) {
            if (total < 16000) { result.error = Lang::Strings::WB_VOICE_TOO_SHORT; break; }
            if (sent_bytes != total) { result.error = Lang::Strings::WB_VOICE_INCOMPLETE; break; }
            char control[128];
            std::snprintf(control, sizeof(control), "{\"type\":\"finish\",\"audioBytes\":%llu,\"pcmCrc32\":\"%08x\"}",
                          static_cast<unsigned long long>(total), static_cast<unsigned>(crc));
            if (!send(control, std::strlen(control), false)) { result.error = Lang::Strings::WB_VOICE_FINISH_FAILED; break; }
            finish_sent = true;
            finish_at = esp_timer_get_time();
        }
        LogAudioState("network_loop");
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!result.ok && result.error.empty()) result.error = Lang::Strings::WB_VOICE_CANCELLED;
    result.authorization_required = result.error_code == 401;
    result.permission_required = result.error_code == 403;
    // 关闭连接前停止接收采样，收尾期间的迟到帧不能覆盖真实网络错误。
    Cancel();
    if (!result.ok && !gate.IsCancelled()) {
        ESP_LOGW(kTag, "ASR failed: stream=%p ready=%d finish_sent=%d service_code=%d elapsed_ms=%lld",
                 this, notified_ready, finish_sent, result.error_code, static_cast<long long>((esp_timer_get_time() - started) / 1000));
    }
    LogAudioState(result.ok ? "success" : "stopped", true);
    // Close及析构可能等待接收任务退出，只在本网络任务调用，禁止移至LVGL或音频回调。
    LogAudioState("close_begin", true);
    socket->Close();
    socket.reset();
    LogAudioState("close_end", true);
    return result;
}
}  // namespace workbuddy
