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

/** @brief TCP回调只发布状态，shared_ptr使关闭期间迟到的回调仍有有效存储。 */
struct ReceiveState {
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
        state->error = "实时语音响应格式异常";
        return;
    }
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(cJSON_ParseWithLength(data, size), cJSON_Delete);
    if (!json || !cJSON_IsObject(json.get())) { state->error = "实时语音响应无效"; return; }
    const std::string type = Field(json.get(), "type");
    const std::string session = Field(json.get(), "sessionId");
    if (session.empty() || session.size() > 64 || (!state->session.empty() && state->session != session)) {
        state->error = "实时语音会话编号不匹配";
        return;
    }
    state->session = session;
    if (type == "error") {
        const cJSON* code = cJSON_GetObjectItemCaseSensitive(json.get(), "code");
        state->code = cJSON_IsNumber(code) ? code->valueint : 502;
        state->error = Field(json.get(), "message");
        if (state->error.empty() || state->error.size() > 600) state->error = "实时语音识别失败，请重新说话";
    } else if (type == "ready") {
        state->ready = true;
    } else if (type == "partial" || type == "final") {
        std::string text = Field(json.get(), "text");
        if (!state->ready || text.size() > 2000 || (type == "final" && text.empty())) {
            state->error = "实时识别文字为空、过长或顺序错误";
            return;
        }
        if (state->text != text || type == "final") {
            state->text = std::move(text);
            ++state->revision;
        }
        state->final = type == "final";
    } else if (type != "recognizing") {
        state->error = "实时语音协议版本不兼容，请更新Java服务";
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

/** @return 无；识别与采音均已退出才可调用，取消标志阻止迟到写入，清空指针避免重复释放。 */
void VoiceStream::ReleaseAudioBuffer() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_.store(true);
    heap_caps_free(ring_);
    ring_ = nullptr;
    used_ = 0;
    read_ = 0;
}

/** @param pcm 前处理后的单声道数据；@param samples 采样数；@return 是否完整入队，绝不丢半帧。 */
bool VoiceStream::PushPcm(const int16_t* pcm, size_t samples) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cancelled_.load() || finished_ || overflow_ || !ring_ || !pcm || samples == 0) return false;
    const size_t size = samples * sizeof(int16_t);
    if (size > kCapacity - used_ || size > 480000U - bytes_) { overflow_ = true; return false; }
    const auto* data = reinterpret_cast<const uint8_t*>(pcm);
    const size_t write = (read_ + used_) % kCapacity;
    const size_t first = std::min(size, kCapacity - write);
    std::memcpy(ring_ + write, data, first);
    std::memcpy(ring_, data + first, size - first);
    used_ += size;
    bytes_ += size;
    // 与Java CRC32一致，确认整段音频没有在队列或传输中被截断，不保存完整录音。
    for (size_t i = 0; i < size; ++i) {
        crc_ ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc_ = (crc_ >> 1) ^ (0xedb88320U & (0U - (crc_ & 1U)));
    }
    return true;
}

/** @return 无；只有录音回调退出后调用，确保finish排在最后一帧之后。 */
void VoiceStream::FinishInput() { std::lock_guard<std::mutex> lock(mutex_); finished_ = true; }

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
    if (!ring_) { result.error = "实时录音缓冲申请失败"; return result; }
    if (gate.IsCancelled() || cancelled_.load()) return result;
    if (!EnsureNetworkReady(gate)) { result.error = "网络未连接，请重新连接后说话"; return result; }
    if (gate.IsCancelled() || cancelled_.load()) return result;
    auto* network = Board::GetInstance().GetNetwork();
    // 与电脑消息HTTP的连接号4隔离，避免蜂窝网络上旧查询收尾关闭新语音连接。
    auto socket = network ? network->CreateWebSocket(5) : nullptr;
    if (!socket) { result.error = "无法创建实时语音连接"; return result; }
    auto state = std::make_shared<ReceiveState>();
    socket->SetReceiveBufferSize(8192);
    socket->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    socket->SetHeader("WorkBuddy-Device-Token", token_.c_str());
    socket->OnData([state](const char* data, size_t size, bool binary) { Receive(state, data, size, binary); });
    socket->OnDisconnected([state]() { std::lock_guard<std::mutex> lock(state->mutex); state->disconnected = true; });
    socket->OnError([state](int) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->final && state->error.empty()) state->error = "实时语音传输异常，请重新说话";
    });
    const int64_t started = esp_timer_get_time();
    if (!socket->Connect(url_.c_str())) {
        result.error = "实时语音连接失败，请检查Java版本、WebSocket代理和设备授权";
        socket->Close();
        return result;
    }
    bool notified_ready = false;
    bool finish_sent = false;
    int64_t finish_at = 0;
    uint32_t painted_revision = 0;
    uint32_t sent_bytes = 0;
    uint8_t* const frame = ring_ + kCapacity;
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
            if (!finish_sent) { result.error = "识别提前结束，指令未发送"; break; }
            result.ok = true;
            result.transcript = std::move(text);
            on_text(result.transcript, true);
            break;
        }
        if (disconnected) { result.error = "语音连接中断，指令未发送，请重新说话"; break; }
        if (revision != painted_revision) {
            on_text(text, false);
            painted_revision = revision;
        }
        if (ready && !notified_ready) {
            notified_ready = true;
            on_ready();
        }
        const int64_t now = esp_timer_get_time();
        if ((!ready && now - started > 25000000) || (finish_sent && now - finish_at > 35000000) || now - started > 75000000) {
            result.error = "实时语音等待超时，指令未发送，请重试";
            break;
        }
        size_t count = 0;
        bool finished;
        bool overflow;
        uint32_t total;
        uint32_t crc;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished = finished_;
            overflow = overflow_;
            total = bytes_;
            crc = crc_ ^ 0xffffffffU;
            if (ready && !finish_sent) {
                count = std::min(used_, kFrameBytes);
                const size_t first = std::min(count, kCapacity - read_);
                std::memcpy(frame, ring_ + read_, first);
                std::memcpy(frame + first, ring_, count - first);
                read_ = (read_ + count) % kCapacity;
                used_ -= count;
            }
        }
        if (overflow) { result.error = "语音网络过慢，缓冲已满，请重新说话"; break; }
        if (count > 0) {
            if (!socket->Send(frame, count, true)) { result.error = "实时音频发送失败，请重新说话"; break; }
            sent_bytes += count;
        } else if (ready && finished && !finish_sent) {
            if (total < 16000 || sent_bytes != total) { result.error = "录音过短或不完整，指令未发送"; break; }
            char control[128];
            std::snprintf(control, sizeof(control), "{\"type\":\"finish\",\"audioBytes\":%u,\"pcmCrc32\":\"%08x\"}",
                          static_cast<unsigned>(total), static_cast<unsigned>(crc));
            if (!socket->Send(std::string(control))) { result.error = "语音结束确认发送失败"; break; }
            finish_sent = true;
            finish_at = esp_timer_get_time();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!result.ok && result.error.empty()) result.error = "本次语音已取消，指令未发送";
    result.authorization_required = result.error_code == 401;
    result.permission_required = result.error_code == 403;
    // Close及析构可能等待接收任务退出，只在本网络任务调用，禁止移至LVGL或音频回调。
    socket->Close();
    return result;
}
}  // namespace workbuddy
