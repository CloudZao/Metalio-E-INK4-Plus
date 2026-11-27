#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "web_socket.h"
#include "workbuddy_voice_stream.h"
#include "assets/lang_config.h"
#include "assets/lang_workbuddy.h"
#include "boards/common/board/board.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "infra/platform/system_info.h"

static constexpr const char* kWorkBuddyTag = "WorkBuddyStream";

/** @brief TCP回调只发布状态，shared_ptr使关闭期间迟到的回调仍有有效存储。 */
struct WorkBuddyReceiveState {
    std::mutex mutex;                       // 保护回调与识别工作任务共享的状态
    bool ready = false;                     // 上游已允许发送音频
    bool final = false;                     // 已收到最终识别结果
    bool disconnected = false;              // 底层连接已断开
    const char* failure_stage = "receive";  // 安全的失败阶段标识，不记录上游正文
    int code = 0;                           // 业务或 HTTP 状态码
    uint32_t revision = 0;                  // 识别文本快照版本
    std::string session;                    // 上游会话编号，用于校验消息归属
    std::string text;                       // 当前识别全文
    std::string error;                      // 可直接显示的错误信息
};

// 参数 root：JSON对象；@param name 字段名；@return 字符串字段或空串。
static std::string WorkBuddyField(const cJSON* root, const char* name) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}

// 参数 state：当前接收状态；@param data 完整消息；@param size 字节数；@param binary 是否二进制；@return
// 无。
static void WorkBuddyReceive(const std::shared_ptr<WorkBuddyReceiveState>& state, const char* data, size_t size,
                             bool binary) {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->final || !state->error.empty()) {
        return;
    }
    const auto reject = [&state](const char* error, const char* stage) {
        state->error = error;
        state->failure_stage = stage;
    };
    if (binary || size == 0 || size > 8192) {
        reject(LangString_WB_VOICE_RESPONSE_FORMAT, "response_format");
        return;
    }
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(cJSON_ParseWithLength(data, size), cJSON_Delete);
    if (!json || !cJSON_IsObject(json.get())) {
        reject(LangString_WB_VOICE_RESPONSE_INVALID, "response_json");
        return;
    }
    const std::string type = WorkBuddyField(json.get(), "type");
    const std::string session = WorkBuddyField(json.get(), "sessionId");
    if (session.empty() || session.size() > 64 || (!state->session.empty() && state->session != session)) {
        reject(LangString_WB_VOICE_SESSION_MISMATCH, "session");
        return;
    }
    state->session = session;
    if (type == "error") {
        state->failure_stage = "server";
        const cJSON* code = cJSON_GetObjectItemCaseSensitive(json.get(), "code");
        state->code = cJSON_IsNumber(code) ? code->valueint : 502;
        state->error = WorkBuddyField(json.get(), "message");
        if (state->error.empty() || state->error.size() > 600) {
            state->error = LangString_WB_VOICE_RECOGNITION_FAILED;
        }
    } else if (type == "ready") {
        state->ready = true;
    } else if (type == "partial" || type == "final") {
        std::string text = WorkBuddyField(json.get(), "text");
        if (!state->ready || text.size() > 2000 || (type == "final" && text.empty())) {
            reject(LangString_WB_VOICE_TEXT_INVALID, "text");
            return;
        }
        if (state->text != text || type == "final") {
            state->text = std::move(text);
            ++state->revision;
        }
        state->final = type == "final";
    } else if (type != "recognizing") {
        reject(LangString_WB_VOICE_VERSION, "protocol");
    }
}

/**
 * @brief 初始化实时语音连接及编码缓冲
 * @param url 已配置的实时入口
 * @param token 本机授权秘密；在 PSRAM 申请有界 Opus 包队列与发送暂存区。
 */
WorkBuddyVoiceStream::WorkBuddyVoiceStream(std::string url, std::string token)
    : url_(std::move(url)), token_(std::move(token)) {
    ring_ = static_cast<uint8_t*>(heap_caps_malloc(kCapacity + kMaxPacketBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!ring_) {
        ESP_LOGE(kWorkBuddyTag, "ASR buffer allocation failed: requested_bytes=%u psram_free=%u psram_largest=%u",
                 static_cast<unsigned>(kCapacity + kMaxPacketBytes),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    }
}

/**
 * @brief 释放本轮音频缓冲
 * @note 调用方须先停止录音回调，并等待网络工作任务退出。
 */
WorkBuddyVoiceStream::~WorkBuddyVoiceStream() {
    heap_caps_free(ring_);
}

/** @brief 接收共享编码任务输出的单个 Opus 包，队列中的长度前缀仅供本地分帧。 */
bool WorkBuddyVoiceStream::PushOpus(const uint8_t* packet, size_t size, size_t samples, const char*& error) {
    std::unique_lock<std::mutex> lock(mutex_);
    error = nullptr;
    if (cancelled_.load() || finished_) {
        return false;
    }
    if (!input_error_) {
        const char* event = nullptr;
        if (!ring_) {
            input_error_ = LangString_WB_VOICE_BUFFER_UNAVAILABLE;
            event = "buffer_unavailable";
        } else if (!packet || size == 0 || size > kMaxPacketBytes || samples == 0) {
            input_error_ = LangString_WB_VOICE_PACKET_INVALID;
            event = "packet_invalid";
        } else if (size + sizeof(uint16_t) > kCapacity - used_) {
            input_error_ = LangString_WB_VOICE_OVERFLOW;
            event = "queue_full";
        }
        if (event) {
            error = input_error_;
            lock.unlock();
            ESP_LOGW(kWorkBuddyTag, "ASR Opus rejected: reason=%s packet_bytes=%u", event, static_cast<unsigned>(size));
            return false;
        }
    }
    if (input_error_) {
        error = input_error_;
        return false;
    }
    const uint8_t header[] = {static_cast<uint8_t>(size), static_cast<uint8_t>(size >> 8)};
    const auto append = [this](const uint8_t* data, size_t count) {
        const size_t write = (read_ + used_) % kCapacity;
        const size_t first = std::min(count, kCapacity - write);
        std::memcpy(ring_ + write, data, first);
        std::memcpy(ring_, data + first, count - first);
        used_ += count;
    };
    append(header, sizeof(header));
    append(packet, size);
    bytes_ += size;
    samples_ += samples;
    return true;
}

/**
 * @brief 在最后一帧音频发送后结束语音输入
 * @return 无；只有录音回调退出后调用，确保finish排在最后一帧之后。
 */
void WorkBuddyVoiceStream::FinishInput() {
    std::lock_guard<std::mutex> lock(mutex_);
    finished_ = true;
}

/**
 * @brief 请求网络任务断开实时语音连接
 * @return 无；可从任何线程调用，实际断开由网络工作任务执行。
 */
void WorkBuddyVoiceStream::Cancel() {
    cancelled_.store(true);
}

/**
 * @brief 在录音和网络任务退出后释放音频缓冲
 * @return 无；识别与采音均已退出才可调用，取消标志阻止迟到写入，清空指针避免重复释放。
 */
void WorkBuddyVoiceStream::ReleaseAudioBuffer() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_.store(true);
    heap_caps_free(ring_);
    ring_ = nullptr;
    used_ = 0;
    read_ = 0;
}

// 参数 gate：页面取消门闩；@param on_ready 请求开始采音；@param on_text 发布全文快照。
// 返回值：只有收到服务端final且本地已发送finish时才返回成功。
WorkBuddyTurnResult WorkBuddyVoiceStream::Recognize(DownloadGate& gate, const std::function<void()>& on_ready,
                                                    const std::function<void(const std::string&, bool)>& on_text) {
    WorkBuddyTurnResult result;
    const char* failure_stage = "receive";
    PowerNeedHold network_hold(PowerNeed::ForegroundNetwork);
    if (!ring_) {
        result.error = LangString_WB_VOICE_BUFFER_FAILED;
        ESP_LOGE(kWorkBuddyTag, "ASR start failed: stage=buffer");
        return result;
    }
    if (gate.IsCancelled() || cancelled_.load()) {
        return result;
    }
    if (!WorkBuddyEnsureNetworkReady(gate)) {
        result.error = LangString_WB_VOICE_NETWORK_HINT;
        if (!gate.IsCancelled() && !cancelled_.load()) {
            ESP_LOGW(kWorkBuddyTag, "ASR start failed: stage=network_ready");
        }
        return result;
    }
    if (gate.IsCancelled() || cancelled_.load()) {
        return result;
    }
    auto* network = Board::GetInstance().GetNetwork();
    // 与电脑消息HTTP的连接号4隔离，避免蜂窝网络上旧查询收尾关闭新语音连接。
    auto socket = network ? network->CreateWebSocket(5) : nullptr;
    if (!socket) {
        result.error = LangString_WB_VOICE_CONNECTION_CREATE;
        ESP_LOGE(kWorkBuddyTag, "ASR start failed: stage=socket_create");
        return result;
    }
    auto state = std::make_shared<WorkBuddyReceiveState>();
    socket->SetReceiveBufferSize(8192);
    socket->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    socket->SetHeader("WorkBuddy-Device-Token", token_.c_str());
    socket->OnData(
        [state](const char* data, size_t size, bool binary) { WorkBuddyReceive(state, data, size, binary); });
    socket->OnDisconnected([state]() {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->disconnected = true;
    });
    socket->OnError([state](int code) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->final && state->error.empty()) {
            state->error = LangString_WB_VOICE_TRANSFER_FAILED;
            state->failure_stage = "transport";
        }
        ESP_LOGD(kWorkBuddyTag, "ASR transport callback: code=%d", code);
    });
    const int64_t started = esp_timer_get_time();
    ESP_LOGI(kWorkBuddyTag, "ASR started: format=opus");
    if (!socket->Connect(url_.c_str())) {
        result.error = LangString_WB_VOICE_CONNECTION_FAILED;
        ESP_LOGW(kWorkBuddyTag, "ASR connection failed: code=%d", socket->GetLastError());
        Cancel();
        socket->Close();
        return result;
    }
    bool notified_ready = false;
    bool finish_sent = false;
    int64_t finish_at = 0;
    uint32_t painted_revision = 0;
    uint8_t* const frame = ring_ + kCapacity;
    uint32_t audio_crc = 0xffffffffU;
    uint64_t sent_bytes = 0;
    const auto send = [&](const void* data, size_t size, bool binary) {
        if (!socket->Send(data, size, binary)) {
            return false;
        }
        if (binary) {
            sent_bytes += size;
        }
        return true;
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
            failure_stage = state->failure_stage;
        }
        if (!result.error.empty()) {
            break;
        }
        if (final) {
            if (!finish_sent) {
                result.error = LangString_WB_VOICE_EARLY_END;
                failure_stage = "early_final";
                break;
            }
            result.ok = true;
            result.transcript = std::move(text);
            on_text(result.transcript, true);
            break;
        }
        if (disconnected) {
            result.error = LangString_WB_VOICE_DISCONNECTED;
            failure_stage = "disconnected";
            break;
        }
        if (revision != painted_revision) {
            on_text(text, false);
            painted_revision = revision;
        }
        if (ready && !notified_ready) {
            notified_ready = true;
            on_ready();
        }
        const int64_t now = esp_timer_get_time();
        if (!ready && now - started > 25000000) {
            result.error = LangString_WB_VOICE_READY_TIMEOUT;
            failure_stage = "ready_timeout";
            break;
        }
        if (finish_sent && now - finish_at > 35000000) {
            result.error = LangString_WB_VOICE_FINAL_TIMEOUT;
            failure_stage = "final_timeout";
            break;
        }
        size_t count = 0;
        bool finished;
        const char* input_error;
        uint64_t total;
        uint64_t samples;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished = finished_;
            input_error = input_error_;
            total = bytes_;
            samples = samples_;
            if (ready && !finish_sent && !input_error && used_ > 0) {
                count = ring_[read_] | (static_cast<size_t>(ring_[(read_ + 1) % kCapacity]) << 8);
                read_ = (read_ + sizeof(uint16_t)) % kCapacity;
                used_ -= sizeof(uint16_t);
                const size_t first = std::min(count, kCapacity - read_);
                std::memcpy(frame, ring_ + read_, first);
                std::memcpy(frame + first, ring_, count - first);
                read_ = (read_ + count) % kCapacity;
                used_ -= count;
            }
        }
        if (input_error) {
            result.error = input_error;
            failure_stage = "input";
            break;
        }
        if (count > 0) {
            if (!send(frame, count, true)) {
                result.error = LangString_WB_VOICE_SEND_FAILED;
                failure_stage = "audio_send";
                break;
            }
            // 与 Java CRC32 一致，按发送顺序只累计 Opus 包正文，不含 WebSocket 帧头。
            for (size_t i = 0; i < count; ++i) {
                audio_crc ^= frame[i];
                for (int bit = 0; bit < 8; ++bit) {
                    audio_crc = (audio_crc >> 1) ^ (0xedb88320U & (0U - (audio_crc & 1U)));
                }
            }
        } else if (ready && finished && !finish_sent) {
            if (samples < 8000) {
                result.error = LangString_WB_VOICE_TOO_SHORT;
                failure_stage = "duration";
                break;
            }
            if (sent_bytes != total) {
                result.error = LangString_WB_VOICE_INCOMPLETE;
                failure_stage = "audio_length";
                break;
            }
            char control[128];
            std::snprintf(control, sizeof(control), "{\"type\":\"finish\",\"audioBytes\":%llu,\"audioCrc32\":\"%08x\"}",
                          static_cast<unsigned long long>(sent_bytes), static_cast<unsigned>(audio_crc ^ 0xffffffffU));
            if (!send(control, std::strlen(control), false)) {
                result.error = LangString_WB_VOICE_FINISH_FAILED;
                failure_stage = "finish_send";
                break;
            }
            finish_sent = true;
            finish_at = esp_timer_get_time();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!result.ok && result.error.empty()) {
        result.error = LangString_WB_VOICE_CANCELLED;
    }
    result.authorization_required = result.error_code == 401;
    result.permission_required = result.error_code == 403;
    // 关闭连接前停止接收采样，收尾期间的迟到帧不能覆盖真实网络错误。
    const bool cancelled = gate.IsCancelled() || cancelled_.load();
    Cancel();
    if (result.ok) {
        ESP_LOGI(kWorkBuddyTag, "ASR completed: audio_bytes=%llu elapsed_ms=%lld",
                 static_cast<unsigned long long>(sent_bytes),
                 static_cast<long long>((esp_timer_get_time() - started) / 1000));
    } else if (!cancelled) {
        ESP_LOGW(kWorkBuddyTag, "ASR failed: stage=%s ready=%d finish_sent=%d service_code=%d elapsed_ms=%lld",
                 failure_stage, notified_ready, finish_sent, result.error_code,
                 static_cast<long long>((esp_timer_get_time() - started) / 1000));
    }
    // Close及析构可能等待接收任务退出，只在本网络任务调用，禁止移至LVGL或音频回调。
    socket->Close();
    socket.reset();
    return result;
}
