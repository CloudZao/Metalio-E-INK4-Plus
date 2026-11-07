#pragma once

#include "bluetooth_screen.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <lvgl.h>

constexpr const char* TAG = "BtScreen";
constexpr int kAddrHexLen = 12; // 地址十六进制长度

enum class BtMode : uint8_t {
    kNone = 0, // 未选
    kMode1, // 模式 1
    kMode2, // 模式 2
    kMode3, // 模式 3
    kMode5 = 5, // 蓝牙键盘与语音
};

constexpr BtMode kBluetoothModes[] = {BtMode::kMode1, BtMode::kMode2, BtMode::kMode3, BtMode::kMode5}; // 界面模式顺序
constexpr int kBluetoothModeCount = sizeof(kBluetoothModes) / sizeof(kBluetoothModes[0]); // 模式数量

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
    lv_obj_t* name_label = nullptr; // 经典蓝牙名称
    lv_obj_t* current_mode_label = nullptr; // 当前模式说明
    lv_obj_t* version_label = nullptr; // 蓝牙固件版本
    lv_obj_t* version_hint_label = nullptr; // 固件刷新提示。
    lv_obj_t* check_version_btn = nullptr; // 固件版本刷新行
    lv_obj_t* mode_btns[kBluetoothModeCount] = {}; // 模式按钮
    lv_obj_t* mode_description_label = nullptr; // 当前选中模式的说明
    lv_obj_t* mode2_entry = nullptr; // 蓝牙设备管理入口。
    lv_obj_t* mode2_panel = nullptr; // 模式 2 面板
    lv_obj_t* hid_panel = nullptr; // 模式 5 HID 测试入口
    lv_obj_t* scan_btn = nullptr; // 扫描按钮
    lv_obj_t* device_list = nullptr; // 设备列表
    lv_obj_t* music_btn = nullptr; // 音乐模式按钮
    lv_obj_t* call_btn = nullptr; // 通话模式按钮
};

extern std::atomic<bool> Bluetooth_mode_cmd_busy; // 模式命令忙
/** @brief 取蓝牙页 UI 状态 */
BluetoothUiState& Bluetooth_State();
extern std::atomic<BtMode> Bluetooth_active_mode; // 当前模式
extern std::atomic<ConnState> Bluetooth_conn_state; // 连接状态
extern std::mutex Bluetooth_rx_mutex; // 保护接收缓冲和扫描结果，不覆盖 UART 发送或硬件操作
extern std::string Bluetooth_rx_buffer; // UART 收缓冲
extern std::vector<BtDevice> Bluetooth_devices; // 已发现设备
extern std::atomic<bool> Bluetooth_screen_active; // UI 与 UART 任务共享的页面激活状态

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
/** @brief 将蓝牙版本号投递到界面线程 */
void Bluetooth_post_version(const char* version);
/** @brief 将模块名称投递到界面线程 */
void Bluetooth_post_name(const char* name);
/** @brief 刷新模式按钮选中态 */
void Bluetooth_refresh_mode_buttons();
/** @brief 显示模式 2 设备管理入口，隐藏时收起管理页 */
void Bluetooth_show_mode2_panel(bool show);
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
