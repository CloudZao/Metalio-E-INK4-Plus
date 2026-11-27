#pragma once

/** @brief 绑定重启前的会话收尾入口，启动阶段调用一次 */
void SystemControlInitialize(void (*prepare_restart)());

/** @brief 请求重启，队列满时保留请求并由系统节拍重试 */
bool SystemRequestReboot();

/** @brief 交付尚未执行的重启请求 */
void SystemControlPoll();
