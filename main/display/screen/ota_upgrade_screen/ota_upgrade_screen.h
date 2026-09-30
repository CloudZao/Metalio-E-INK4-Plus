#pragma once

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
