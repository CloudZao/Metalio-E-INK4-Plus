#pragma once
#include <memory>
#include <string>
#include "app_api.h"

struct AppAudioRecorder;

/** @brief 创建应用私有单录音槽，后台探测已保存录音 */
std::shared_ptr<AppAudioRecorder> AppRecordCreate(const std::string& storage_key);

/** @brief 查询当前硬件支持的录音格式及输入参数 */
int AppRecordCaps(xz_app_audio_caps_t* result);

/** @brief 接纳一次新录音，写临时文件，停止成功后覆盖上一条 */
int AppRecordStart(const std::shared_ptr<AppAudioRecorder>& recorder);

/** @brief 暂停或继续写入，暂停时继续排空输入，保留麦克风归属 */
int AppRecordPause(const std::shared_ptr<AppAudioRecorder>& recorder, bool paused);

/** @brief 请求停止录音并异步保存 */
int AppRecordStop(const std::shared_ptr<AppAudioRecorder>& recorder);

/** @brief 获取状态、有效录音时长与已保存文件路径 */
int AppRecordStatus(const std::shared_ptr<AppAudioRecorder>& recorder, xz_app_record_status_t* result);

/** @brief 查询是否仍在准备、录制或保存 */
bool AppRecordBusy(const std::shared_ptr<AppAudioRecorder>& recorder);

/** @brief 更新前台状态；失去前台时结束并保存当前录音 */
void AppRecordForeground(const std::shared_ptr<AppAudioRecorder>& recorder, bool foreground);

/** @brief 关闭宿主会话，后台保存并归还资源，不引用 ELF 内存 */
void AppRecordClose(const std::shared_ptr<AppAudioRecorder>& recorder);
