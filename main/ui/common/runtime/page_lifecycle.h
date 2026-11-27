#pragma once
#include <atomic>
#include <cstdint>
#include <lvgl.h>

enum class UiPagePowerEvent : uint8_t {
    StopTransfers,
    PrepareOverlay,
    PauseApplications, // SD仍准入；业务停止生产并交付已接纳的写入
    PauseViews, // 普通SD准入已关闭；页面停止更新，工作所有者继续收尾
    ResumeCalibration,
    ResumeApplications, // SD及硬件已恢复；恢复业务入口
    ResumeViews, // 恢复界面更新，不撤销仍在进行的资源收尾
    PauseImages,
    ResumeImages,
    FinishResume, // 控件树恢复完成，可恢复图片与传输入口；物理首帧另行等待
};

struct UiPageHooks {
    void (*update)() = nullptr; // 只对当前页面或有效叠加层更新
    void (*power)(UiPagePowerEvent) = nullptr; // UI线程非阻塞发出暂停/恢复请求；返回不表示后台已退出
    void (*leave)() = nullptr; // 卸载后撤销页面接收与定时更新
    uint32_t update_ms = 0; // 零表示不需要周期更新
};

class UiPageLifecycle {
public:
    /** @brief 绑定页面或叠加层；重绑和离页自动作废旧身份，实例须比控件树存活更久。 */
    void Bind(lv_obj_t* root, UiPageHooks hooks = {}, bool overlay = false);

    /** @brief 解除绑定，不销毁控件；允许重复调用。 */
    void Unbind();

    /** @brief 获取可交给后台结果的页面身份。 */
    uint32_t Generation() const;

    /** @brief 检查异步结果是否仍属于当前页面。 */
    bool IsCurrent(uint32_t generation) const;

    /** @brief 在UI任务分发当前页面及叠加层更新。 */
    static void Poll();

    /** @brief 在UI任务按系统既有顺序转发电源事件。 */
    static void NotifyPower(UiPagePowerEvent event);

    UiPageLifecycle() = default;
    UiPageLifecycle(const UiPageLifecycle&) = delete;
    UiPageLifecycle& operator=(const UiPageLifecycle&) = delete;

private:
    static void OnEvent(lv_event_t* event);
    lv_obj_t* root_ = nullptr;
    UiPageHooks hooks_{};
    UiPageLifecycle* next_ = nullptr;
    std::atomic<uint32_t> generation_{0};
    std::atomic<bool> active_{false};
    uint32_t last_update_ = 0;
    bool overlay_ = false;
    bool paused_ = false;
};
