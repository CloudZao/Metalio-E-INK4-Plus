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
    };

    using UiNotifyFn = std::function<void()>;

    static UsbVirtualDisk& GetInstance();

    UsbVirtualDisk(const UsbVirtualDisk&) = delete;
    UsbVirtualDisk& operator=(const UsbVirtualDisk&) = delete;

    // 创建 worker；启动时保持 USJ。可重复调用。
    void Init();

    bool IsSupported() const;
    bool IsGadgetActive() const;
    bool IsBusy() const;
    bool IsSdExportedToHost() const;
    UiHint GetUiHint() const;

    // 按钮切换：启用 / 停用虚拟 U 盘（异步；忙时可再点以改期望态）。
    void Toggle();

    // 若当前已启用（或正在切换），异步停用；离开设置页时调用。
    void DisableIfActive();

    // UI 注册；worker 完成后在 LVGL 线程外调用，UI 侧用 lv_async_call 刷新。
    void SetUiNotify(UiNotifyFn fn);

    // 中文提示文案。
    static const char* HintText(UiHint hint);

private:
    UsbVirtualDisk() = default;
};
