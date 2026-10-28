#pragma once

#include "bluetooth_screen_priv.h"

/** @brief 解析扫描到的设备行（地址+名称） */
bool Bluetooth_parse_bt_device_line(const std::string& line, char* address, size_t addr_sz, char* name,
                                    size_t name_sz);
/** @brief 下发模式切换 AT/命令 */
void Bluetooth_send_mode_command(BtMode mode);
/** @brief 阻塞下发模式指令，供复位后恢复使用；调用方持有命令忙标记 */
void Bluetooth_apply_mode_commands(BtMode mode);
/** @brief 更新模式的功放需求；模式3跨页面持有 */
void Bluetooth_set_mode_audio_power(BtMode mode);
/** @brief 离开页面仅释放非模式3的音频需求 */
void Bluetooth_release_page_audio_power();
/** @brief 查询蓝牙固件版本，命令发送成功返回 true */
bool Bluetooth_query_version();
/** @brief UART 收包回调 */
void Bluetooth_on_uart_data(const std::vector<uint8_t>& data);
/** @brief 复位蓝牙页 UI 状态 */
void Bluetooth_reset_ui_state();
/** @brief 创建操作按钮 */
lv_obj_t* Bluetooth_make_action_button(lv_obj_t* parent, const char* label, lv_event_cb_t cb);
/** @brief 是否为十六进制字符 */
bool Bluetooth_is_hex_char(char c);
/** @brief 音乐模式切换后台任务 */
void Bluetooth_music_mode_task(void* param);
/** @brief 处理一行 UART 响应 */
void Bluetooth_handle_response_line(const std::string& raw_line);
/** @brief 按当前模式恢复面板显示 */
void Bluetooth_restore_mode_ui();
/** @brief 通话模式切换后台任务 */
void Bluetooth_call_mode_task(void* param);
/** @brief 创建模式切换按钮 */
lv_obj_t* Bluetooth_make_mode_button(lv_obj_t* parent, const char* label, int idx);
/** @brief 去掉行首尾空白 */
void Bluetooth_trim_line(std::string& line);
/** @brief 通用模式命令后台任务 */
void Bluetooth_mode_cmd_task(void* param);
