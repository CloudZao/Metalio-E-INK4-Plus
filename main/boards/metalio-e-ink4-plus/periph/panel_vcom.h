#pragma once

#include <cstdint>
#include <vector>

struct PanelVcomState {
    int vcom_mv; // 蓝牙芯片最近确认的负毫伏值，0 表示尚未读取有效记录
    uint32_t revision; // 最近一次操作结果版本
    bool busy; // 正在处理读取或写入
    bool ok; // 最近一次本地蓝牙芯片操作是否成功
    char code[32]; // 不包含用户数据的稳定结果码
};

/** @brief 启动 USB 产测服务并读取蓝牙芯片校准记录 */
void PanelVcomInit();
/** @brief 接收蓝牙芯片串口数据，由持续运行的串口观察者调用 */
void PanelVcomOnUartData(const std::vector<uint8_t>& data);
/** @brief 获取供界面轮询的校准状态快照 */
PanelVcomState PanelVcomGetState();

/** @brief 异步查询蓝牙芯片校准记录，忙或未就绪时返回 false */
bool PanelVcomRequestRead();
