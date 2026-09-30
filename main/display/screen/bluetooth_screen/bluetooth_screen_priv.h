#pragma once

#include "bluetooth_screen.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <lvgl.h>

constexpr const char* TAG = "BtScreen";
constexpr int kAddrHexLen = 12; // 地址十六进制长度
constexpr bool kBtPowerResetSupported = false; // 是否支持断电复位

enum class BtMode : uint8_t {
    kNone = 0, // 未选
    kMode1, // 模式 1
    kMode2, // 模式 2
    kMode3, // 模式 3
};

enum class ConnState : uint8_t {
    kIdle, // 空闲
    kScanning, // 扫描中
    kConnecting, // 连接中
    kConnected, // 已连接
};

struct BtDevice {
    char address[kAddrHexLen + 1]; // 设备地址
    char name[64]; // 设备名
};

struct BluetoothUiState {
    lv_obj_t* root = nullptr; // 根容器
    lv_obj_t* status_label = nullptr; // 状态文案
    lv_obj_t* mode_btns[3] = {}; // 模式按钮
    lv_obj_t* mode1_panel = nullptr; // 模式 1 面板
    lv_obj_t* mode2_panel = nullptr; // 模式 2 面板
    lv_obj_t* scan_btn = nullptr; // 扫描按钮
    lv_obj_t* device_list = nullptr; // 设备列表
    lv_obj_t* music_btn = nullptr; // 音乐模式按钮
    lv_obj_t* call_btn = nullptr; // 通话模式按钮
};

extern std::atomic<bool> Bluetooth_mode_cmd_busy; // 模式命令忙
/** @brief 取蓝牙页 UI 状态 */
BluetoothUiState& Bluetooth_State();
extern BtMode Bluetooth_active_mode; // 当前模式
extern ConnState Bluetooth_conn_state; // 连接状态
extern std::string Bluetooth_rx_buffer; // UART 收缓冲
extern std::vector<BtDevice> Bluetooth_devices; // 已发现设备
extern bool Bluetooth_screen_active; // 页是否激活

struct AsyncStatusMsg {
    char text[128]; // 状态文案
};

struct AsyncAddDeviceMsg {
    char address[kAddrHexLen + 1]; // 设备地址
    char name[64]; // 设备名
};

struct ModeCmdArgs {
    BtMode mode; // 目标模式
};

/** @brief 更新状态标签文案 */
void Bluetooth_update_status_label(const char* text);
/** @brief 投递状态栏文案 */
void Bluetooth_post_status(const char* text);
/** @brief 刷新模式按钮选中态 */
void Bluetooth_refresh_mode_buttons();
/** @brief 显示/隐藏模式 2 面板 */
void Bluetooth_show_mode2_panel(bool show);
/** @brief 显示/隐藏模式 1 面板 */
void Bluetooth_show_mode1_panel(bool show);
/** @brief 向列表追加设备 */
void Bluetooth_add_device_to_list(const char* address, const char* name);
/** @brief 投递清空设备列表 */
void Bluetooth_post_clear_list();
/** @brief 下发模式切换命令 */
void Bluetooth_send_mode_command(BtMode mode);
/** @brief 按当前模式恢复面板显示 */
void Bluetooth_restore_mode_ui();
/** @brief 复位蓝牙页 UI 状态 */
void Bluetooth_reset_ui_state();
/** @brief 创建操作按钮 */
lv_obj_t* Bluetooth_make_action_button(lv_obj_t* parent, const char* label, lv_event_cb_t cb);
/** @brief 创建模式切换按钮 */
lv_obj_t* Bluetooth_make_mode_button(lv_obj_t* parent, const char* label, int idx);
