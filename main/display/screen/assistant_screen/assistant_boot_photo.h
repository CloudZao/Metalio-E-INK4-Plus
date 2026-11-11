#pragma once

#include <cstdint>
#include <string>

/**
 * @brief 百问 AI：申请拍摄一张待发送照片
 * @return true 表示已消费双击
 */
bool AssistantBootPhoto_OnDoubleClick();

struct _lv_obj_t;
/** @brief 创建待发送照片区域，宿主为百问页面 */
void AssistantPhoto_Create(_lv_obj_t* screen);
/** @brief 离页清理照片及控件引用，使异步任务失效 */
void AssistantPhoto_Reset();
/** @brief 待机时使异步任务失效并关闭大图，保留待发送照片 */
void AssistantPhoto_Suspend();
/** @brief 关闭大图；返回是否消费返回操作 */
bool AssistantPhoto_ClosePreview();
/** @brief 检查本地照片操作是否允许开始录音，并显示必要提示 */
bool AssistantPhoto_AllowListening();
/** @brief 查询当前是否有待发送照片，仅在 LVGL 线程调用 */
bool AssistantPhoto_HasPending();
/** @brief 后台上传并提交当前照片问题，提交成功后清除待发送照片 */
void AssistantPhoto_Send();

/** @brief 固定本轮录音附件，正在上传的照片不关联到新录音 */
void AssistantPhoto_BeginListening(uint32_t turn);
/** @brief 录音松手后上传附件，并提交对应轮次的结束消息 */
void AssistantPhoto_EndListening(uint32_t turn);
/** @brief 接收单图提问的 WebSocket 就绪结果 */
void AssistantPhoto_UploadQuestion(uint32_t turn, bool ready);

/** @brief 恢复被服务端拒绝的当前照片，必要时重新上传 */
void AssistantPhoto_QuestionError(const std::string& image_id, bool reupload, bool disabled);

/** @brief 回答结束后释放用于服务端错误重试的原图 */
void AssistantPhoto_FinishQuestion();
