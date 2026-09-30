#pragma once

#include "bluetooth_screen_priv.h"

/** @brief LV 异步：模式 3 设置完成 */
void Bluetooth_async_on_mode3_set(void* user_data);
/** @brief 投递清空设备列表 */
void Bluetooth_post_clear_list();
/** @brief 显示/隐藏模式 1 面板 */
void Bluetooth_show_mode1_panel(bool show);
/** @brief 点击通话模式 */
void Bluetooth_on_call_mode_clicked(lv_event_t* e);
/** @brief LV 异步：模式 1 设置完成 */
void Bluetooth_async_on_mode1_set(void* user_data);
/** @brief 显示/隐藏模式 2 面板 */
void Bluetooth_show_mode2_panel(bool show);
/** @brief 刷新模式按钮选中态 */
void Bluetooth_refresh_mode_buttons();
/** @brief 投递状态栏文案 */
void Bluetooth_post_status(const char* text);
/** @brief 点击复位蓝牙模块 */
void Bluetooth_on_reset_bt_clicked(lv_event_t* e);
/** @brief 向列表追加设备 */
void Bluetooth_add_device_to_list(const char* address, const char* name);
/** @brief LV 异步：模式 2 设置完成 */
void Bluetooth_async_on_mode2_set(void* user_data);
/** @brief 蓝牙复位后台任务 */
void Bluetooth_bt_reset_task(void* param);
/** @brief 去掉容器多余样式 */
void Bluetooth_strip_container(lv_obj_t* obj);
/** @brief LV 异步：复位完成后刷新 */
void Bluetooth_async_after_bt_reset(void* user_data);
/** @brief 更新状态标签文案 */
void Bluetooth_update_status_label(const char* text);
/** @brief LV 异步：更新状态标签 */
void Bluetooth_async_update_status(void* user_data);
/** @brief 点击扫描 */
void Bluetooth_on_scan_clicked(lv_event_t* e);
/** @brief 取蓝牙页 UI 状态 */
BluetoothUiState& Bluetooth_State();
/** @brief 点击音乐模式 */
void Bluetooth_on_music_mode_clicked(lv_event_t* e);
/** @brief LV 异步：清空列表 */
void Bluetooth_async_clear_list(void* user_data);
/** @brief 点击模式按钮 */
void Bluetooth_on_mode_btn_clicked(lv_event_t* e);
/** @brief 清空设备列表 UI */
void Bluetooth_clear_device_list_ui();
/** @brief LV 异步：追加设备项 */
void Bluetooth_async_add_device_item(void* user_data);
