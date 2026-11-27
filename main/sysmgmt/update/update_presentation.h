#pragma once

#include <functional>

// OTA 升级确认弹窗：系统级 Overlay，用于处理升级确认与忽略逻辑。
class OtaConfirmDialog {
public:
    enum class Mode {
        Boot,
        About,
        Sd,  // SD 卡本地固件：两键确认，文案仅当前版本
    };

    enum class Choice {
        Failed,  // UI 未能创建或被外部关闭；不能当成用户选择
        Upgrade,
        IgnorePersist,
        RemindLater,
    };

    using ChoiceCallback = std::function<void(Choice)>;

    /** @brief 阻塞等待用户选择，适用于开机升级确认流程。 */
    static Choice ShowBlocking(const char* current_version, const char* new_version, Mode mode);

    /** @brief 非阻塞展示并在用户选择后回调，适合关于页场景。 */
    static bool ShowAsync(const char* current_version, const char* new_version, Mode mode, ChoiceCallback cb);

    /** @brief 关闭升级页并解锁按键（失败路径） */
    static void Dismiss();

    /** @brief 是否处于激活/门禁状态 */
    static bool IsActive();
};


// OTA 全屏升级页：覆盖在当前页面上，显示版本信息和升级进度。
class OtaUpgradeScreen {
public:
    /** @brief 显示全屏升级页并等待首帧完成，超时后仍保持输入锁定。 */
    static bool Show(const char* current_version, const char* new_version);

    /** @brief 更新升级进度，线程安全且允许高频调用。 */
    static void SetProgress(int percent);

    /** @brief 更新底部提示文案。 */
    static void SetHint(const char* hint);

    /** @brief 关闭升级页并解锁按键，适用于失败路径。 */
    static void Dismiss();

    /** @brief 升级过程中是否启用输入门禁。 */
    static bool IsActive();
};
