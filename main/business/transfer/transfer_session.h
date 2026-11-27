#pragma once
#include <cstdint>
#include "ap_transfer.h"

enum class TransferSessionMode : int8_t {
    Standby = -2, // 关闭服务并阻止重新启动
    Closed = -1, // 关闭会话并恢复原网络
    Cloud = 0, // 云资源入口
    Hotspot = 1, // 本地热点
    Usb = 2, // USB介质交接
    Wifi = 3, // 局域网文件传输
};

struct TransferSessionSnapshot {
    uint32_t generation = 0;
    TransferSessionMode mode = TransferSessionMode::Closed;
    bool ready = false;
    bool ok = false;
    ApTransferInfo info{};
};

/** @brief 提交传输模式切换，合并旧请求；未接纳返回0，关闭复用已保留的会话任务。 */
uint32_t TransferSessionRequest(TransferSessionMode mode);

/** @brief 获取传输会话的状态快照，不访问页面或等待网络操作。 */
TransferSessionSnapshot TransferSessionRead();
