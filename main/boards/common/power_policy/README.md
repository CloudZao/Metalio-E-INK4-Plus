# 低功耗策略（S31）

业务经 `PowerPolicy::GetInstance()` 声明需求与活动；**勿经 Board 转发**。策略评估后调用 `power_hw_*` 原子动作。

| 文件 | 职责 |
|------|------|
| `power_policy.h` | 对外 API / `PowerNeed` |
| `power_policy.cc` | 调度：计时、档位评估、NVS、WiFi/PA/CPU 投递 |
| `power_policy_standby_lp.cc` | 待机 Overlay 后切片浅睡会话 |
| `power_hw.*` | 硬件原子（PA / WiFi pause / CPU / 浅睡 / 关机脉冲） |
| `power_policy_priv.h` | 目录内部共享状态 |

---

## 档位

```text
PowerNeed + DeviceState + 无操作计时
        │
        ▼
  ReevaluateLocked → Mode
        │
   ┌────┼────────────┬────────────┐
   ▼    ▼            ▼            ▼
 Full  NetActive   AppIdle    StandbyUi
```

| Mode | 进入条件（要点） | 硬件动作 |
|------|------------------|----------|
| **Full** | `PhoneCall` / `BtAudio` | 满频 + PA |
| **NetActive** | 硬占网 / 软保网 / 断网暂留未满 / 配网激活 | 满频 + 保 WiFi；PA 另算 |
| **AppIdle** | `Idle` 且无占网、暂留已满 | 关 PA、pause WiFi、MAIN_PWR 态掉电、CPU→空闲 MHz |
| **StandbyUi** | 无操作满阈值，或电源键短按进待机 | Overlay → settle/Park → `standby_lp` 切片浅睡 |

`KeepNet` 为保留档，当前评估不选用。

### PA

- 开：`Full`，或 `NetActive` 且 `DeviceState` 为 Speaking / AudioTesting
- 关：其余；`UiKeepNet` 持有时不改 PA

Connecting / Listening **保网但不拉 PA**。

### 前台联网应用

WorkBuddy 与百问 AI 使用 `ForegroundNetwork` 声明页面及后台查询的软保网需求。引用存在时跳过空闲断网倒计时，保持网络可用；不改变 PA，不刷新用户活动时间，也不阻止无操作自动待机。进入待机释放页面引用，取消轮询及连接；用户退出待机且页面仍保留时重新申请并恢复联网。手动关闭 Wi-Fi 时不自动打开，待机内部的定时唤醒不恢复业务联网。

页面引用原位存放，使用现有 `PowerNeedHold` 管理。该需求的硬件刷新由现有网络工作队列执行，投递失败由策略节拍兜底。联网应用的音频占用在采音和编解码任务退出前保持可见，待机任务确认这些应用的网络引用、音频占用以及网络和共享电源的硬件操作全部收尾后，才进入切断共享外设电源的浅睡阶段；等待期间仍响应电源键退出待机。OTA、下载和实际录音继续使用原有活动需求。

百问 AI 使用会话代号取消离页或待机前的异步启动、录音及音频就绪回调，旧停止消息不会关闭恢复后的新会话。WorkBuddy 恢复授权时若旧任务仍占用并发名额，复用页面节拍延后重试，保留当前授权和换绑意图；服务返回的授权错误仍由用户选择重试。

### 禁待机

- `UsbVirtualDisk` / `StandbyInhibit`：不计无操作进待机（不硬占网）
- 硬占网（`LocalMic` / `OtaDownload` / Connecting 等）期间也不计无操作进待机

---

## 设置→功耗（NVS ns=`power`）

UI：`settings_power_tab`。改后异步写 NVS，重启可读。

| 项 | API | 可选 | 默认 | 作用 |
|----|-----|------|------|------|
| 空闲降频 | `Get/SetIdleCpuMhz` | 160 / 240 / 320 | 320 | 仅 **AppIdle** 定频（需 `CONFIG_PM_ENABLE`） |
| 保网时长 | `Get/SetNetGraceSec` | 0（永不）/ 30 / 60 / 120 | 30 | 无保网需求后仍保持 **NetActive**；选择“永不”时不因空闲断网，待机仍停网 |
| 浅睡待机 | `Get/SetUserIdleToStandbySec` | 180 / 600 / 1800 | 180 | 无操作 → StandbyUi |
| 自动关机 | `Get/SetStandbyToShutdownSec` | 0（永不）/ 180 / 600 / 1800 / 10800 / 21600 | 1800 | 浅睡累计 → 硬关机 |

允许 Wi-Fi 时，选择“永不”后不因空闲断网；自动待机和电源键待机仍停网，退出待机后按业务需求恢复联网。全局下滑面板的手动关闭优先于保网与唤醒恢复，使用约束见根 README 的快捷控制说明。开机联网等待上限为 30 秒，超时后继续后台连接。

---

## 按键

对齐 397：浅睡只靠电源键唤醒并退出待机。

| 场景 | POWER 短按 | POWER 长按（约 3s） | BOOT |
|------|------------|---------------------|------|
| 非待机 | 进 Standby Overlay → LP 会话 | `RequestPowerOff` → 关机图 → PWR_KEY | 业务 / 百问等（屏内 VkKey） |
| 待机内（浅睡醒） | EXT1(GPIO2/P17) → 一律按 POWER：短按 dismiss | 量满长按 → 硬关机 | — |
| 待机内（醒着轮询） | 同上 dismiss | 同上关机 | 短按继续睡；长按约 500ms → 百问 hold-through |

待机内手势由 `standby_lp` 量时长（浅睡期间正常按键任务不参与决策）。EXT1 醒后即使 P17 已松开，仍按「POWER 短按 dismiss」（对齐 397 latch）。

---

## S31 浅睡原子（`power_hw_light_sleep_once`）

对齐 **add76573 tip** 入睡序列；唤醒脚 P17→TCA INT→GPIO2：

```text
SD HoldPinsLow
→ TPS enter_sleep
→ 屏总线 bus_hold_low
→ Accel ReleaseIntLine
→ Keys MaskForSleep
→ 等 GPIO2 空闲高 → 仅 P17 输入唤醒
→ EXT1 GPIO2 ANY_LOW（+ 可选定时）
→ sleep_cut_rails → bus/SD latch
→ esp_light_sleep_start
→ 醒瞬间锁存 P17
→ Keys Resume → restore rails → **Sd_Init → ReviveUsj（软拔插）** → bus
```

**SD hold 与串口**：浅睡仍做 `kLsSd`（CNNT release + 脚拉低）。醒后先 remount（IDF 拉回 `ded_sel`），再 `ReviveUsjAfterSdRestore`——先前在 remount **之前**踢 USJ 无效。

进睡日志应见 `sd=1`；醒后应见 `usj revive after SD conn=1`。

- 单次切片上限：`kLightSleepSliceUs`（60s）
- 排查时可设 `kStandbyLpUseLightSleep = false`（改用 `vTaskDelay`，串口不断；此时 BOOT 长按仍可进百问）
- 硬关机：`power_hw_begin_power_off` → 关 PA → `ShowPoweredOffScreen` → 关前光 → PWR_KEY 脉冲

关机任务使用 PSRAM 栈执行同步 LVGL 绘制，壁纸解码由独立后台任务完成。加载提示及画面保留行为见[屏幕显示说明](../../../display/README.md)。

`MAIN_PWR`：S31 无独立轨脚，`power_hw_main_rail_set` 为策略态幂等 no-op。

触控芯片深睡：板级 `TouchEnterSleep` 暂未接，待机仍可靠 POWER/定时唤醒。

---

关闭虚拟 U 盘时，先删除 TinyUSB 的 MSC 存储挂载，再释放 raw card/SDMMC
控制器并由 `SdCardManager` 普通挂载；浅睡卸载因此始终对应 IDF 的挂载上下文。
TinyUSB 临时 APP 挂载不转移 raw card 所有权。

物理刷屏与日志存储操作互斥，避免 Flash 写入暂停缓存导致 LCD 行缓冲欠载。
进出待机和关机复用切页推白：准备完整画面后提交一次刷新，避免额外强制全刷；驱动异常恢复及周期清屏仍按原策略执行。
待机定格和关机在等待物理刷新前冻结后续 UI 提交；刷新失败时重试，成功后才进入浅睡或发送断电脉冲。退出待机在恢复界面落墨后恢复前光。

S31 连续浅睡复位定位在 SDK 唤醒时的 MPLL 校准等待。失败前 RTC 快照中
电源配置为 7/7，启动时写入的 12/12 已被覆盖。校准前每次恢复 12/12，
并补齐官方 MPLL 模拟初始化与 PSRAM/MPLL 时钟交接；全局 SDK 文件不修改。
参考官方提交 `9726078b55430ef8d2ce4a460ebf21d889bb5c12` 及当前 S31 PMU 实现。
项目内兼容补丁见 `s31_sleep/s31_sleep_sdk.cmake`，更换 SDK 时应复核这些兼容处理。

SDK 包装、启动配置与 RTC 阶段/寄存器诊断统一放在 `s31_sleep/s31_sleep.cc`。
`power_hw.cc` 仅通过 `s31_sleep.h` 的初始化、报告、具名阶段和浅睡接口调用，
保留板级电源流程。链接包装也由模块 CMake 统一接入；清理构建后自动重新生成补丁源码，
无需修改全局 IDF。

RTC 看门狗保留，CPU retention、PSRAM halfsleep 使用原配置。
RTC 阶段/寄存器快照在启动后延迟重放，避免 USB 重枚举期间丢失复位证据。

## 待机 / 关机壁纸

| 能力 | 状态 |
|------|------|
| 待机 Overlay：NVS 有待机壁纸 → `StandbyWallpaper`，否则经典页 | 已移植 |
| 壁纸库设为待机 / 关机（NVS 文件名） | 已移植 |
| 关机：`ShowPoweredOffScreen`（NVS → 内置 `bg_shutdown.a2i1` → 「已关机」）→ 冻结 flush → PWR_KEY | 已接 `power_hw` |
| 待机 Park 后冻结 flush | 已接（`ParkEpdForStandby`） |
| 待机暂停 touch_feed | 已接（对齐 397；落墨期间嵌套 pause） |

---

## 业务接入

```c
PowerPolicy::GetInstance().Acquire(PowerNeed::OtaDownload);
// … 占网工作 …
PowerPolicy::GetInstance().Release(PowerNeed::OtaDownload);

// 或 RAII
PowerNeedHold hold(PowerNeed::AudioSession);
```

| 钩子 | 时机 |
|------|------|
| `NotifyDeviceState` | `SetDeviceState` 内 |
| `NotifyUserActivity` | 触摸 / 侧键；触摸深睡后待机中不应点屏 dismiss（靠 POWER） |
| `RequestReevaluate` | Overlay Show 后等 |
| `OnStandbyOverlayDismissed` | `StandbyScreen::Dismiss` |
| `Init(board)` | 板构造末尾 |

已接线示例：百问 `AudioSession`、OTA/云下载 `OtaDownload`、蓝牙音频 `BtAudio`、虚拟 U 盘 `UsbVirtualDisk`。
