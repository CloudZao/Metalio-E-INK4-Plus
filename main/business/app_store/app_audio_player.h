#pragma once
#include <memory>
#include "app_api.h"
#include "app_store.h"

struct AppAudioPlayer;

/** @brief 创建与已加载应用绑定的宿主音频会话，不启动后台播放 */
std::shared_ptr<AppAudioPlayer> AppAudioCreate(const AppStoreItem& item);

/** @brief 查询宿主实际支持的格式、来源与输出参数 */
int AppAudioGetCaps(xz_app_audio_caps_t* result);

/** @brief 接纳播放请求并在后台打开文件、解码和输出 */
int AppAudioPlay(std::shared_ptr<AppAudioPlayer>& player, const xz_app_audio_config_t* config);

/** @brief 暂停或继续当前播放；暂停会关闭文件并归还音频硬件 */
int AppAudioPause(const std::shared_ptr<AppAudioPlayer>& player, bool paused);

/** @brief 请求停止当前播放；后台任务自行收尾，不引用 ELF 回调 */
int AppAudioStop(const std::shared_ptr<AppAudioPlayer>& player);

/** @brief 更新前台状态；离页或待机时暂停文件和音频访问 */
void AppAudioSetForeground(const std::shared_ptr<AppAudioPlayer>& player, bool foreground);

/** @brief 更新系统音量，范围 0—100，与系统音量键共用 */
int AppAudioSetVolume(uint32_t volume);

/** @brief 查询播放状态及系统音量，按调用方结构容量复制 */
int AppAudioGetStatus(const std::shared_ptr<AppAudioPlayer>& player, xz_app_audio_status_t* result);

/** @brief 查询播放任务是否仍在运行或收尾 */
bool AppAudioBusy(const std::shared_ptr<AppAudioPlayer>& player);
