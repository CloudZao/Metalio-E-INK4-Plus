#pragma once

#ifdef __cplusplus
extern "C" {
#endif

extern const char cloudzao_host[]; // 业务 API 基址
extern const char cloudzao_ota_url[]; // OTA 检查地址 
extern const char cloudzao_book_tools_url[]; // 书籍/字体转换工具页
extern const char cloudzao_path_sinicloud_token[]; // 同传 Token
extern const char cloudzao_path_asr_transcribe[]; // ASR 转写上传
extern const char cloudzao_path_asr_audio_records[]; // ASR 结果查询
extern const char cloudzao_path_checklist_items_all[]; // 全部待办
extern const char cloudzao_path_checklist_item_prefix[]; // 待办项前缀
extern const char cloudzao_path_checklist_item_complete_suffix[]; // 待办项完成后缀
extern const char cloudzao_path_weather_latest[]; // 实时天气
extern const char cloudzao_path_device_settings_tts[]; // TTS 偏好
extern const char cloudzao_path_push_resources_next[]; // 下一条推送
extern const char cloudzao_path_push_resources[]; // 推送队列删除
extern const char cloudzao_path_library_sync[]; // 书库同步
extern const char cloudzao_path_device_location_wifi[]; // WiFi 定位上报
extern const char cloudzao_path_vision_upload[];// 视觉上传

#ifdef __cplusplus
}
#endif
