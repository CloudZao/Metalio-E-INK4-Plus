#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

constexpr int kAddrHexLen = 12;
enum class BtMode : uint8_t { kNone = 0, kMode1, kMode2, kMode3, kMode5 = 5 };
enum class ConnState : uint8_t { kIdle, kScanning, kConnecting, kConnected };

struct BtDevice {
    char address[kAddrHexLen + 1] = {};
    char name[64] = {};
};

struct BluetoothControlSnapshot {
    uint32_t revision = 0;
    uint32_t devices_revision = 0;
    char status[128] = {};
    char name[64] = {};
    std::vector<BtDevice> devices;
};

/** @brief 接入UART附加接收；控制能力持续接收在途操作的确认，不依赖页面。 */
void BluetoothStartControl();

/** @brief 读取发生变更的状态快照，无变更时不复制设备列表。 */
bool BluetoothReadControl(BluetoothControlSnapshot& snapshot);

/** @brief 查询当前已确认的模块模式。 */
BtMode BluetoothCurrentMode();

/** @brief 查询设备连接状态。 */
ConnState BluetoothConnectionState();

/** @brief 查询是否有模式、改名或复位操作尚未收尾。 */
bool BluetoothCommandsBusy();

/** @brief 请求切换模块模式，投递失败保留原模式并归还忙碌状态。 */
bool Bluetooth_send_mode_command(BtMode mode);

/** @brief 查询模块名称，串口事务繁忙时返回false。 */
bool Bluetooth_query_name();

/** @brief 校验经典蓝牙名称长度和控制字符。 */
bool Bluetooth_is_valid_name(const char* name);

/** @brief 提交名称修改及模式恢复，离页后继续完成操作。 */
bool Bluetooth_rename(const char* name);

/** @brief 发送界面预设的HID指令，释放按键时允许越过模式忙标记。 */
bool Bluetooth_send_hid_shortcut(const char* shortcut);

/** @brief 提交耳机扫描。 */
bool BluetoothScanDevices();

/** @brief 提交连接已发现的设备，地址须为12位十六进制。 */
bool BluetoothConnectDevice(const char* address);

/** @brief 提交通话或音乐协议切换，保留模式3的跨页面功放需求。 */
bool BluetoothSetCallMode(bool call);

/** @brief 提交模块复位；复位前等待调用方提供的安全窗口，不借用页面。 */
bool BluetoothResetModule(std::function<bool()> ready);

/** @brief 离页时归还非模式3的临时功放需求。 */
void Bluetooth_release_page_audio_power();
