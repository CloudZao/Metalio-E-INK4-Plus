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

### 禁待机

- `UsbVirtualDisk` / `StandbyInhibit`：不计无操作进待机（不硬占网）
- 硬占网（`LocalMic` / `OtaDownload` / Connecting 等）期间也不计无操作进待机

---

## 设置→功耗（NVS ns=`power`）

UI：`settings_power_tab`。改后异步写 NVS，重启可读。

| 项 | API | 可选 | 默认 | 作用 |
|----|-----|------|------|------|
| 空闲降频 | `Get/SetIdleCpuMhz` | 160 / 240 / 320 | 320 | 仅 **AppIdle** 定频（需 `CONFIG_PM_ENABLE`） |
| 保网时长 | `Get/SetNetGraceSec` | 30 / 60 / 120 | 30 | 无保网需求后仍保持 **NetActive** |
| 浅睡待机 | `Get/SetUserIdleToStandbySec` | 180 / 600 / 1800 | 180 | 无操作 → StandbyUi |
| 自动关机 | `Get/SetStandbyToShutdownSec` | 0（永不）/ 180 / 600 / 1800 / 10800 / 21600 | 1800 | 浅睡累计 → 硬关机 |

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
- 硬关机：`power_hw_begin_power_off` → 关 PA/前光 → `ShowPoweredOffScreen` → PWR_KEY 脉冲

`MAIN_PWR`：S31 无独立轨脚，`power_hw_main_rail_set` 为策略态幂等 no-op。

触控芯片深睡：板级 `TouchEnterSleep` 暂未接，待机仍可靠 POWER/定时唤醒。

---

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
