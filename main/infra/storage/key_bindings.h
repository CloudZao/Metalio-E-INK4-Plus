#pragma once
#include <cstdint>

enum class BindingKey : uint8_t {
    Boot,       // BOOT 实体键
    Power,      // 电源实体键
    VolumeUp,   // 音量加实体键
    VolumeDown, // 音量减实体键
    Previous,   // 底部上页触摸键
    Next,       // 底部下页触摸键
    Home,       // 底部 Home 触摸键
    Count       // 按键数量
};

enum class BindingAction : uint8_t {
    None,           // 不执行操作
    FullRefresh,    // 全刷一次
    Back,           // 返回上一层
    Home,           // 返回系统首页
    VolumeUp,       // 增加音量
    VolumeDown,     // 降低音量
    VolumeMax,      // 最大音量
    VolumeMin,      // 最小音量
    LightUp,        // 前光调亮
    LightDown,      // 前光调暗
    Standby,        // 进入待机
    Wake,           // 退出待机
    Mute,           // 切换静音
    Light,          // 切换前光
    OpenAi,         // 快捷进入 AI 应用
    Record,         // 按住录音
    ReaderSettings, // 阅读设置
    Interrupt,      // 打断播报
    Previous,       // 上一页
    Next,           // 下一页
    Shutdown,       // 软件关机
    Count           // 功能数量
};

using BindingMask = uint32_t;

/** @brief 获取功能对应的位掩码。 */
constexpr BindingMask BindingBit(BindingAction action) {
    return action == BindingAction::None ? 0 : BindingMask{1} << static_cast<unsigned>(action);
}

constexpr BindingMask kBindingLocalActions = BindingBit(BindingAction::OpenAi) | BindingBit(BindingAction::Record) |
    BindingBit(BindingAction::ReaderSettings) | BindingBit(BindingAction::Interrupt) |
    BindingBit(BindingAction::Previous) | BindingBit(BindingAction::Next); // 仅在适用页面执行的功能

/** @brief 切换所选功能，保留兼容功能并替换同一作用范围内的冲突功能。 */
BindingMask KeyBindingsCandidate(BindingMask current, BindingAction action);

enum class BindingSaveResult {
    Saved,    // 已持久化并应用
    Volatile, // 无卡，仅本次运行生效
    Invalid,  // 配置不符合约束
    IoError   // 存储失败，保留原配置
};

/** @brief 获取线程安全的绑定快照，long_press 为长按。 */
BindingMask KeyBindingsGet(BindingKey key, bool long_press);

/** @brief 检查单个绑定的硬件能力、手势限制与功能组合，不检查必留动作。 */
bool KeyBindingsCanBind(BindingKey key, bool long_press, BindingMask actions);

/** @brief 检查替换后的整套配置，保证必留动作及手势限制。 */
bool KeyBindingsCanSet(BindingKey key, bool long_press, BindingMask actions);

/** @brief 在存储工作线程持久化后应用绑定；无卡仅保存在内存，写失败保留原配置。 */
BindingSaveResult KeyBindingsSet(BindingKey key, bool long_press, BindingMask actions);

/** @brief 在存储工作线程恢复整套初始绑定，不改变手势开关；无卡时只恢复内存配置。 */
BindingSaveResult KeyBindingsReset();

/** @brief 查询左右滑动手势识别是否开启，初始为开启。 */
bool KeyBindingsGesturesEnabled();

/** @brief 在存储工作线程保存并应用手势开关；无卡仅本次生效，失败保留原值。 */
BindingSaveResult KeyBindingsSetGesturesEnabled(bool enabled);

/** @brief 在存储工作线程加载 SD 配置；缺文件时创建默认值，介质暂忙返回 false 供重试。 */
bool KeyBindingsPollStorage();

/** @brief 返回实体按键是否需要作为硬件唤醒输入。 */
bool KeyBindingsNeedsWake(BindingKey key);

/** @brief 查询所选板型是否具备该功能的硬件能力。 */
bool KeyBindingsActionSupported(BindingAction action);

/** @brief 获取指定手势在待机状态下的动作，优先处理关机。 */
BindingAction KeyBindingsStandbyAction(BindingKey key, bool long_press);
