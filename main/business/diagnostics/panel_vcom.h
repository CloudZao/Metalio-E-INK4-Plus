#pragma once

#include <cstdint>

struct PanelVcomState {
    int vcom_mv;        // 蓝牙芯片最近确认的负毫伏值，0 表示尚未读取有效记录
    uint32_t revision;  // 最近一次操作结果版本
    bool busy;          // 正在处理读取或写入
    bool ok;            // 最近一次本地蓝牙芯片操作是否成功
    char code[32];      // 不包含用户数据的稳定结果码
};

struct PanelVcomBindings {
    bool (*apply)(int mv);                          // 安全应用面板电压
    int (*current)();                               // 查询已应用电压
    void (*notify)(bool write, const char* error);  // 可选的后台安全结果通知
    bool (*enable_saved_depth)();                   // 无有效校准时启用已保存的黑色深度档位
};

/** @brief 启动校准产测流程，显示、通知和备用档位由板级装配提供。 */
void PanelVcomInit(PanelVcomBindings bindings);

/** @brief 获取供界面轮询的校准状态快照 */
PanelVcomState PanelVcomGetState();

/** @brief 异步查询校准记录，忙或未就绪时返回 false */
bool PanelVcomRequestRead();
