#pragma once

/** @brief 通过显示门面更新提示与表情，可从后台任务调用 */
void UiShowSystemAlert(const char* status, const char* message, const char* emotion = "");

/** @brief 恢复空闲时钟、固定文案或待绑定提示 */
void UiRestoreIdleStatus();

/** @brief 在空闲状态恢复时钟或待绑定提示 */
void UiDismissSystemAlert();

/** @brief 恢复配网或激活状态提示，并按当前暂停状态请求激活检查 */
void UiRestoreProvisioningStatus();
