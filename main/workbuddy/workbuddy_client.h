#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "reader/download_gate.h"

namespace workbuddy {

/** @brief 原始接口返回的单条可显示消息。 */
struct LocalMessage {
    std::string id; // 上游真实消息编号，不作为分页游标
    bool mine = false; // user 为我，assistant 为助理
    std::string text; // 合并全部文本元素，保留正文换行
};

/** @brief 在后台分片等待网络，离页或待机时结束，尊重用户的 Wi-Fi 开关 */
bool EnsureNetworkReady(DownloadGate& gate);

/** @brief Java 一轮语音或文字对话的结果；错误信息可直接用于页面提示。 */
struct TurnResult {
    bool ok = false;
    bool authorization_required = false;
    bool permission_required = false;
    int error_code = 0;
    std::string transcript;
    std::string reply;
    std::string error;
    std::string execution_mode;
    std::string message_id;
    std::string status;
    std::vector<LocalMessage> messages; // 手动同步的独立消息，按旧到新排列
};

/** @brief Java 返回的设备授权状态；凭证只在 HTTP 请求头和本机 NVS 中使用。 */
struct AuthorizationStatus {
    bool ok = false;
    bool authorized = false;
    std::string status;
    std::string error;
};

/** @brief 腾讯实际校验的电脑助理状态；离线、权限不足和令牌失效不能互相替代。 */
struct LocalAssistantStatus {
    bool ok = false;
    bool online = false;
    int code = 0;
    std::string error;
};

/**
 * @brief 在后台用已保存的设备凭证查询电脑助理，授权保存不等于电脑已经连接成功。
 * @param gate 页面拥有的可取消请求门闩。
 * @return 腾讯报告的在线状态或明确错误；仅进行读取，不发送电脑指令。
 */
LocalAssistantStatus GetLocalStatus(DownloadGate& gate);

/** @brief 扫码会话；二维码仅编码 verification_url，禁止显示或记录 device_token。 */
struct AuthorizationSession {
    bool ok = false;
    bool account_switch_allowed = false;
    std::string session_id;
    std::string device_token;
    std::string verification_url;
    int expires_in = 600;
    int poll_interval = 3;
    std::string error;
};

/**
 * @brief 使用本机保存的设备凭证检查当前设备是否已完成用户授权。
 * @param gate 页面拥有的可取消网络门闩。
 * @return 授权状态或用户可读错误；在工作任务中调用。
 */
AuthorizationStatus GetAuthorization(DownloadGate& gate);

/**
 * @brief 读取未完成的扫码会话，使重新进入应用或掉电后能继续等待授权。
 * @return 只有会话标识、设备凭证和扫码地址全部存在时 ok 才为 true。
 */
AuthorizationSession LoadPendingSession();

/**
 * @brief 向Java创建扫码会话，主动换绑时用当前凭证证明设备授权，并先保存待授权凭证。
 * @param gate 页面拥有的可取消网络门闩。
 * @param switch_account 是否允许本次扫码更换绑定账号，默认仅普通续授权。
 * @return 可显示的扫码地址；本地保存失败时返回错误，防止授权后丢失凭证。
 */
AuthorizationSession CreateAuthorizationSession(DownloadGate& gate, bool switch_account = false);

/**
 * @brief 凭设备专属令牌轮询扫码会话，授权成功后清除本地待处理标记。
 * @param session 由当前设备创建的扫码会话。
 * @param gate 页面拥有的可取消网络门闩。
 * @return pending、authorizing、authorized、expired 或 failed 状态。
 */
AuthorizationStatus PollAuthorizationSession(const AuthorizationSession& session, DownloadGate& gate);

/**
 * @brief 使用本机设备授权直接向 Java 发送文字，跳过录音和语音识别。
 * @param text UTF-8 问题，去除首尾空白后最多 2000 字节，不允许空内容或内嵌空字符。
 * @param gate 页面拥有的取消门闩；离页终止请求，不自动重试。
 * @return 原问题、回答或用户可读错误；必须在网络工作任务中调用。
 */
TurnResult PostText(const std::string& text, DownloadGate& gate);

/**
 * @brief 读取默认历史页的原始消息，保留角色和真实消息编号，不发送指令。
 * @param gate 可取消请求门闩，离页终止本次查询。
 * @return 独立的用户和助理文本消息；空列表不代表电脑任务完成。
 */
TurnResult GetLocalMessages(DownloadGate& gate);

/**
 * @brief 只读取本次指令后的新回复；waiting为空回复，不代表网络故障或任务失败。
 * @param message_id 本次POST成功后返回的消息编号，不能为空。
 * @param gate 页面取消门闩。
 * @param timeout_ms 单次请求预算，限制在1至35秒，覆盖Java最长30秒的本地请求。
 * @return waiting或新助理消息；不发送电脑指令，不回退历史。
 */
TurnResult GetLocalReply(const std::string& message_id, DownloadGate& gate, int timeout_ms = 35000);

}  // namespace workbuddy
