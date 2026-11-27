#pragma once

#include <string>
#include <cstdint>
#include <esp_err.h>

class YunxinBinding {
public:
    /** @brief 当前板型和云服务是否使用国际版扫码绑定 */
    static bool Enabled();

    /** @brief 当前国际版网易设备是否仍需完成扫码绑定 */
    static bool NeedsBinding();
    struct Snapshot {
        std::string qr_content;  // 当前绑定二维码数据
        bool request_failed;     // 最近请求是否失败
        uint32_t revision;       // 状态版本
    };

    /** @brief 拷贝绑定状态，不返回内部字符串引用 */
    static Snapshot GetSnapshot();

    /** @brief 是否已到下一次查询时间 */
    static bool IsDue();

    /** @brief 查询绑定状态并更新二维码；仅由串行网络任务调用，成功时写入 active */
    static esp_err_t Check(bool& active);
};

/** @brief 合并投递未绑定提示，仅提示一次。 */
void YunxinBindingNotifyUnlinked();
