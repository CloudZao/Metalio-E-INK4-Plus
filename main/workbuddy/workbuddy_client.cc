#include "assets/lang_workbuddy.h"
#include "assets/lang_config.h"
#include "workbuddy_client.h"
#include "workbuddy_voice_stream.h"

#include "api_endpoints.h"
#include "api_http.h"
#include "application.h"
#include "board.h"
#include "power_policy.h"
#include "standby_screen/standby_screen.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace workbuddy {
namespace {
constexpr const char* kTag = "WorkBuddyHttp";
constexpr size_t kMaxResponseBytes = 32 * 1024;
std::mutex s_nvs_mutex;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

/** @brief HTTP 的解析结果；不保存或打印设备凭证。 */
struct JsonReply {
    bool ok = false; // HTTP 及业务码是否均成功
    int code = -1; // 业务或 HTTP 状态码
    Json root{nullptr, cJSON_Delete}; // 拥有解析结果的根对象
    std::string error; // 可直接显示的错误说明
};

/**
 * @brief 从 JSON 对象安全读取字符串，缺少字段时返回空串。
 * @param object JSON 对象，可为空。
 * @param key 字段名。
 * @return 字符串字段的副本。
 */
std::string StringField(const cJSON* object, const char* key) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}

/**
 * @brief 读取受限的正整数字段，防止异常服务响应造成忙轮询。
 * @param object JSON 对象。
 * @param key 字段名。
 * @param fallback 字段缺失时的默认值。
 * @param minimum 最小允许值。
 * @param maximum 最大允许值。
 * @return 范围内的整数。
 */
int IntField(const cJSON* object, const char* key, int fallback, int minimum, int maximum) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(value) || value->valuedouble < minimum || value->valuedouble > maximum) return fallback;
    return value->valueint;
}

/**
 * @brief 读取 NVS 字符串，缺失或读取失败时返回空串，避免存储故障触发设备重启。
 * @param handle 已打开的 NVS 句柄。
 * @param key 字段名。
 * @return 字符串副本，不包含末尾空字符。
 */
std::string ReadString(nvs_handle_t handle, const char* key) {
    size_t length = 0;
    if (nvs_get_str(handle, key, nullptr, &length) != ESP_OK || length == 0 || length > 2048) return {};
    std::string value(length, '\0');
    if (nvs_get_str(handle, key, value.data(), &length) != ESP_OK) return {};
    while (!value.empty() && value.back() == '\0') value.pop_back();
    return value;
}

enum class StorageStage {
    Queued, // 已投递，尚未开始访问 NVS
    Running, // 内部栈已开始操作，必须等待确定结果
    Done, // 结果已发布
    Cancelled // 排队超时，回调不得再访问 NVS
};

/** @brief 拥有操作结果和调度状态，不引用等待方的栈。 */
template <typename Result>
struct StoredResult {
    std::atomic<StorageStage> stage{StorageStage::Queued}; // 跨任务交接及超时取消
    Result value{}; // Done 发布后才允许读取
};

/**
 * @brief 将 NVS 操作统一交给内部栈；锁由具体操作在内部栈中获取。
 * @param action 拥有所需数据的操作，禁止捕获调用方的栈引用。
 * @param fallback 排队超时的返回值。
 * @param error 可选调度错误输出。
 * @return 操作结果；排队超过五秒则取消，已开始的写入等待提交结果，不报告未知成功状态。
 */
template <typename Result, typename Action>
Result OnStorageStack(Action action, Result fallback, std::string* error = nullptr) {
    if (error) error->clear();
    uint8_t stack_marker = 0;
    if (esp_ptr_internal(&stack_marker)) return action();
    auto pending = std::make_shared<StoredResult<Result>>();
    if (!Application::GetInstance().Schedule([pending, action = std::move(action)]() {
        StorageStage expected = StorageStage::Queued;
        if (!pending->stage.compare_exchange_strong(expected, StorageStage::Running)) return;
        pending->value = action();
        pending->stage.store(StorageStage::Done, std::memory_order_release);
    }, Application::TaskDomain::Storage)) {
        if (error) *error = Lang::Strings::WB_STORAGE_TASK_BUSY;
        return fallback;
    }
    const int64_t deadline = esp_timer_get_time() + 5000000;
    while (pending->stage.load(std::memory_order_acquire) != StorageStage::Done) {
        if (esp_timer_get_time() >= deadline) {
            StorageStage expected = StorageStage::Queued;
            if (pending->stage.compare_exchange_strong(expected, StorageStage::Cancelled)) {
                ESP_LOGW(kTag, "NVS dispatch timed out: task=%s", pcTaskGetName(nullptr));
                if (error) *error = Lang::Strings::WB_STORAGE_BUSY;
                return fallback;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return std::move(pending->value);
}

/**
 * @brief 在内部栈读取设备凭证，不记录凭证内容。
 * @param error 可选存储调度错误输出。
 * @return 已保存的设备凭证，未保存或读取失败时返回空串。
 */
std::string DeviceToken(std::string* error = nullptr) {
    return OnStorageStack([]() -> std::string {
        std::lock_guard<std::mutex> lock(s_nvs_mutex);
        nvs_handle_t handle = 0;
        if (nvs_open("workbuddy", NVS_READONLY, &handle) != ESP_OK) return {};
        auto value = ReadString(handle, "device_token");
        nvs_close(handle);
        return value;
    }, std::string{}, error);
}

/**
 * @brief 按UTF-8字节编码查询参数，防止消息编号拼入额外参数。
 * @param value 已验证长度的原始参数。
 * @return 百分号编码后的参数，不记录原值。
 */
std::string EncodeQuery(const std::string& value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 3);
    for (unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') encoded += static_cast<char>(ch);
        else { encoded += '%'; encoded += hex[ch >> 4]; encoded += hex[ch & 15]; }
    }
    return encoded;
}

/**
 * @brief 保存待授权会话与临时令牌，不覆盖仍可使用的已授权设备凭证。
 * @param session Java 创建的会话。
 * @return 是否成功提交；失败时页面不显示二维码。
 */
bool SaveSession(const AuthorizationSession& session) {
    return OnStorageStack([session]() {
        std::lock_guard<std::mutex> lock(s_nvs_mutex);
        nvs_handle_t handle = 0;
        esp_err_t error = nvs_open("workbuddy", NVS_READWRITE, &handle);
        if (error == ESP_OK) error = nvs_set_str(handle, "pending_token", session.device_token.c_str());
        if (error == ESP_OK) error = nvs_set_str(handle, "session_id", session.session_id.c_str());
        if (error == ESP_OK) error = nvs_set_str(handle, "verify_url", session.verification_url.c_str());
        if (error == ESP_OK) error = nvs_set_u8(handle, "switch_account", session.account_switch_allowed ? 1 : 0);
        if (error == ESP_OK) error = nvs_commit(handle);
        if (handle) nvs_close(handle);
        if (error != ESP_OK) ESP_LOGW(kTag, "Authorization storage failed: %s", esp_err_to_name(error));
        return error == ESP_OK;
    }, false);
}

/**
 * @brief 授权完成或二维码失效后移除待处理标记，保留音频请求所需的设备令牌。
 * @param expected_session 非空时仅清理匹配会话，避免旧任务覆盖新会话。
 * @return 无；清理失败不会丢失设备令牌，下次仍可查询授权状态。
 */
void ClearPendingSession(const std::string& expected_session = {}) {
    OnStorageStack([expected_session]() {
        std::lock_guard<std::mutex> lock(s_nvs_mutex);
        nvs_handle_t handle = 0;
        if (nvs_open("workbuddy", NVS_READWRITE, &handle) != ESP_OK) return false;
        if (!expected_session.empty() && ReadString(handle, "session_id") != expected_session) {
            nvs_close(handle);
            return false;
        }
        nvs_erase_key(handle, "session_id");
        nvs_erase_key(handle, "verify_url");
        nvs_erase_key(handle, "pending_token");
        nvs_erase_key(handle, "switch_account");
        nvs_commit(handle);
        nvs_close(handle);
        return true;
    }, false);
}

/**
 * @brief 会话授权成功后才替换正式设备凭证，刷新二维码不会提前使旧授权失效。
 * @param session 已被 Java 确认为 authorized 的会话。
 * @return 是否成功提交正式令牌；失败时保留待处理会话供下次重试。
 */
bool PromoteSession(const AuthorizationSession& session) {
    return OnStorageStack([session]() {
        std::lock_guard<std::mutex> lock(s_nvs_mutex);
        nvs_handle_t handle = 0;
        esp_err_t error = nvs_open("workbuddy", NVS_READWRITE, &handle);
        if (error != ESP_OK) return false;
        if (ReadString(handle, "session_id") != session.session_id ||
            ReadString(handle, "pending_token") != session.device_token) {
            nvs_close(handle);
            return false;
        }
        error = nvs_set_str(handle, "device_token", session.device_token.c_str());
        if (error == ESP_OK) {
            // 设备凭证轮换时清掉旧授权的消息游标，不把旧会话标识带入新授权。
            nvs_erase_key(handle, "local_msg");
            nvs_erase_key(handle, "session_id");
            nvs_erase_key(handle, "verify_url");
            nvs_erase_key(handle, "pending_token");
            nvs_erase_key(handle, "switch_account");
            error = nvs_commit(handle);
        }
        nvs_close(handle);
        if (error != ESP_OK) ESP_LOGW(kTag, "Authorization commit failed: %s", esp_err_to_name(error));
        return error == ESP_OK;
    }, false);
}

/**
 * @brief 发送 JSON、解析服务响应并回收连接。
 * @param method HTTP 方法。
 * @param path WorkBuddy 独立 Java 服务的接口路径。
 * @param content 请求体。
 * @param token 本机设备令牌，可为空。
 * @param gate 取消门闩。
 * @param timeout_ms 单次请求超时。
 * @return 已解析的标准 Result 响应或用户可读错误。
 */
JsonReply Request(const char* method, const std::string& path, std::string content,
                  const std::string& token, DownloadGate& gate, int timeout_ms) {
    JsonReply reply;
    if (gate.IsCancelled() || StandbyScreen::IsActive()) return reply;
    PowerNeedHold network_hold(PowerNeed::ForegroundNetwork);
    const std::string endpoint = api::WorkBuddyUrl(path.c_str());
    if (endpoint.empty()) { reply.error = Lang::Strings::WB_ENDPOINT_MISSING; return reply; }
    if (gate.IsCancelled()) return reply;
    if (!EnsureNetworkReady(gate)) {
        reply.error = Lang::Strings::WB_NETWORK_SETTINGS_HINT;
        return reply;
    }
    if (gate.IsCancelled()) return reply;
    auto* network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(4) : nullptr;
    if (!http) { reply.error = Lang::Strings::WB_CONNECTION_CREATE_FAILED; return reply; }
    Http* raw = http.get();
    raw->SetTimeout(timeout_ms);
    api::ApplyCommonHeaders(raw);
    raw->SetHeader("Content-Type", "application/json");
    raw->SetHeader("Accept", "application/json");
    raw->SetHeader("Accept-Encoding", "identity");
    if (!token.empty()) raw->SetHeader("WorkBuddy-Device-Token", token);
    if (!content.empty()) raw->SetContent(std::move(content));
    if (!gate.BindHttp(raw)) return reply;
    /** @brief 关闭本次请求，确保解析失败和页面取消时也释放套接字。 */
    struct HttpScope {
        DownloadGate& gate; // 绑定到本次 HTTP 请求的取消门闩
        /** @brief 离开请求作用域后关闭连接；无输入输出。 */
        ~HttpScope() { gate.CloseHttp(); }
    } scope{gate};
    if (!raw->Open(method, endpoint)) {
        reply.error = Lang::Strings::WB_SERVICE_TIMEOUT;
        return reply;
    }
    const int status = raw->GetStatusCode();
    if (status < 0) {
        reply.error = Lang::Strings::WB_RESPONSE_TIMEOUT;
        return reply;
    }
    std::string body;
    char buffer[1024];
    while (!gate.IsCancelled()) {
        const int count = raw->Read(buffer, sizeof(buffer));
        if (count < 0) {
            reply.error = Lang::Strings::WB_RESPONSE_READ_FAILED;
            return reply;
        }
        if (count == 0) break;
        if (body.size() + count > kMaxResponseBytes) {
            reply.error = Lang::Strings::WB_RESPONSE_TOO_LONG;
            return reply;
        }
        body.append(buffer, count);
    }
    if (gate.IsCancelled()) return reply;
    reply.root.reset(cJSON_Parse(body.c_str()));
    const cJSON* code = cJSON_GetObjectItemCaseSensitive(reply.root.get(), "code");
    // HTTP身份/权限拒绝优先于正文code，避免网关正文覆盖真实的401/403。
    reply.code = (status == 401 || status == 403) ? status : (cJSON_IsNumber(code) ? code->valueint : status);
    if (status < 200 || status >= 300 || reply.code != 0) {
        ESP_LOGW(kTag, "Request rejected: HTTP=%d code=%d",
                 status, reply.code);
    }
    if (status < 200 || status >= 300 || !cJSON_IsNumber(code) || code->valueint != 0) {
        reply.error = StringField(reply.root.get(), "msg");
        if (reply.error.empty()) reply.error = StringField(reply.root.get(), "message");
        if (reply.error.empty()) reply.error = Lang::Strings::WB_HTTP_ERROR_PREFIX + std::to_string(status) + ")";
        if (reply.error.size() > 600) reply.error = Lang::Strings::WB_RESPONSE_INVALID;
        return reply;
    }
    reply.ok = true;
    return reply;
}

/**
 * @brief 统一解析文字和语音的对话结果，保持设备授权过期及错误提示行为一致。
 * @param reply 已完成读取的 Java 标准 Result 响应。
 * @return 包含原问题和回答的结果；业务失败或回答为空时 ok 为 false。
 */
TurnResult ParseTurnReply(const JsonReply& reply) {
    TurnResult result;
    if (!reply.ok) {
        result.authorization_required = reply.code == 401;
        result.permission_required = reply.code == 403;
        result.error_code = reply.code;
        result.error = reply.error;
        return result;
    }
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(reply.root.get(), "data");
    result.transcript = StringField(data, "transcript");
    result.reply = StringField(data, "reply");
    result.execution_mode = StringField(data, "executionMode");
    result.message_id = StringField(data, "messageId");
    result.status = StringField(data, "status");
    if (result.reply.empty() && !(result.execution_mode == "local" && result.status == "waiting")) {
        result.error = Lang::Strings::WB_NO_REPLY;
        return result;
    }
    result.ok = true;
    return result;
}
}  // namespace

bool EnsureNetworkReady(DownloadGate& gate) {
    auto& board = Board::GetInstance();
    const int64_t deadline = esp_timer_get_time() + 30000000;
    while (!gate.IsCancelled() && !StandbyScreen::IsActive()) {
        if (board.GetNetworkErrorText(nullptr) || board.IsWifiConfigMode()) return false;
        if (board.IsNetworkReady()) return true;
        if (esp_timer_get_time() >= deadline) return false;
        if (board.EnsureNetworkReady(200)) return !gate.IsCancelled() && !StandbyScreen::IsActive();
        // 板级接口可能立即失败，避免失败循环持续占用 CPU。
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

/** @param error 失败原因；@return 带本机凭证的实时会话，失败返回空，不退回旧音频提交路径。 */
std::shared_ptr<VoiceStream> VoiceStream::Create(std::string& error) {
    error.clear();
    std::string url = api::WorkBuddyUrl(api::kWorkBuddyAsrStream);
    if (url.empty()) { error = Lang::Strings::WB_VOICE_ENDPOINT_MISSING; return {}; }
    const std::string token = DeviceToken(&error);
    if (!error.empty()) return {};
    if (token.empty()) { error = Lang::Strings::WB_TOKEN_MISSING_SCAN; return {}; }
    if (url.rfind("https://", 0) == 0) url.replace(0, 8, "wss://");
    else if (url.rfind("http://", 0) == 0) url.replace(0, 7, "ws://");
    else { error = Lang::Strings::WB_VOICE_ENDPOINT_INVALID; return {}; }
    auto stream = std::shared_ptr<VoiceStream>(new (std::nothrow) VoiceStream(std::move(url), token));
    if (!stream || !stream->ring_) { error = Lang::Strings::WB_RECORDING_BUFFER_MEMORY; return {}; }
    return stream;
}

/**
 * @brief 使用当前设备凭证查询腾讯电脑助理的真实状态，不凭扫码完成推断在线。
 * @param gate 可取消的请求门闩。
 * @return 严格解析的在线状态；权限不足和凭证失效保持独立错误码。
 */
LocalAssistantStatus GetLocalStatus(DownloadGate& gate) {
    LocalAssistantStatus result;
    const std::string token = DeviceToken(&result.error);
    if (!result.error.empty()) return result;
    if (token.empty()) { result.code = 401; result.error = Lang::Strings::WB_TOKEN_NOT_SAVED; return result; }
    auto reply = Request("GET", api::kWorkBuddyLocalStatus, {}, token, gate, 35000);
    result.code = reply.code;
    if (!reply.ok) { result.error = std::move(reply.error); return result; }
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(reply.root.get(), "data");
    const cJSON* online = cJSON_GetObjectItemCaseSensitive(data, "online");
    if (!cJSON_IsBool(online)) { result.error = Lang::Strings::WB_ONLINE_STATUS_INVALID; return result; }
    result.ok = true;
    result.online = cJSON_IsTrue(online);
    return result;
}

/**
 * @brief 查询设备自身授权，不再依赖 OTA 激活或小程序设备绑定记录。
 * @param gate 可取消请求门闩。
 * @return 当前用户授权是否可用。
 */
AuthorizationStatus GetAuthorization(DownloadGate& gate) {
    AuthorizationStatus result;
    auto reply = Request("GET", api::kWorkBuddyAuthorization, {}, DeviceToken(), gate, 20000);
    if (!reply.ok) { result.error = std::move(reply.error); return result; }
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(reply.root.get(), "data");
    const cJSON* authorized = cJSON_GetObjectItemCaseSensitive(data, "authorized");
    if (!cJSON_IsBool(authorized)) { result.error = Lang::Strings::WB_AUTH_STATUS_MISSING; return result; }
    result.ok = true;
    result.authorized = cJSON_IsTrue(authorized);
    result.status = result.authorized ? "authorized" : "pending";
    // 旧凭证仍有效时也可能正在重新授权，查询结果不能删除尚未提升的新凭证。
    return result;
}

/**
 * @brief 从 NVS 恢复未完成会话，只读取本设备保存的数据。
 * @return 可继续轮询的会话；不存在时 ok 为 false。
 */
AuthorizationSession LoadPendingSession() {
    return OnStorageStack([]() {
        AuthorizationSession session;
        std::lock_guard<std::mutex> lock(s_nvs_mutex);
        nvs_handle_t handle = 0;
        if (nvs_open("workbuddy", NVS_READONLY, &handle) != ESP_OK) return session;
        session.device_token = ReadString(handle, "pending_token");
        session.session_id = ReadString(handle, "session_id");
        session.verification_url = ReadString(handle, "verify_url");
        uint8_t switch_account = 0;
        nvs_get_u8(handle, "switch_account", &switch_account);
        session.account_switch_allowed = switch_account != 0;
        nvs_close(handle);
        session.ok = !session.device_token.empty() && !session.session_id.empty() && !session.verification_url.empty();
        return session;
    }, AuthorizationSession{});
}

/**
 * @brief 创建扫码或换绑申请；换绑须由Java明确确认能力，防止旧服务仍仅允许原账号。
 * @param gate 可取消请求门闩。
 * @param switch_account 是否请求其他账号通过扫码替换当前绑定。
 * @return 会话和扫码地址，存储失败则拒绝展示二维码。
 */
AuthorizationSession CreateAuthorizationSession(DownloadGate& gate, bool switch_account) {
    AuthorizationSession session;
    const std::string path = std::string(api::kWorkBuddyAuthorizationSessions) + (switch_account ? "?switchAccount=true" : "");
    auto reply = Request("POST", path, "{}", DeviceToken(), gate, 20000);
    if (!reply.ok) { session.error = std::move(reply.error); return session; }
    if (gate.IsCancelled()) { session.error = Lang::Strings::WB_SCAN_CANCELLED; return session; }
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(reply.root.get(), "data");
    session.account_switch_allowed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "accountSwitchAllowed"));
    if (switch_account && !session.account_switch_allowed) {
        session.error = Lang::Strings::WB_RELINK_UNSUPPORTED;
        return session;
    }
    session.session_id = StringField(data, "sessionId");
    session.device_token = StringField(data, "deviceToken");
    session.verification_url = StringField(data, "verificationUrl");
    session.expires_in = IntField(data, "expiresIn", 600, 30, 1800);
    session.poll_interval = IntField(data, "pollInterval", 3, 3, 30);
    if (session.session_id.empty() || session.session_id.size() > 128 ||
        session.session_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos ||
        session.device_token.empty() || session.device_token.size() > 512 ||
        session.verification_url.rfind("https://", 0) != 0 || session.verification_url.size() > 1024) {
        session.error = Lang::Strings::WB_QR_RESPONSE_INVALID;
        return session;
    }
    if (gate.IsCancelled()) return session;
    if (!SaveSession(session)) { session.error = Lang::Strings::WB_AUTH_SAVE_FAILED; return session; }
    session.ok = true;
    return session;
}

/**
 * @brief 轮询扫码结果，授权成功后仅保留设备凭证，腾讯令牌始终由 Java 保管。
 * @param session 当前设备创建的会话。
 * @param gate 可取消请求门闩。
 * @return 服务端会话状态。
 */
AuthorizationStatus PollAuthorizationSession(const AuthorizationSession& session, DownloadGate& gate) {
    AuthorizationStatus result;
    if (!session.ok || session.session_id.empty()) { result.error = Lang::Strings::WB_SCAN_SESSION_INVALID; return result; }
    auto reply = Request("GET", std::string(api::kWorkBuddyAuthorizationSessions) + "/" + session.session_id, {},
                         session.device_token, gate, 15000);
    if (!reply.ok) {
        result.error = std::move(reply.error);
        if (reply.code == 404 || reply.code == 410) { result.ok = true; result.status = "expired"; ClearPendingSession(session.session_id); }
        return result;
    }
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(reply.root.get(), "data");
    result.status = StringField(data, "status");
    if (result.status != "pending" && result.status != "authorizing" && result.status != "authorized" &&
        result.status != "expired" && result.status != "failed") {
        result.error = Lang::Strings::WB_AUTH_STATUS_INVALID;
        return result;
    }
    if (gate.IsCancelled()) return result;
    result.authorized = result.status == "authorized";
    if (result.authorized && !PromoteSession(session)) {
        result.authorized = false;
        result.error = Lang::Strings::WB_TOKEN_SAVE_FAILED;
        return result;
    }
    result.ok = true;
    if (result.status == "expired" || result.status == "failed") ClearPendingSession(session.session_id);
    return result;
}

/**
 * @brief 通过现有文本接口提交识别结果，不创建音频服务。
 * @param text 用户输入的 UTF-8 文本，限制为 2000 字节。
 * @param gate 支持离页取消的 HTTP 所有权容器。
 * @return 问题、回答或错误；不记录文字或设备凭证，不自动重放请求。
 */
TurnResult PostText(const std::string& text, DownloadGate& gate) {
    TurnResult result;
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { result.error = Lang::Strings::WB_ENTER_QUESTION; return result; }
    const size_t last = text.find_last_not_of(" \t\r\n");
    if (last - first + 1 > 2000 || text.find('\0') != std::string::npos) {
        result.error = Lang::Strings::WB_QUESTION_INVALID;
        return result;
    }
    const std::string token = DeviceToken(&result.error);
    if (!result.error.empty()) return result;
    if (token.empty()) {
        result.authorization_required = true;
        result.error = Lang::Strings::WB_AUTHORIZE_FIRST;
        return result;
    }
    Json payload(cJSON_CreateObject(), cJSON_Delete);
    const std::string question = text.substr(first, last - first + 1);
    if (!payload || !cJSON_AddStringToObject(payload.get(), "text", question.c_str())) {
        result.error = Lang::Strings::WB_TEXT_MEMORY;
        return result;
    }
    std::unique_ptr<char, decltype(&cJSON_free)> content(cJSON_PrintUnformatted(payload.get()), cJSON_free);
    if (!content) { result.error = Lang::Strings::WB_TEXT_MEMORY; return result; }
    result = ParseTurnReply(Request("POST", api::kWorkBuddyTextTurn, content.get(), token, gate, 210000));
    return result;
}

/**
 * @brief 读取历史消息并解析角色和全部文本元素，按旧到新排列。
 * @param gate 页面取消门闩。
 * @return 单条消息列表或明确错误，空数组按成功的空查询处理。
 */
TurnResult GetLocalMessages(DownloadGate& gate) {
    TurnResult result;
    const std::string token = DeviceToken(&result.error);
    if (!result.error.empty()) return result;
    if (token.empty()) {
        result.authorization_required = true;
        result.error = Lang::Strings::WB_AUTHORIZE_FIRST;
        return result;
    }
    const auto reply = Request("GET", std::string(api::kWorkBuddyLocalMessages) +
            "?mode=page&limit=20&offset=0", {}, token, gate, 35000);
    if (!reply.ok) {
        result.authorization_required = reply.code == 401;
        result.permission_required = reply.code == 403;
        result.error_code = reply.code;
        result.error = reply.error;
        return result;
    }
    const auto* data = cJSON_GetObjectItemCaseSensitive(reply.root.get(), "data");
    const auto* messages = cJSON_GetObjectItemCaseSensitive(data, "messages");
    if (!cJSON_IsArray(messages)) {
        result.error = Lang::Strings::WB_MESSAGE_INVALID;
        return result;
    }
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, messages) {
        const auto role = StringField(item, "role");
        if (role != "user" && role != "assistant") continue;
        LocalMessage message{StringField(item, "message_id"), role == "user", {}};
        if (message.id.empty()) continue;
        const auto* content = cJSON_GetObjectItemCaseSensitive(item, "content");
        const auto append = [&message](const cJSON* value) {
            const char* text = cJSON_IsString(value) ? value->valuestring : nullptr;
            if (!text && cJSON_IsObject(value)) {
                const auto* field = cJSON_GetObjectItemCaseSensitive(value, "text");
                if (cJSON_IsString(field)) text = field->valuestring;
            }
            if (!text || !*text) return;
            if (!message.text.empty()) message.text += '\n';
            message.text += text;
        };
        if (cJSON_IsArray(content)) {
            const cJSON* part = nullptr;
            cJSON_ArrayForEach(part, content) append(part);
        } else append(content);
        if (message.text.empty()) continue;
        auto found = std::find_if(result.messages.begin(), result.messages.end(), [&message](const LocalMessage& previous) {
            return previous.id == message.id && previous.mine == message.mine;
        });
        if (found == result.messages.end()) result.messages.push_back(std::move(message));
        else found->text = std::move(message.text);
    }
    // 历史接口按新到旧返回，聊天区按旧到新排列，最新消息位于末尾。
    std::reverse(result.messages.begin(), result.messages.end());
    result.ok = true;
    return result;
}

/**
 * @brief 使用本次受理编号查询新消息，自动读取失败不会重放已发送的指令。
 * @param message_id 不含空字符的受理编号。
 * @param gate 离页或停止等待时取消请求。
 * @param timeout_ms 剩余请求时间，限制为1至35秒。
 * @return waiting表示暂未回复；404明确提示先部署新版Java。
 */
TurnResult GetLocalReply(const std::string& message_id, DownloadGate& gate, int timeout_ms) {
    TurnResult result;
    if (message_id.empty() || message_id.size() > 2048 || message_id.find('\0') != std::string::npos) {
        result.error_code = 400;
        result.error = Lang::Strings::WB_REQUEST_ID_INVALID;
        return result;
    }
    const std::string token = DeviceToken(&result.error);
    if (!result.error.empty()) return result;
    if (token.empty()) {
        result.authorization_required = true;
        result.error_code = 401;
        result.error = Lang::Strings::WB_TOKEN_MISSING;
        return result;
    }
    const std::string path = std::string(api::kWorkBuddyLocalReply) + "?messageId=" + EncodeQuery(message_id);
    result = ParseTurnReply(Request("GET", path, {}, token, gate, std::clamp(timeout_ms, 1000, 35000)));
    if (result.error_code == 404) result.error = Lang::Strings::WB_AUTO_REPLY_UNAVAILABLE;
    if (result.ok && (result.execution_mode != "local" || result.message_id != message_id ||
                      (result.status != "waiting" && result.status != "messages"))) {
        result.ok = false;
        result.error_code = 502;
        result.error = Lang::Strings::WB_AUTO_REPLY_MISMATCH;
    }
    return result;
}
}  // namespace workbuddy
