#include <algorithm>
#include <new>
#include <optional>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include "infra/tasks/task_create.h"
#include "assets/lang_workbuddy.h"
#include "assets/lang_screen.h"
#include "boards/common/audio/bluetooth/bluetooth_audio.h"
#include "sysmgmt/tasks/task_executor.h"
#include "workbuddy_session.h"

static constexpr const char* kTag = "WorkBuddySession";
static constexpr size_t kMinSamples = 8000;
static constexpr int64_t kLocalReplyWaitUs = 180LL * 1000000;
static constexpr uint32_t kVoiceStackBytes = 16 * 1024;
static constexpr uint32_t kPcStackBytes = 12 * 1024;
static std::atomic<unsigned> s_request_workers{0};
static std::atomic<unsigned> s_authorization_workers{0};
static void StartCapture(const std::shared_ptr<WorkBuddyTurn>& turn);

static WorkBuddyConnectionState ConnectionResult(const WorkBuddyLocalAssistantStatus& result) {
    if (!result.ok) {
        return WorkBuddyConnectionState::Failed;
    }
    return result.online ? WorkBuddyConnectionState::Online : WorkBuddyConnectionState::Offline;
}

void WorkBuddySetAuthorizationView(const std::shared_ptr<WorkBuddyAuthorization>& authorization,
                                 WorkBuddyAuthPhase phase, const std::string& message,
                                 const std::string& url) {
    std::lock_guard<std::mutex> lock(authorization->view_mutex);
    if (authorization->gate.IsCancelled()) {
        return;
    }
    if (phase == WorkBuddyAuthPhase::Checking) {
        authorization->connection.store(WorkBuddyConnectionState::Querying);
    } else if (phase == WorkBuddyAuthPhase::Error) {
        authorization->connection.store(WorkBuddyConnectionState::Failed);
    } else if (phase != WorkBuddyAuthPhase::Authorized) {
        authorization->connection.store(WorkBuddyConnectionState::Unknown);
    }
    if (authorization->phase.load() == phase && authorization->message == message &&
        authorization->verification_url == url) {
        return;
    }
    authorization->message = message;
    authorization->verification_url = url;
    authorization->phase.store(phase);
    ++authorization->revision;
}


static bool WaitAuthorization(const std::shared_ptr<WorkBuddyAuthorization>& authorization, int milliseconds) {
    for (int elapsed = 0; elapsed < milliseconds; elapsed += 200) {
        if (authorization->gate.IsCancelled() || authorization->refresh_requested.load()) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return !authorization->gate.IsCancelled();
}

static bool WaitAuthorizationRefresh(const std::shared_ptr<WorkBuddyAuthorization>& authorization) {
    while (!authorization->gate.IsCancelled()) {
        if (authorization->refresh_requested.exchange(false)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return false;
}

static void VerifyLocalAssistant(const std::shared_ptr<WorkBuddyAuthorization>& authorization) {
    WorkBuddySetAuthorizationView(authorization, WorkBuddyAuthPhase::Checking, LangString_WB_AUTH_SAVED_CHECKING);
    auto status = WorkBuddyGetLocalStatus(authorization->gate);
    if (authorization->gate.IsCancelled()) {
        return;
    }
    authorization->connection.store(ConnectionResult(status));
    authorization->reauthorization_required.store(status.code == 401 || status.code == 403);
    std::string message;
    if (status.ok) {
        message = status.online ? LangString_WB_CONNECTED : LangString_WB_BOUND_OFFLINE;
    } else {
        message = LangString_WB_BOUND_CONNECT_FAILED + status.error;
        if (status.code == 401 || status.code == 403) {
            message += LangString_WB_RELINK_HINT;
        } else {
            message += LangString_WB_SYNC_RETRY_HINT;
        }
    }
    WorkBuddySetAuthorizationView(authorization, WorkBuddyAuthPhase::Authorized, message);
}

static void RunAuthorization(const std::shared_ptr<WorkBuddyAuthorization>& authorization) {
    bool create_new = authorization->refresh_requested.exchange(false);
    while (!authorization->gate.IsCancelled()) {
        auto awake = std::make_unique<PowerNeedHold>(PowerNeed::ForegroundNetwork);
        WorkBuddySetAuthorizationView(authorization, WorkBuddyAuthPhase::Checking,
                             create_new ? LangString_WB_QR_GENERATING : LangString_WB_AUTH_CHECKING);
        // 先恢复待处理会话：旧凭证在重新授权期间仍有效，先查询旧凭证会掩盖即将轮换的新凭证。
        auto session = create_new ? WorkBuddyAuthorizationSession{} : WorkBuddyLoadPendingSession();
        if (session.ok) {
            authorization->switch_account.store(session.account_switch_allowed);
        }
        if (!create_new && !session.ok) {
            auto status = WorkBuddyGetAuthorization(authorization->gate);
            if (authorization->gate.IsCancelled()) {
                return;
            }
            if (!status.ok) {
                WorkBuddySetAuthorizationView(authorization, WorkBuddyAuthPhase::Error, status.error);
                awake.reset();
                if (!WaitAuthorizationRefresh(authorization)) {
                    return;
                }
                // 授权状态查询的网络错误不代表凭证失效，重试时仍先检查已保存的授权。
                create_new = false;
                continue;
            }
            if (status.authorized) {
                VerifyLocalAssistant(authorization);
                return;
            }
        }
        if (!session.ok) {
            session = WorkBuddyCreateAuthorizationSession(authorization->gate, authorization->switch_account.load());
        }
        if (authorization->gate.IsCancelled()) {
            return;
        }
        if (!session.ok) {
            WorkBuddySetAuthorizationView(authorization, WorkBuddyAuthPhase::Error, session.error);
            awake.reset();
            if (!WaitAuthorizationRefresh(authorization)) {
                return;
            }
            create_new = true;
            continue;
        }
        authorization->expires_in.store(session.expires_in);
        const char* scan_hint =
            session.account_switch_allowed ? LangString_WB_SCAN_FRIEND : LangString_WB_SCAN_AUTHORIZE;
        WorkBuddySetAuthorizationView(authorization, WorkBuddyAuthPhase::Waiting, scan_hint, session.verification_url);
        const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(session.expires_in) * 1000000;
        int failures = 0;
        while (!authorization->gate.IsCancelled()) {
            if (authorization->refresh_requested.exchange(false)) {
                break;
            }
            auto status = WorkBuddyPollAuthorizationSession(session, authorization->gate);
            if (authorization->gate.IsCancelled()) {
                return;
            }
            if (authorization->refresh_requested.load()) {
                continue;
            }
            if (status.ok && status.authorized) {
                authorization->binding_changed.store(true);
                VerifyLocalAssistant(authorization);
                return;
            }
            const bool terminal = status.ok && (status.status == "expired" || status.status == "failed");
            if (terminal || esp_timer_get_time() >= deadline) {
                // 只有 Java 已确认会话终止，才允许退回旧授权；本地超时不代表腾讯回调不会继续到达。
                if (terminal) {
                    auto previous = WorkBuddyGetAuthorization(authorization->gate);
                    if (authorization->gate.IsCancelled()) {
                        return;
                    }
                    if (authorization->refresh_requested.load()) {
                        continue;
                    }
                    if (previous.ok && previous.authorized) {
                        VerifyLocalAssistant(authorization);
                        return;
                    }
                }
                WorkBuddySetAuthorizationView(
                    authorization, WorkBuddyAuthPhase::Expired,
                    status.status == "failed" ? LangString_WB_AUTH_INCOMPLETE : LangString_WB_QR_EXPIRED);
                awake.reset();
                if (!WaitAuthorizationRefresh(authorization)) {
                    return;
                }
                break;
            }
            if (!status.ok) {
                ++failures;
                if (failures >= 3) {
                    WorkBuddySetAuthorizationView(authorization, WorkBuddyAuthPhase::Error, status.error);
                    awake.reset();
                    if (!WaitAuthorizationRefresh(authorization)) {
                        return;
                    }
                    break;
                }
                WorkBuddySetAuthorizationView(authorization, WorkBuddyAuthPhase::Waiting, LangString_WB_AUTH_RECONNECTING,
                                     session.verification_url);
            } else {
                failures = 0;
                const bool authorizing = status.status == "authorizing";
                WorkBuddySetAuthorizationView(
                    authorization,
                    authorizing ? WorkBuddyAuthPhase::Authorizing : WorkBuddyAuthPhase::Waiting,
                    authorizing ? LangString_WB_AUTHORIZE_PHONE : scan_hint, session.verification_url);
            }
            WaitAuthorization(authorization, session.poll_interval * 1000);
        }
        create_new = true;
    }
}

static void AuthorizationTask(void* argument) {
    {
        std::unique_ptr<std::shared_ptr<WorkBuddyAuthorization>> holder(
            static_cast<std::shared_ptr<WorkBuddyAuthorization>*>(argument));
        auto authorization = *holder;
        try {
            RunAuthorization(authorization);
            authorization->running.store(false);
            // 授权完成后低频读取真实连接状态；取消仍复用同一门闩和任务上限。
            while (!authorization->gate.IsCancelled() &&
                   authorization->phase.load() == WorkBuddyAuthPhase::Authorized) {
                if (!WaitAuthorization(authorization, 30000)) {
                    break;
                }
                PowerNeedHold awake(PowerNeed::ForegroundNetwork);
                authorization->connection.store(WorkBuddyConnectionState::Querying);
                auto status = WorkBuddyGetLocalStatus(authorization->gate);
                if (authorization->gate.IsCancelled()) {
                    break;
                }
                authorization->connection.store(ConnectionResult(status));
                authorization->reauthorization_required.store(status.code == 401 || status.code == 403);
            }
        } catch (const std::bad_alloc&) {
            authorization->phase.store(WorkBuddyAuthPhase::Error);
            authorization->connection.store(WorkBuddyConnectionState::Failed);
            ESP_LOGE(kTag, "operation=authorize stage=execute result=no_memory");
        }
        authorization->running.store(false);
    }
    s_authorization_workers.fetch_sub(1);
    vTaskDeleteWithCaps(nullptr);
}

void WorkBuddyFailTurn(const std::shared_ptr<WorkBuddyTurn>& turn, const char* message) {
    std::lock_guard<std::mutex> lock(turn->result_mutex);
    try {
        turn->result.error = message;
    } catch (const std::bad_alloc&) {
        ESP_LOGE(kTag, "operation=publish_error result=no_memory");
    }
    turn->result_revision.fetch_add(1);
    turn->phase.store(WorkBuddyPhase::Error);
}

static void PublishTurnResult(const std::shared_ptr<WorkBuddyTurn>& turn, WorkBuddyTurnResult result,
                              WorkBuddyPhase phase) {
    if (turn->gate.IsCancelled()) {
        return;
    }
    std::lock_guard<std::mutex> lock(turn->result_mutex);
    turn->result = std::move(result);
    turn->result_revision.fetch_add(1);
    turn->phase.store(phase);
}

static bool WaitLocalReply(const std::shared_ptr<WorkBuddyTurn>& turn, int milliseconds) {
    for (int elapsed = 0; elapsed < milliseconds; elapsed += 200) {
        if (turn->gate.IsCancelled()) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return !turn->gate.IsCancelled();
}

static void WatchLocalReplies(const std::shared_ptr<WorkBuddyTurn>& turn, WorkBuddyTurnResult accepted) {
    PowerNeedHold wait_hold(PowerNeed::ForegroundNetwork);
    const int64_t started = esp_timer_get_time();
    const int64_t deadline = started + kLocalReplyWaitUs;
    bool received = false;
    int failures = 0;
    while (!turn->gate.IsCancelled() && esp_timer_get_time() < deadline) {
        const int64_t elapsed = esp_timer_get_time() - started;
        const int interval =
            failures ? failures * 3000 : (elapsed < 30000000 ? 2000 : (elapsed < 90000000 ? 5000 : 10000));
        const int wait_ms = static_cast<int>(std::min<int64_t>(interval, (deadline - esp_timer_get_time()) / 1000));
        if (wait_ms <= 0 || !WaitLocalReply(turn, wait_ms)) {
            break;
        }
        const int64_t remaining_ms = (deadline - esp_timer_get_time()) / 1000;
        if (remaining_ms < 1000) {
            break;
        }
        auto reply = WorkBuddyGetLocalReply(accepted.message_id, turn->gate,
                                            static_cast<int>(std::min<int64_t>(remaining_ms, 35000)));
        if (turn->gate.IsCancelled()) {
            break;
        }
        if (!reply.ok) {
            ++failures;
            const bool transient = reply.error_code <= 0 || reply.error_code == 429 || reply.error_code >= 500;
            if (transient && failures < 3) {
                continue;
            }
            reply.transcript = accepted.transcript;
            reply.error = LangString_WB_REPLY_INTERRUPTED + reply.error + LangString_WB_TASK_CONTINUES_HINT;
            if (received) {
                // 移动已显示文字，错误时保留最后读到的回复，不复制另一份长文本。
                std::lock_guard<std::mutex> lock(turn->result_mutex);
                reply.error = std::move(turn->result.reply) + "\n\n" + reply.error;
            }
            PublishTurnResult(turn, std::move(reply), WorkBuddyPhase::Error);
            return;
        }
        failures = 0;
        if (reply.status == "waiting") {
            continue;
        }
        reply.transcript = accepted.transcript;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(turn->result_mutex);
            // 固定受理编号持续读取，避免乱猜消息排序；内容不变时不刷新墨水屏。
            changed = !received || turn->result.reply != reply.reply;
        }
        if (changed) {
            PublishTurnResult(turn, std::move(reply), WorkBuddyPhase::Done);
        }
        received = true;
    }
    if (turn->gate.IsCancelled()) {
        return;
    }
    if (!received) {
        accepted.reply = LangString_WB_REPLY_TIMEOUT;
        PublishTurnResult(turn, std::move(accepted), WorkBuddyPhase::Done);
    }
}

static void LogRequestMemory(const char* stage, const char* task_name, uint32_t stack_bytes) {
    ESP_LOGW(kTag,
             "Request memory: stage=%s task=%s stack_bytes=%u stack_heap=PSRAM workers=%u "
             "internal_free=%u internal_largest=%u internal_min=%u psram_free=%u psram_largest=%u",
             stage, task_name, static_cast<unsigned>(stack_bytes), s_request_workers.load(),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}

static WorkBuddyTurnResult RecognizeTurn(const std::shared_ptr<WorkBuddyTurn>& turn) {
    WorkBuddyTurnResult result;
    if (turn->gate.IsCancelled() || !turn->held.load()) {
        result.error = LangString_WB_RECORDING_CANCELLED;
        return result;
    }
    PowerNeedHold preparing_audio(PowerNeed::LocalMic);
    if (!BluetoothAudio_WaitModeReady(kBluetoothAudioInitWaitMs,
                                   [turn]() { return !turn->gate.IsCancelled() && turn->held.load(); })) {
        result.error = turn->gate.IsCancelled() || !turn->held.load()
                           ? LangString_WB_RECORDING_CANCELLED
                           : (BluetoothAudio_GetState() == BluetoothAudioState::Failed ? LangString_AUDIO_MODULE_INIT_FAILED
                                                                                 : LangString_AUDIO_MODULE_NOT_READY);
    } else if (turn->gate.IsCancelled() || !turn->held.load()) {
        result.error = LangString_WB_RECORDING_CANCELLED;
    } else {
        result = turn->stream->Recognize(
            turn->gate,
            [turn]() {
                if (!TaskExecutor::Schedule([turn]() { StartCapture(turn); }, TaskDomain::Audio)) {
                    WorkBuddyFailTurn(turn, LangString_WB_RECORDING_TASK_BUSY);
                    turn->stream->Cancel();
                }
            },
            [turn](const std::string& text, bool final) {
                std::lock_guard<std::mutex> lock(turn->result_mutex);
                if (turn->gate.IsCancelled() || turn->phase.load() == WorkBuddyPhase::Error) {
                    return;
                }
                turn->result.transcript = text;
                turn->result_revision.fetch_add(1);
                if (final) {
                    turn->transcript_final.store(true);
                    turn->phase.store(WorkBuddyPhase::Sending);
                }
            });
    }
    return result;
}

static void ReleaseTurnAudio(const std::shared_ptr<WorkBuddyTurn>& turn) {
    for (;;) {
        try {
            if (TaskExecutor::Schedule([turn]() {
                    turn->audio.Stop();
                    if (turn->stream) {
                        turn->stream->ReleaseAudioBuffer();
                    }
                }, TaskDomain::Audio)) {
                return;
            }
        } catch (const std::bad_alloc&) {
            // 保留上下文直到收尾被接纳，避免内存紧张时遗留采音和音频缓冲。
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void RequestTask(void* arg) {
    {
        PowerNeedHold request(PowerNeed::ForegroundNetwork);
        std::unique_ptr<std::shared_ptr<WorkBuddyTurn>> holder(
            static_cast<std::shared_ptr<WorkBuddyTurn>*>(arg));
        auto turn = *holder;
        bool audio_release_queued = false;
        try {
            WorkBuddyTurnResult result;
            if (turn->is_local_refresh) {
                result = WorkBuddyGetLocalMessages(turn->gate);
            } else {
                result = RecognizeTurn(turn);
                turn->stream_running.store(false);
                turn->held.store(false);
                ReleaseTurnAudio(turn);
                audio_release_queued = true;
                if (!turn->gate.IsCancelled() && turn->phase.load() != WorkBuddyPhase::Error) {
                    if (result.ok) {
                        const std::string transcript = result.transcript;
                        // 最终文字已发布到UI，电脑请求可独立等待；失败绝不自动重发指令。
                        result = WorkBuddyPostText(transcript, turn->gate);
                        result.transcript = transcript;
                        if (!result.ok && (result.error_code <= 0 || result.error_code >= 500)) {
                            result.error += LangString_WB_SEND_UNCONFIRMED_HINT;
                        }
                    } else {
                        std::lock_guard<std::mutex> lock(turn->result_mutex);
                        result.transcript = turn->result.transcript;
                        result.error += LangString_WB_RECOGNITION_INCOMPLETE_HINT;
                    }
                } else {
                    // 采音失败或短录音错误优先于随后触发的网络取消错误，保留用户已经看到的文字。
                    std::lock_guard<std::mutex> lock(turn->result_mutex);
                    result = turn->result;
                }
            }
            if (!turn->gate.IsCancelled()) {
                if (!turn->is_local_refresh && result.ok && result.execution_mode == "local" &&
                    result.status == "accepted" && !result.message_id.empty()) {
                    turn->local_accepted.store(true);
                    turn->watch_running.store(true);
                    PublishTurnResult(turn, result, WorkBuddyPhase::Waiting);
                    WatchLocalReplies(turn, std::move(result));
                    turn->watch_running.store(false);
                    turn->result_revision.fetch_add(1);
                } else {
                    const bool ok = result.ok;
                    PublishTurnResult(turn, std::move(result),
                                      ok ? WorkBuddyPhase::Done : WorkBuddyPhase::Error);
                }
            }
        } catch (const std::bad_alloc&) {
            ESP_LOGE(kTag, "operation=turn stage=execute result=no_memory");
            WorkBuddyFailTurn(turn, LangString_WB_CONTEXT_MEMORY);
        }
        turn->held.store(false);
        turn->watch_running.store(false);
        turn->stream_running.store(false);
        if (!turn->is_local_refresh && !audio_release_queued) {
            // 包含异常出口，接纳释放工作前由网络会话继续持有音频上下文。
            ReleaseTurnAudio(turn);
        }
    }
    // FreeRTOS删除任务不会展开栈；智能指针已退出作用域，WithCaps才能释放栈和任务控制块。
    s_request_workers.fetch_sub(1);
    vTaskDeleteWithCaps(nullptr);
}

static const char* StartRequestTask(const std::shared_ptr<WorkBuddyTurn>& turn, const char* name,
                                    uint32_t stack_bytes) {
    if (s_request_workers.fetch_add(1) >= 2) {
        s_request_workers.fetch_sub(1);
        LogRequestMemory("previous_workers_closing", name, stack_bytes);
        return LangString_WB_PREVIOUS_CLOSING;
    }
    auto* argument = new (std::nothrow) std::shared_ptr<WorkBuddyTurn>(turn);
    if (!argument) {
        s_request_workers.fetch_sub(1);
        LogRequestMemory("argument_allocation_failed", name, stack_bytes);
        return LangString_WB_CONTEXT_MEMORY;
    }
    // TCB仍由IDF放内部RAM；只有大块栈使用PSRAM。NVS由client调度至Application内部栈。
    if (FirmwareCreateTask(RequestTask, name, stack_bytes, argument, 4, nullptr, tskNO_AFFINITY,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        delete argument;
        s_request_workers.fetch_sub(1);
        LogRequestMemory("task_allocation_failed", name, stack_bytes);
        return LangString_WB_TASK_MEMORY;
    }
    return "";
}

void WorkBuddyFinishTurn(const std::shared_ptr<WorkBuddyTurn>& turn) {
    if (turn->finish_scheduled.exchange(true)) {
        return;
    }
    if (!TaskExecutor::Schedule(
            [turn]() {
                turn->audio.Stop();
                if (turn->gate.IsCancelled() || turn->phase.load() == WorkBuddyPhase::Error) {
                    if (turn->stream) {
                        turn->stream->Cancel();
                    }
                    return;
                }
                if (turn->samples.load() < kMinSamples) {
                    WorkBuddyFailTurn(turn, LangString_WB_HOLD_TO_SPEAK_HINT);
                    if (turn->stream) {
                        turn->stream->Cancel();
                    }
                    return;
                }
                turn->phase.store(WorkBuddyPhase::Recognizing);
                if (turn->stream) {
                    turn->stream->FinishInput();
                }
            },
            TaskDomain::Audio)) {
        turn->finish_scheduled.store(false);
        WorkBuddyFailTurn(turn, LangString_WB_RECORDING_FINISH_BUSY);
        if (turn->stream) {
            turn->stream->Cancel();
        }
    }
}

static void StartCapture(const std::shared_ptr<WorkBuddyTurn>& turn) try {
    if (turn->gate.IsCancelled() || turn->finish_scheduled.load() ||
        turn->phase.load() != WorkBuddyPhase::Preparing || !turn->held.load() ||
        (turn->input_held && !turn->input_held())) {
        turn->held.store(false);
        turn->stream->Cancel();
        return;
    }
    // 仅保存弱引用，避免“对话持有音频回调、回调又持有对话”的循环引用。
    std::weak_ptr<WorkBuddyTurn> weak = turn;
    turn->phase.store(WorkBuddyPhase::Recording);
    std::string capture_error;
    const bool started = turn->audio.Start(
        [weak](const uint8_t* packet, size_t size, size_t samples) {
            auto current = weak.lock();
            if (!current || current->gate.IsCancelled() || !current->stream_running.load() ||
                current->phase.load() != WorkBuddyPhase::Recording) {
                return false;
            }
            // 松手收尾期间仍接收共享编码任务已排队的尾帧。
            if (!packet || size == 0) {
                WorkBuddyFailTurn(current, LangString_WB_AUDIO_CAPTURE_FAILED);
                current->finish_requested.store(true);
                return false;
            }
            const char* input_error = nullptr;
            if (!current->stream->PushOpus(packet, size, samples, input_error)) {
                // 正常取消、结束或网络收尾沿用原结果，只发布明确的采音写入错误。
                if (!input_error || !current->stream_running.load() || current->gate.IsCancelled() ||
                    current->phase.load() != WorkBuddyPhase::Recording) {
                    return false;
                }
                WorkBuddyFailTurn(current, input_error);
                current->finish_requested.store(true);
                return false;
            }
            current->samples.store(std::min(kMinSamples, current->samples.load() + samples));
            return true;
        },
        [turn]() {
            return !turn->gate.IsCancelled() && turn->held.load() && !turn->finish_scheduled.load() &&
                   (!turn->input_held || turn->input_held());
        },
        capture_error);
    if (!started) {
        turn->audio.Stop();
        WorkBuddyFailTurn(turn, capture_error.c_str());
        turn->stream->Cancel();
        return;
    }
} catch (const std::bad_alloc&) {
    turn->audio.Stop();
    WorkBuddyFailTurn(turn, LangString_WB_CONTEXT_MEMORY);
    if (turn->stream) {
        turn->stream->Cancel();
    }
    ESP_LOGE(kTag, "operation=capture stage=start result=no_memory");
}

int WorkBuddyStartAuthorization(const std::shared_ptr<WorkBuddyAuthorization>& authorization) {
    if (s_authorization_workers.fetch_add(1) >= 2) {
        s_authorization_workers.fetch_sub(1);
        authorization->running.store(false);
        WorkBuddySetAuthorizationView(authorization, WorkBuddyAuthPhase::Checking, LangString_WB_WAIT_CLOSE);
        return 1000;
    }
    auto* argument = new (std::nothrow) std::shared_ptr<WorkBuddyAuthorization>(authorization);
    authorization->running.store(true);
    if (!argument || FirmwareCreateTask(AuthorizationTask, "workbuddy_auth", 12 * 1024, argument, 4, nullptr, tskNO_AFFINITY,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        delete argument;
        s_authorization_workers.fetch_sub(1);
        authorization->running.store(false);
        WorkBuddySetAuthorizationView(authorization, WorkBuddyAuthPhase::Error, LangString_WB_WAIT_MEMORY);
    }
    return authorization->running.load() ? 0 : 2000;
}

void WorkBuddyBeginTurn(const std::shared_ptr<WorkBuddyTurn>& turn) {
    if (!TaskExecutor::Schedule([turn]() {
            try {
                if (turn->gate.IsCancelled() || turn->finish_scheduled.load() || !turn->held.load() ||
                    (turn->input_held && !turn->input_held())) {
                    turn->held.store(false);
                    return;
                }
                std::string error;
                turn->stream = WorkBuddyVoiceStream::Create(error);
                if (!turn->stream) {
                    LogRequestMemory("stream_initialization_failed", "workbuddy_voice", kVoiceStackBytes);
                    WorkBuddyFailTurn(turn, error.c_str());
                    return;
                }
                turn->stream_running.store(true);
                const char* failure = StartRequestTask(turn, "workbuddy_voice", kVoiceStackBytes);
                if (*failure) {
                    turn->stream_running.store(false);
                    turn->held.store(false);
                    turn->stream->ReleaseAudioBuffer();
                    WorkBuddyFailTurn(turn, failure);
                }
            } catch (const std::bad_alloc&) {
                turn->stream_running.store(false);
                turn->held.store(false);
                if (turn->stream) {
                    turn->stream->ReleaseAudioBuffer();
                }
                WorkBuddyFailTurn(turn, LangString_WB_CONTEXT_MEMORY);
                ESP_LOGE(kTag, "operation=turn stage=prepare result=no_memory");
            }
        }, TaskDomain::Audio)) {
        turn->held.store(false);
        WorkBuddyFailTurn(turn, LangString_WB_RECORDING_TASK_BUSY);
    }
}

void WorkBuddyReadMessages(const std::shared_ptr<WorkBuddyTurn>& turn) {
    const char* failure = StartRequestTask(turn, "workbuddy_pc", kPcStackBytes);
    if (*failure) {
        WorkBuddyFailTurn(turn, failure);
    }
}

void WorkBuddyCancelAuthorization(const std::shared_ptr<WorkBuddyAuthorization>& authorization) {
    if (authorization) {
        authorization->gate.RequestCancel();
        TaskExecutor::Schedule([authorization]() { authorization->gate.AbortBoundHttp(); }, TaskDomain::Cleanup);
    }
}

void WorkBuddyCancelTurn(const std::shared_ptr<WorkBuddyTurn>& turn) {
    if (!turn) {
        return;
    }
    turn->held.store(false);
    turn->gate.RequestCancel();
    // 请求任务也负责最终停止采音和释放缓冲；此处只加速取消，不把责任交给页面。
    TaskExecutor::Schedule([turn]() { turn->gate.AbortBoundHttp(); }, TaskDomain::Cleanup);
    WorkBuddyFinishTurn(turn);
}
