#pragma once
#include <cstdint>
#include <string>
#include "sysmgmt/device_state.h"

// 会话表现端口：业务仅交付状态和意图，实现由UI应用提供，不暴露LVGL对象。
class ConversationPresentation {
public:
    /** @brief 当前页面是否在前台；后台消息会被忽略 */
    static bool IsActive();

    /** @brief 在 LVGL 线程暂停或恢复前台保网和语音连接；唤醒不自动开始录音 */
    static void SetStandby(bool standby);

    /** @brief PTT 是否仍保持按下；用于统一监听和松手判定 */
    static bool IsPttHeld();

    /** @brief 状态栏是否显示录音波形 */
    static bool IsPttWaveVisible();

    /** @brief 同步当前 PTT/监听状态到状态栏 */
    static void SyncPttOverlay();

    /** @brief 在 LVGL 任务中为本轮录音固定待发送照片 */
    static bool BeginPhotoListening(uint32_t turn);

    /** @brief 松手后上传本轮照片，再通知语音任务结束录音 */
    static bool EndPhotoListening(uint32_t turn);

    /** @brief 将单图提问的连接结果交给页面上传流程 */
    static bool UploadPhotoQuestion(uint32_t turn, bool ready);

    /** @brief 显示图片提问错误，并恢复当前照片供重试 */
    static void PhotoQuestionError(const std::string& image_id, bool reupload, bool disabled);

    /** @brief 图片回答结束后释放重试用原图 */
    static void FinishPhotoQuestion();

    /** @brief 异步打开页面；已在前台则忽略 */
    static void RequestOpen();

    /** @brief 向会话流追加消息；须在持 LVGL 锁时调用 */
    static void AddMessage(const char* role, const char* content);

    /** @brief 提交最后的语音回答快照并保存最终内容，结束本段流式状态。 */
    static void FinishYunxinSpeech();

    /** @brief 接收语音回答的累计文本快照，保存新增内容供后续刷新。 */
    static void AddYunxinSpeech(const char* text);

    /** @brief 将尚未绘制的语音回答快照更新到会话流；须在 LVGL 上下文调用。 */
    static void FlushYunxinSpeech();

    /** @brief 设置情绪表情（当前可为空操作） */
    static void SetEmotion(const char* emotion);

    /** @brief 经显示接口更新会话状态文案。 */
    static void SetStatus(const char* status);

    /** @brief 经显示接口交付一条会话消息。 */
    static void SetDisplayMessage(const char* role, const char* text);

    /** @brief 经显示接口更新表情。 */
    static void SetDisplayEmotion(const char* emotion);

    /** @brief 经显示接口提交通知。 */
    static void ShowNotification(const char* text);

    /** @brief 请求刷新联网等状态信息。 */
    static void RefreshStatusBar(bool force = false);

    /** @brief 将短提示提交给当前界面，须在UI回调中使用。 */
    static void Notify(const char* text);

    /** @brief 向有界UI队列投递不持有控件的通知回调。 */
    static bool Dispatch(void (*callback)(void*), bool urgent = false);

    /** @brief 查询用于对话按住说话的BOOT输入状态。 */
    static bool IsBootHeld();
};

/** @brief 呈现已接受的会话状态，音频和协议动作由会话模块执行 */
void UiPresentConversationState(DeviceState state);
