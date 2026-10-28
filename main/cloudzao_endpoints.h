#pragma once

#ifdef __cplusplus
extern "C" {
#endif

extern const char cloudzao_log_upload_host[];
extern const char cloudzao_path_device_logs[];
extern const char cloudzao_host[]; // 业务 API 基址
extern const char cloudzao_ota_url[]; // OTA 检查地址 
extern const char xiaozhi_ota_url[]; // 小智 OTA 检查地址
extern const char yunxin_ota_url[]; // 云信 OTA 检查地址
extern const char yunxin_websocket_url[]; // 云信 WebSocket 基址
extern const char yunxin_app_key[]; // 云信应用标识
extern const char yunxin_token[]; // 云信 WebSocket Token
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

extern const char cloudzao_workbuddy_host[]; // WorkBuddy 服务配置
extern const char cloudzao_path_workbuddy_authorization[]; // WorkBuddy 设备授权
extern const char cloudzao_path_workbuddy_authorization_sessions[]; // WorkBuddy 扫码授权会话
extern const char cloudzao_path_workbuddy_text_turn[]; // WorkBuddy 服务配置
extern const char cloudzao_path_workbuddy_asr_stream[]; // WorkBuddy 服务配置
extern const char cloudzao_path_workbuddy_local_messages[]; // WorkBuddy 服务配置
extern const char cloudzao_path_workbuddy_local_status[]; // WorkBuddy 服务配置
extern const char cloudzao_path_workbuddy_local_reply[]; // WorkBuddy 服务配置

#ifdef __cplusplus
}
#endif
