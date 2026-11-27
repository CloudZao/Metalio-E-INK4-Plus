#pragma once

#include <functional>
#include <string>

class Board;

struct McpToolActions {
    std::function<bool()> reboot;                     // 可选重启请求，返回系统是否接纳
    std::function<bool(const std::string&)> upgrade;  // 可选升级请求，返回系统是否接纳
};

/**
 * @brief 绑定 MCP 会话传输并注册设备工具，启动阶段调用一次
 * @note board 须保持有效至工具会话停止；系统动作由回调提交，视觉配置由本模块持有。
 */
void InitializeDeviceMcpTools(Board& board, const McpToolActions& actions,
                              std::function<void(const std::string&)> send);
