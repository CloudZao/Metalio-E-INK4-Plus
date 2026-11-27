#pragma once

#include <functional>

// ESP32-S3 / P4 / S31：将 SD 卡以 TinyUSB MSC 挂到电脑（虚拟 U 盘）。
// S3：启用时占用 USB OTG FS PHY（与 USJ 互斥）。S31：HS UTMI，与 USJ 分离。
// 未启用 CONFIG_TINYUSB_MSC_ENABLED 时提供空实现，UI 仅提示不可用。
class UsbVirtualDisk {
public:
    enum class UiHint {
        Idle,
        Switching,
        Enabling,
        Disabling,
        EnabledHost,
        EnabledLocal,
        Disabled,
        EnableFailed,
        DisableFailed,
        NoSdCard,
        FormatRequired,
        HostBusy,
        CameraHostBusy,
    };

    using UiNotifyFn = std::function<void()>;

    /** @brief 获取虚拟 U 盘常驻实例 */
    static UsbVirtualDisk& GetInstance();

    UsbVirtualDisk(const UsbVirtualDisk&) = delete;
    UsbVirtualDisk& operator=(const UsbVirtualDisk&) = delete;

    /** @brief 初始化请求队列并保持 USJ；业务请求到来时才创建工作任务。 */
    void Init();

    /** @brief 查询当前目标及配置是否提供 USB 大容量存储功能 */
    bool IsSupported() const;

    /** @brief 查询 USB 存储设备后端是否已启用 */
    bool IsGadgetActive() const;

    /** @brief 查询是否正在执行启用或停用操作 */
    bool IsBusy() const;

    /** @brief 查询 SD 卡是否已交给 USB 主机，交出时本机不可访问 */
    bool IsSdExportedToHost() const;

    /** @brief 获取最近一次切换操作的界面状态快照 */
    UiHint GetUiHint() const;

    /** @brief 异步切换启用状态，忙碌时更新目标状态供当前操作结束后处理 */
    void Toggle();

    /** @brief 异步停用已启用或正在切换的虚拟 U 盘，供离页时收尾 */
    void DisableIfActive();

    /** @brief 设置状态变化通知，回调须自行将刷新转交 UI 线程 */
    void SetUiNotify(UiNotifyFn fn);

    /** @brief 获取指定界面状态在当前语言下的操作提示 */
    static const char* HintText(UiHint hint);

private:
    UsbVirtualDisk() = default;
};
