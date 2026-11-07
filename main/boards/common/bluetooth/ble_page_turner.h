#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace BlePageTurner {
constexpr int kMaxDevices = 6;

enum class Action : uint8_t {
    kPrevious, // 上一页
    kNext, // 下一页
    kStandby, // 进入待机
};

enum class State : uint8_t {
    kIdle, // 等待扫描
    kStarting, // 初始化 BLE
    kConnecting, // 连接及订阅通知
    kConnected, // 通知已订阅
    kError, // 操作失败，可重新扫描
};

enum class Model : uint8_t {
    kFree2, // Free 2
    kFree3R, // Free3-R
    kSmart1E, // Smart 1-E
    kHbtr002X, // HBTR002-X
    kHbtr003X, // HBTR003-X
    kCount, // 机型数量，不用于持久化
};

/** @brief 获取翻页器广播名称 */
constexpr const char* ModelName(Model model) {
    switch (model) {
        case Model::kFree2: return "Free 2";
        case Model::kFree3R: return "Free3-R";
        case Model::kSmart1E: return "Smart 1-E";
        case Model::kHbtr002X: return "HBTR002-X";
        case Model::kHbtr003X: return "HBTR003-X";
        default: return "";
    }
}

/** @brief 判断机型是否已有可学习的翻页按键协议 */
constexpr bool SupportsButtonLearning(Model model) {
    return model == Model::kFree2 || model == Model::kFree3R || model == Model::kSmart1E ||
        model == Model::kHbtr002X || model == Model::kHbtr003X;
}

struct Device {
    char address[18] = {}; // 设备地址
    Model model = Model::kFree2; // 扫描识别的型号
    int8_t rssi = -127; // 最近扫描的信号强度
    int saved_index = -1; // 对应已保存设备，-1 为未保存
};

struct SavedDevice {
    char address[18] = {}; // 保存的设备身份地址
    uint8_t keys[2] = {1, 2}; // 各功能绑定的输入编号，0 为未设置
    Model model = Model::kFree2; // 保存的设备型号
    bool nearby = false; // 最近扫描发现或当前已连接
    int8_t rssi = -127; // 最近扫描的信号强度
};

struct Snapshot {
    State state = State::kIdle; // 当前连接状态
    bool enabled = false; // 自动连接开关，手动连接成功后保持开启
    bool scanning = false; // 扫描独立于连接，可在已连接时刷新附近设备
    bool searching = false; // 正在进行限时自动连接
    bool timed_out = false; // 自动连接超时并关闭
    bool binding_pending = false; // 按键绑定正在保存
    bool operation_failed = false; // 最近的绑定保存失败
    char target_address[18] = {}; // 当前连接或正在连接的设备地址
    Model target_model = Model::kFree2; // 当前连接或正在连接的型号
    Device devices[kMaxDevices]; // 有界扫描结果
    int count = 0; // 扫描结果数量
    uint32_t revision = 0; // 状态变化序号
    std::shared_ptr<const std::vector<SavedDevice>> saved; // 按状态变化发布的不可变设备列表
    int saved_count = 0; // 已保存设备数量
    int active_saved = -1; // 当前已连接设备的保存索引
    bool learning = false; // 学习期间隔离业务输入
    uint8_t learned_key = 0; // 最近识别的输入编号

    /** @brief 获取当前快照持有的已保存设备列表 */
    const std::vector<SavedDevice>& Saved() const {
        static const std::vector<SavedDevice> empty;
        return saved ? *saved : empty;
    }
};

enum class KeyPhase : uint8_t {
    kClick, // 短按
    kCancel, // 取消未完成输入
};
using CommandHandler = void (*)(Action action, KeyPhase phase); // BLE 线程回调，不得同步操作 UI

/** @brief 初始化翻页输入回调并加载持久化设置，不启动扫描 */
void Init(CommandHandler handler);
/** @brief 处理限时自动连接；由现有 UI 轮询调用，不阻塞 */
void Poll();
/** @brief 设置自动连接开关；关闭时停止扫描并断开，持久化写入由 BLE 任务处理 */
bool SetEnabled(bool enabled);
/** @brief 进入待机暂停蓝牙活动，退出待机按原开关重新搜索 */
void SetStandby(bool standby);
/** @brief 获取翻页器状态及扫描结果的副本 */
Snapshot GetSnapshot();
/** @brief 异步扫描可识别翻页器；后台刷新保留上一轮结果，完成后移除未发现设备 */
bool Scan(CommandHandler handler, bool keep_results = false);
/** @brief 取消设备管理扫描及待执行的扫描请求，不断开连接或关闭自动连接开关 */
void StopScan();
/** @brief 按地址异步连接扫描列表中的设备，避免后台刷新导致条目索引变化 */
bool Connect(const char* address);
/** @brief 按地址异步连接已保存设备，无需重新扫描 */
bool ConnectSaved(const char* address);
/** @brief 按地址异步忘记设备、配对密钥及按键配置；活动连接将断开 */
bool Forget(const char* address);
/** @brief 开始或结束当前连接的按键学习，开始时清除上次识别结果 */
bool SetLearning(bool enabled);
/** @brief 保存指定功能的输入编号；上一页与下一页互斥 */
bool Bind(Action action, uint8_t key);
/** @brief 异步取消扫描、连接或断开当前设备 */
void Disconnect();
/** @brief 浅睡前停止 BLE 并释放协议栈；须从非 UI、非 BLE 任务调用，失败时应推迟浅睡 */
bool PrepareSleep();
} // namespace BlePageTurner
