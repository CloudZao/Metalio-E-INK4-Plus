#pragma once

#include <functional>

// OTA 升级确认弹窗：系统级 Overlay，用于处理升级确认与忽略逻辑。
class OtaConfirmDialog {
public:
    enum class Mode {
        Boot,
        About,
        Sd, // SD 卡本地固件：两键确认，文案仅当前版本
    };

    enum class Choice {
        Upgrade,
        IgnorePersist,
        RemindLater,
    };

    using ChoiceCallback = std::function<void(Choice)>;

    /** @brief 阻塞等待用户选择，适用于开机升级确认流程。 */
    static Choice ShowBlocking(const char* current_version, const char* new_version, Mode mode);

    /** @brief 非阻塞展示并在用户选择后回调，适合关于页场景。 */
    static bool ShowAsync(const char* current_version, const char* new_version, Mode mode,
                          ChoiceCallback cb);

    /** @brief 关闭升级页并解锁按键（失败路径） */
    static void Dismiss();
    /** @brief 是否处于激活/门禁状态 */
    static bool IsActive();
};
