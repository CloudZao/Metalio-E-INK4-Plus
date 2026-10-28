#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include "power_policy.h"

namespace workbuddy {
/** @brief 管理 WorkBuddy 的音频占用与采集收尾，所有方法由音频工作任务串行调用 */
class AudioCapture {
public:
    AudioCapture() = default;
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;
    /** @brief 申请音频资源并注册 PCM 接收器；失败返回 false，并通过 error 区分未就绪、占用和启动失败。 */
    bool Start(std::function<bool(const int16_t*, size_t)> receiver, std::function<bool()> still_needed, std::string& error);
    /** @brief 停止采集并归还本模块的音频和电源占用，允许重复调用 */
    void Stop();
private:
    bool acquired_ = false; // 是否持有共享音频服务
    bool capturing_ = false; // 是否拥有当前 PCM 采集路由
    std::unique_ptr<PowerNeedHold> power_; // 录音期间的硬件电源占用
};
}
