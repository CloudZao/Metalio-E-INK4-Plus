#pragma once
#include <atomic>
#include <memory>

enum class ChatCloudProvider;
enum class ConversationSettingsOperation { QueryTts, SaveTts, SaveProvider };

struct ConversationSettingsResult {
    ConversationSettingsOperation operation = ConversationSettingsOperation::QueryTts;
    std::atomic<bool> completed{false}; // 发布后读取操作结果
    bool ok = false;
    bool enabled = true;
    char error[96] = {};
};

using ConversationSettingsRequest = std::shared_ptr<const ConversationSettingsResult>;

/** @brief 查询播报状态，已有操作执行中或通道未接纳时返回空。 */
ConversationSettingsRequest ConversationSettingsQueryTts();

/** @brief 保存播报开关；本地配置提交与会话生效在语音通道完成。 */
ConversationSettingsRequest ConversationSettingsSaveTts(bool enabled);

/** @brief 保存云服务选择，成功提交后请求重启；离页不影响保存收尾。 */
ConversationSettingsRequest ConversationSettingsSaveProvider(ChatCloudProvider provider);

/** @brief 获取尚未完成的操作，重新进入的页面可继续展示其状态。 */
ConversationSettingsRequest ConversationSettingsCurrent();
