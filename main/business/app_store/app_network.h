#pragma once
#include <memory>
#include "app_api.h"

struct AppNetwork;

/** @brief 创建只由宿主持有的应用网络会话。 */
std::shared_ptr<AppNetwork> AppNetworkCreate();

/** @brief 非阻塞撤销全部连接；closed 为真时永久关闭会话。 */
void AppNetworkStop(const std::shared_ptr<AppNetwork>& state, bool closed);

/** @brief 更新应用前台状态；使用过联网能力且未启用空闲计时时持续软保网。 */
void AppNetworkForeground(const std::shared_ptr<AppNetwork>& state, bool foreground);

/** @brief 开关应用空闲断网计时；开启后归还页面保网引用，不修改系统设置。 */
int AppNetworkSetIdleTimer(const std::shared_ptr<AppNetwork>& state, int enabled);

/** @brief 异步准备网络，不创建业务连接；通过网络句柄查询完成或关闭取消。 */
int AppNetworkPrepare(const std::shared_ptr<AppNetwork>& state, uint32_t timeout_ms, uint32_t* id);

/** @brief 异步解析首个 IPv4，复用网络句柄和生命周期。 */
int AppNetworkResolve(const std::shared_ptr<AppNetwork>& state, const char* hostname, uint32_t timeout_ms, uint32_t* id);

/** @brief 异步执行 ICMP Echo，复制应用配置并在取消后归还资源。 */
int AppNetworkPing(const std::shared_ptr<AppNetwork>& state, const xz_app_ping_config_t* config, uint32_t* id);

/** @brief 按调用方结构容量复制地址解析结果或 Ping 统计。 */
int AppNetworkDiagnostics(const std::shared_ptr<AppNetwork>& state, uint32_t id, xz_app_net_diagnostics_t* result);

/** @brief 异步连接或提交 HTTP 请求，复制所有应用输入。 */
int AppNetworkOpen(const std::shared_ptr<AppNetwork>& state, const xz_app_net_config_t* config, uint32_t* id);

/** @brief 复制一条待发送负载，不阻塞 UI。 */
int AppNetworkSend(const std::shared_ptr<AppNetwork>& state, uint32_t id, const void* data,
                   uint32_t length, uint32_t flags);

/** @brief 按调用方容量读取连接状态。 */
int AppNetworkStatus(const std::shared_ptr<AppNetwork>& state, uint32_t id, xz_app_net_status_t* result);

/** @brief TCP/TLS 按容量复制接收字节；其他协议复制完整记录，容量不足时保留原记录。 */
int AppNetworkRead(const std::shared_ptr<AppNetwork>& state, uint32_t id, void* buffer,
                   uint32_t capacity, uint32_t* flags);

/** @brief 复制已保存的 HTTP 响应头，返回字节数或负错误码。 */
int AppNetworkHeader(const std::shared_ptr<AppNetwork>& state, uint32_t id, const char* name,
                     char* buffer, uint32_t capacity);

/** @brief 撤销句柄并请求后台关闭连接，允许后台继续安全收尾。 */
int AppNetworkClose(const std::shared_ptr<AppNetwork>& state, uint32_t id);

/** @brief 查询包含离页收尾在内的应用网络后台工作。 */
bool AppNetworkBusy();
