# Metalio E-Ink4-Plus

<p align="center">
  <img src="images/product.png" alt="Metalio E-Ink4-Plus" width="720"/>
</p>

**中文** | [English](README.md)

**快速链接**

- ESP-IDF 官方文档：[ESP32-S31 快速入门](https://docs.espressif.com/projects/esp-idf/zh_CN/v6.1/esp32s31/get-started/index.html)
- 同系列产品：[Metalio E-Ink 4](https://github.com/CloudZao/Metalio-E-INK4)（3.97 寸 SPI）

---

## 目录

1. [产品概述](#1-产品概述)
2. [核心特性](#2-核心特性)
3. [应用场景](#3-应用场景)
4. [系统架构](#4-系统架构)
5. [硬件规格](#5-硬件规格)
6. [网络架构](#6-网络架构)
7. [外设与引脚](#7-外设与引脚)
8. [原理图与资料](#8-原理图与资料)
9. [软件架构](#9-软件架构)
10. [云端服务与 API](#10-云端服务与-api)
11. [内置应用](#11-内置应用)
12. [通信协议](#12-通信协议)
13. [开发环境](#13-开发环境)
14. [编译与烧录](#14-编译与烧录)
15. [调试与常见问题](#15-调试与常见问题)

---

## 1. 产品概述

**Metalio E-Ink4-Plus** 是一款面向阅读与 AI 助手场景的开源墨水屏实体设备。整机搭载 **4.7 寸并行** 电子纸（**ED047TC2**，原生 **1216×684** / UI 竖屏 **684×1216**），由 **EPDiy** + **TPS65185** 驱动；配备 **FT6336U** 电容触控、盖板虚拟键、冷暖双路前光、I2S 麦克风与扬声器（经外置蓝牙音频芯片）、microSD（**SDMMC 4-bit**）、片上 Wi‑Fi、SC7A20H 三轴加速度计、PCF8563 RTC、BQ27220 电量计、震动马达，以及 USB MSC 虚拟 U 盘。设备可通过语音与云端 AI 交互，并支持本地书库阅读、云端资源推送、待机壁纸、SD 存储与低功耗管理。

| 属性 | 说明 |
|:---|:---|
| **产品名称** | Metalio E-Ink4-Plus |
| **板级标识** | `metalio-e-ink4-plus`（`BOARD_NAME` / 配网 AP 前缀 `MetalioEInk4Plus`） |
| **主控芯片** | **ESP32-S31**（RISC-V；Flash **16 MB**，Octal PSRAM @ **200 MHz**） |
| **屏幕** | 4.7 寸并行墨水屏，ED047TC2_1216；FT6336U 触控；冷暖前光 |

产品数据落在 SD 的 `metalio/e-ink/`，云端接口走 **ink-screen** 推送与书库同步；墨水屏采用全刷 / 局刷，列表与阅读页禁止滚动。

> 相对 [Metalio E-Ink 4](https://github.com/CloudZao/Metalio-E-INK4)：Plus 换用 **ESP32-S31**、**更大尺寸并行屏** + 前光，以及不同的引脚 / USB 架构。**勿复用 397 SPI 引脚表。**

---

## 2. 核心特性

### 2.1 AI 语音交互

- **流式 ASR / LLM / TTS**：经 WebSocket 或 MQTT+UDP 上云
- **百问 AI**：Push-to-Talk + A2UI 组件分页展示；可语音创建每日清单（见 [§11.2](#112-百问-aiassistant)）

### 2.2 墨水屏阅读体验

- **电子纸专用 UI**：LVGL I1 / A2I1（经 EPDiy）；全刷与局刷策略；列表与阅读页 **禁止滚动**，用盖板虚拟键或音量键（TCA9555 **P1.0**+ / **P1.1**−）翻页
- **逻辑 UI 尺寸**：普通页面四边内缩 2px，可绘区 680×1212；待机与关机页使用完整 684×1216，配置见 `config.h` 中 `DISPLAY_CONTENT_*`
- **书库**：九宫格封面 + 书名 → 详情大图 → 正文；支持 `.ebook` / `.epub` / `.txt`
- **字体**：Flash `font_data` 分区挂载 UI fontpack；阅读字体 `.ef` 可放 SD 或经「传输」下载
- **正文排版**：段落保留首行缩进，不增加额外段距。字体、行距、页边距、阅读方向和底栏显示状态相同时，排满的普通正文页行数固定；章名、插图和章节末页可能减少行数。
- **工具链**：`tools/ebook/` 可将 PDF/EPUB/MOBI/TXT 等转为设备可读 `.ebook`（含 Web 预览）

### 2.3 多模态与本地能力

- **每日清单**：云端 checklist 同步，本地缓存只读展示（待机页复用）
- **壁纸**：SD 壁纸库；可设为待机全屏图 / 关机图
- **传输**：云端推送、AP 热点、同一 Wi-Fi 下的网页文件管理与 USB 传输
- **快捷控制**：在任意页面的非上下滑动区域下滑唤出，左侧切换 Wi-Fi，右侧竖向调节前光亮度与冷暖比例；上滑或点击面板外关闭，OTA 升级、待机期间禁用（§11.6）。
- **传感器**：SC7A20H 三轴加速度计；PCF8563 RTC；BQ27220 电量计

### 2.4 连接能力

- **Wi-Fi**：ESP32-S31 片上 2.4 GHz；配网 AP 名形如 `MetalioEInk4Plus-xxxx`
- **蓝牙音频**：外置蓝牙音频芯片（UART AT），三种工作模式（见 [§12.1](#121-蓝牙音频与三种模式)）
- **虚拟 U 盘**：设置「存储」可将 microSD 以 USB MSC 暴露给电脑（S31 **HS UTMI** TinyUSB；通路由 **ANA_SW** GPIO36 切换）

### 2.5 电源与续航

- **单节锂电** + TI **BQ27220** 电量计（I2C 0x55）；SOC 由电压线性估算（详见 `docs/battery-soc-and-charge.md`）
- **充电**：**USB → CX25601N**（I2C 0x6B）；充电电流可选 **0.5A / 1A / 1.5A**，默认 **1A**
- **开关机**：侧面电源键（TCA9555 **P1.7**）；固件经 **P1.3**（`PWR_KEY_PULSE`）输出脉冲做软件关机
- **空闲策略**（「设置 → 功耗」，`PowerPolicy`）：
  - **无操作进浅睡待机**：默认 **3 分钟**（可选 10 / 30 分钟）→ 待机 Overlay（经典时钟 / 壁纸）
  - **浅睡累计关机**：默认 **3 分钟**（可选 10 / 30 分钟）
  - **AppIdle CPU**：80 / 160 / **240 MHz**（默认 240）；可关 Wi-Fi 降功耗
  - **断网暂留**：无保网需求后仍保网 **30 / 60 / 120 秒**，或选择**永不**因空闲断网；待机仍停网
- **按键**：
  - **POWER 短按**：进入 / 退出待机 Overlay
  - **POWER 长按 ~3 s**：硬关机
  - **BOOT 长按 ~500 ms**：百问 PTT / 待机穿透等（`power_policy.h`）

功放使能为 TCA9555 **P0.4**（`PA_EN`，NS4150 CTRL），由功耗策略按通话 / Speaking / 音频测试等场景开关。

---

## 3. 应用场景

首页第一页提供以下六个应用（`home_screen.cc` → `kApps[]`）：

| 应用 | 典型用途 |
|:---|:---|
| **每日清单** | 云端待办：同步、完成、删除 |
| **百问 AI** | 按住说话问答 + A2UI；可语音创建每日清单 |
| **阅读** | 本地书库浏览与分页阅读（音量键 P1.0 / P1.1） |
| **壁纸** | 壁纸库；待机 / 关机图 |
| **传输** | 云端推送书籍、壁纸、字体 |
| **设置** | 网络、主题、语言、震动、功耗、充电、屏幕、对话、蓝牙、测试、关于 |

---

## 4. 系统架构

```mermaid
flowchart TB
    ui["UI<br/>684×1216 LVGL 墨水屏 · BOOT/POWER · 盖板键 · 前光"]
    apps["应用<br/>清单 · 百问 · 阅读 · 壁纸 · 传输 · 设置 · …"]
    svc["服务<br/>AudioService · SdCardManager · PowerPolicy · BookSession · MCP"]
    proto["协议<br/>WebSocket · MQTT+UDP · HTTP API · BT AT"]
    board["板级<br/>WifiBoard · EPDiy Display · BTAudioCodec · Gauge"]
    drv["驱动<br/>EPDiy/TPS65185 · FT6336U · I2S · SDMMC · I2C · UART · TCA9555"]

    subgraph hw["硬件"]
        direction LR
        s31["ESP32-S31<br/>UI · Wi-Fi · 音频编排"]
        bt["蓝牙音频"]
        epd["ED047TC2 并行"]
    end

    cloud["云端<br/>ASR · LLM · TTS · 清单 / 推送 / 书库"]

    ui --> apps --> svc --> proto --> board --> drv --> hw
    proto <--> cloud
```

### 4.1 语音数据通路（百问）

```mermaid
flowchart LR
    hold["PTT 按住<br/>BOOT / 盖板 HOME / 屏上"] --> mic["I2S 麦"]
    mic --> enc["AudioService 编码"]
    enc --> link["WebSocket / MQTT"]
    link --> asr["云端 ASR"]
    asr --> llm["LLM"]
    llm --> tts["TTS（服务端可关）"]
    llm --> a2ui["A2UI 流"]
    tts --> spk["I2S 喇叭"]
    a2ui --> page["墨水屏分页渲染"]
```

---

## 5. 硬件规格

| 类别 | 规格 |
|:---|:---|
| **主控** | ESP32-S31；Flash 16 MB；Octal PSRAM @ 200 MHz |
| **存储** | 板载 Flash（`partitions/v1/16m.csv`）+ microSD（SDMMC **4-bit**） |
| **显示** | ED047TC2_1216，约 4.7 寸，原生 1216×684 / UI 684×1216；EPDiy 并行 8-bit；TPS65185 PMIC |
| **触控** | FT6336U（I2C 0x38）；INT=GPIO5；RST=TCA9555 **P1.2**（高=上电） |
| **前光** | 冷 GPIO6 / 暖 GPIO7（YX6016N EN/PWM） |
| **盖板键** | HOME / PREV / NEXT（FT 坐标；见 [§7.1](#71-esp32-s31-gpio)） |
| **音频** | 外置蓝牙音频芯片 + I2S 全双工 @ 16 kHz；编解码 / 喇叭 / 耳机 |
| **网络** | 片上 Wi-Fi |
| **蓝牙** | 外置蓝牙音频芯片 UART（GPIO 39/38，115200）；非 ESP 经典蓝牙协议栈 |
| **传感器** | SC7A20H 加速度计（INT 与 TCA INT 同进 GPIO2）；PCF8563 RTC（INT = TCA **P1.6**） |
| **电源** | 单节 + BQ27220；CX25601N 充电；面板 TPS65185；轨控经 TCA **P0.5** / **P1.4** |
| **震动** | TCA9555 **P0.3** 马达（高有效；默认约 50 ms 脉宽） |
| **功放** | TCA9555 **P0.4**（`PA_EN`） |
| **按键** | BOOT（GPIO61）；电源 = TCA **P1.7**；音量+ = **P1.0**，音量− = **P1.1** |
| **USB** | S31 HS UTMI MSC 虚拟 U 盘；**ANA_SW**（GPIO36）选路 |

---

## 6. 网络架构

Metalio E-Ink4-Plus 以 **ESP32-S31** 片上 Wi-Fi 为中心：

```mermaid
flowchart TB
    s31["ESP32-S31<br/>LVGL · 音频 · Wi-Fi · SD · 协议"]
    bt["蓝牙音频"]
    epd["ED047TC2 + TPS65185"]

    s31 <-->|"UART1 115200<br/>TX=39 RX=38"| bt
    s31 -->|"并行 8-bit + I2C TPS"| epd
```

| 单元 | 角色 | 接口 | 职责 |
|:---|:---|:---|:---|
| **ESP32-S31** | 主机 | — | UI、音频编排、Wi-Fi、SD、协议、MCP、阅读 |
| **蓝牙音频** | 编解码 / 喇叭 / 耳机 | UART AT + I2S | 三种模式（§12.1） |
| **TPS65185** | 面板 PMIC | I2C（经 TCA 控制脚） | VCOM / 上电 / PWR_GOOD |

> 在 **设置 → 网络** 中选择 Wi-Fi 或 4G。4G 模式支持内置卡和外置卡切换，切换并恢复模组通信后重启生效。

4G 模组在软件重启前关闭握手、DMA 和网络任务；启动时清除跨 CPU 复位保留的 GPIO 中断与 UHCI 状态，再初始化各外设。切卡失败不会标记为成功或触发成功重启倒计时。

Wi-Fi 配置同步保存在 NVS 和 SD 卡的 `metalio/e-ink/settings/wificonfig.json`；开机导入卡内配置。文件格式、失败恢复和密码保护见 [Wi-Fi 配置与 SD 同步](docs/wifi-config.md)。

设备传输页支持通过 AP 热点或连接同一 Wi-Fi 的局域网上传、下载和管理文件，包括同名文件处理、取消上传、重命名、移动和删除。上传期间可浏览目录、追加任务并操作其他文件，下载暂不可用；接口、文件限制及 SDMMC 补丁说明见 [AP 文件管理](docs/ap-file-transfer.md)。

---

## 7. 外设与引脚

引脚：`main/boards/metalio-e-ink4-plus/config.h`  
板级胶合：`main/boards/metalio-e-ink4-plus/display/epd_board_metalio_eink4_plus.c`  
（硬件版本差异见 `config.h` 注释，如 V1.3–V1.4 的 XSTL / MODE。）

### 7.1 ESP32-S31 GPIO

| 功能 | GPIO | 说明 |
|:---|:---:|:---|
| I2C SDA | 0 | 共用：TCA9555、FT6336U、BQ27220、RTC、充电、加速度计、TPS65185 |
| I2C SCL | 1 | |
| IO 扩展 INT | 2 | TCA（+ 加速度计 INT 线与） |
| EPD XSTL | 4 | V1.3/V1.4 |
| 触控 INT | 5 | FT6336U |
| 前光冷 / 暖 | 6 / 7 | |
| EPD D0–D7 | 8–15 | 并行数据 |
| EPD XCL / XLE / XOE | 16 / 17 / 18 | |
| EPD MODE | 19 | V1.4 |
| SDMMC D0–D3 | 20–23 | 4-bit |
| SDMMC CLK / CMD | 24 / 25 | |
| EPD SPV / BORDER / CKV | 35 / 37 / 40 | |
| ANA_SW | 36 | MSC 选路（低 = 虚拟 U 盘） |
| 蓝牙音频 TX / RX | 39 / 38 | UART1，115200 |
| I2S BCLK / WS / DOUT / DIN | 42 / 43 / 44 / 45 | 全双工 |
| BOOT | 61 | |

**盖板虚拟键坐标**（FT 原生；Y≈1600 在屏外）：

| 键 | X | Y | 典型用途 |
|:---|:---:|:---:|:---|
| HOME | 100 | 1600 | 返回 / 百问 PTT 长按 |
| PREV | 200 | 1600 | 返回 / 上页 |
| NEXT | 300 | 1600 | 下页 |

### 7.2 TCA9555（I2C 16-bit，地址 0x20）

| 逻辑脚 | 硬件 | 方向 | 功能 |
|:---|:---|:---:|:---|
| VCOM_CTRL | P0.0 | OUT | TPS65185 VCOM |
| PWRUP | P0.1 | OUT | TPS65185 上电 |
| WAKEUP | P0.2 | OUT | TPS65185 唤醒 |
| MOTOR | P0.3 | OUT | 震动（高有效） |
| PA_EN | P0.4 | OUT | NS4150 功放使能 |
| CAM_SCR_EN | P0.5 | OUT | 摄像/屏幕电源轨 |
| PWR_GOOD | P0.7 | IN | TPS65185 PG |
| VOLUME_UP | P1.0 | IN | 音量 + |
| VOLUME_DOWN | P1.1 | IN | 音量 − |
| TP_RST | P1.2 | OUT | FT6336 电源/RST（高=开） |
| PWR_KEY_PULSE | P1.3 | OUT | 关机脉冲到开关机芯片 |
| BT_PA_PWR | P1.4 | OUT | 蓝牙 + 功放电源 |
| RTC_INT | P1.6 | IN | PCF8563 INT |
| POWER | P1.7 | IN | 电源键 |

> 原理图「P1x」指 Port1 bit x，勿与十进制脚号混淆。

### 7.3 I2C 地址

| 器件 | 7-bit 地址 | 说明 |
|:---|:---:|:---|
| TCA9555 | 0x20 | IO 扩展 |
| FT6336U | 0x38 | 触控 |
| PCF8563 | 0x51 | RTC |
| BQ27220 | 0x55 | 电量计 |
| CX25601N | 0x6B | 充电 |
| SC7A20H | 0x19（备选 0x18） | 加速度计 |
| TPS65185 | （PMIC） | 面板升压 / VCOM |

### 7.4 外设关系图

```mermaid
flowchart LR
    subgraph s31["ESP32-S31"]
        i2c["I2C 0/1"]
        i2s["I2S"]
        par["并行 EPD"]
        sdmmc["SDMMC 4-bit"]
        uartbt["UART1 BT"]
        usb["USB HS UTMI"]
        fl["GPIO6/7 前光"]
    end

    i2c --> tca["TCA9555"]
    i2c --> ft["FT6336U"]
    i2c --> bq["BQ27220"]
    i2c --> rtc["PCF8563"]
    i2c --> cx["CX25601N"]
    i2c --> acc["SC7A20H"]
    i2c --> tps["TPS65185"]
    par --> epd["1216×684 EPD"]
    i2s --> audio["麦/喇叭经蓝牙音频芯片"]
    sdmmc <--> sd["microSD"]
    uartbt <--> bt["蓝牙音频"]
    usb --> msc["MSC 虚拟 U 盘"]
    fl --> lamp["冷 / 暖前光"]
    tca --> tps
```

---

## 8. 原理图与资料

| 文档 | 路径 |
|:---|:---|
| 电池 / 充电策略 | [docs/battery-soc-and-charge.md](docs/battery-soc-and-charge.md) |
| 墨水屏 Bayer 抖动 | [docs/epd-bayer-dither-gray.md](docs/epd-bayer-dither-gray.md) |
| SD 字体 / 资源 | [docs/sd-font-resources-upgrade.md](docs/sd-font-resources-upgrade.md) |

固件引脚以 `config.h` / `epd_board_metalio_eink4_plus.*` 为准。若原理图另册发布，以硬件团队文档为准。

---

## 9. 软件架构

固件基于 xiaozhi-esp32，面向 `metalio-e-ink4-plus` 定制。CMake 工程名 `Metalio-E-Ink4-Plus`。

### 9.1 分层

| 层 | 路径 / 模块 | 职责 |
|:---|:---|:---|
| **入口** | `main.cc` → `Application` | 启动、事件循环、状态机 |
| **板级** | `boards/metalio-e-ink4-plus/` | 硬件初始化、EPDiy、触控、按键、前光 |
| **显示** | `display/screen/*`、`lv_adapter_*` | 应用、墨水刷新、状态栏 |
| **阅读** | `reader/`、`tools/ebook/` | 书籍会话、`.ebook` 工具链 |
| **音频** | `audio/` + 外置蓝牙 UART | 编解码、录放 |
| **协议** | `protocols/` | WebSocket、MQTT+UDP |
| **MCP** | `mcp_server.cc` | 端侧 Model Context Protocol |
| **功耗** | `boards/common/power_policy/` | 功耗档、待机、关机 |
| **公共** | `boards/common/` | Wi-Fi、SD、电量计、USB MSC、蓝牙编解码 |

显示采用 30 相图片表，保留 S31 / E0470A14-AF 的扫描时序与 TPS65185 电源驱动。显示效果须在本机验收。

### 9.2 状态机

百问 AI 在前台保持网络可用，不使用空闲断网倒计时；无操作自动待机仍遵循系统设置。进入待机停止语音并释放页面保网需求，用户唤醒且页面仍保留时恢复连接，不自动录音。后台等待不刷新无操作计时，手动关闭 Wi-Fi 的设置优先。

启动流程结束后，`app_main` 返回，由 SDK 回收启动任务的栈。运行期 `MainEventLoop` 只接收和分发事件。`Application::Schedule` 按语音会话、联网、存储、本地音频、取消清理五类工作队列执行回调；同类工作串行，各类任务独立。队列有容量上限，投递仅同步短暂队列锁，不等待业务执行或队列空位；失败返回 `false`，调用方需要恢复状态或保留上下文重试。工作任务使用内部 RAM 栈，空闲两秒后退出；语音会话或音频服务仍在占用硬件时保留相应控制任务。语音任务不重入消费控制消息，每次发送最多八个音频包后交还队列；停止意图和异步音频释放在队列暂满时保留重试。录音测试和老化测试的音频初始化复用音频工作队列，退出或会话失效后取消等待；同一调用方重新申请时撤销尚未执行的旧释放请求。云信文本和句尾状态变更按接收顺序重试，最多缓存 32 个未确认句尾，持续拥塞时保留较新的句尾。

`ScreenLvAsync` 和 `ScreenLvAsyncUrgent` 分别投递到独立的普通、优先 UI 队列，由现有 LVGL Tick 持锁消费，不经过应用主循环。界面消息入队成功后由回调接管参数；失败时由发送方回收。离页、取消和待机继续使用现有页面代数与取消标记，后台结果不能访问已经释放的控件。网络等待、HTTP 关闭和硬件收尾不得放入 UI 回调；NVS 操作必须使用内部 RAM 栈。

`Application` 状态包括 starting → configuring → idle → connecting → listening ↔ speaking，以及 upgrading / activating / fatal_error 等。

- **idle**：百问等页面经 `PowerNeed` / 音频会话声明保网需求
- **listening / speaking**：流式 ASR / TTS
- **OTA**：双槽 `ota_0` / `ota_1`；全屏 `OtaUpgradeScreen`

SD 卡升级：将适用于本机的应用固件 `Metalio-E-Ink4-Plus.bin` 复制到 SD 卡根目录（不要使用整包 `merged-binary.bin`），插卡后重启设备，在提示框中选择立即升级，等待完成并自动重启。升级期间请勿断电或拔卡；无需连接 Wi-Fi。

### 9.3 板级初始化顺序

约 `MetalioEInk4PlusBoard` 构造序列：

```mermaid
flowchart TD
    s1["1. MetalioEpd_Init<br/>I2C + TCA + TPS65185 + EPDiy"] --> s2["2. MetalioPower_Init<br/>BQ27220 + CX25601N"]
    s2 --> s3["3. 前光 Init + NVS 恢复"]
    s3 --> s4["4. RTC → Accel → FT6336U 触控"]
    s4 --> s5["5. 创建 Display"]
    s5 --> s6["6. 震动 → SD → 按键 → 模式1"]
    s6 --> s7["7. 系统监控 + PowerPolicy::Init"]
```

### 9.4 目录树（摘录）

```
main/
├── application.cc                 # 启动、状态机、协议分发
├── cloudzao_endpoints.c           # 云服务主机、路径及连接参数
├── api_endpoints.h                # URL 拼装（不含主机字面量）
├── boards/metalio-e-ink4-plus/    # 板级（config.h / EPDiy / FT6336 / 前光）
├── boards/common/                 # Wi-Fi、SD、电量计、功耗、USB MSC
├── display/screen/                # 应用、待机、OTA、设置
├── display/a2ui/                  # A2UI 渲染
├── reader/                        # 电子书会话
├── audio/                         # 录放
└── protocols/                     # WebSocket / MQTT

tools/ebook/                       # .ebook 转换、Web、模拟器
tools/fontpack/                    # UI 字体打包
partitions/v1/16m.csv              # 当前 16MB 分区表
use_font/font.fontpack             # 合并进 font_data
merge_firmware.sh                  # 编译并合并完整 bin
```

### 9.5 Flash 分区（`sdkconfig` → `partitions/v1/16m.csv`）

| 分区 | 约大小 | 用途 |
|:---|:---|:---|
| nvs / otadata / phy_init | 16K / 8K / 4K | NVS、OTA 元数据、PHY |
| model | 400K | 模型分区（SPIFFS） |
| ota_0 / ota_1 | 各 5MB | 双 OTA 应用 |
| resources | 400K | 资源 SPIFFS |
| font_data | 5MB | UI fontpack（mmap） |
| coredump | 64K | 崩溃转储 |

> 分区表偏移：`CONFIG_PARTITION_TABLE_OFFSET=0x8000`。

---

## 10. 云端服务与 API

云端主机与 HTTP 路径在 `main/cloudzao_endpoints.c` / `main/cloudzao_endpoints.h`。业务侧只通过 `main/api_endpoints.h` 拼 URL。依赖后端的语音服务、APP 推送、云端清单等功能需要在端点文件及设备配置中提供兼容的服务地址和凭据；配置后端时保持符号名不变。本地阅读、壁纸、AP 传输、WIFI传输和 U 盘传输不依赖这些配置。

主语音通路仍为 **WebSocket / MQTT+UDP**。设备工具可通过 **MCP** 暴露给大模型。

---

## 11. 内置应用

首页列表：`home_screen.cc` → `kApps[]`（每页最多 6 个）。

### 11.0 首页应用

首页每页显示六个应用，底部圆点仅指示页码，不响应点击，当前页圆点放大；实体翻页键支持首尾循环，长按逐页循环。切换应用页时先推白再显示目标页。第二页仅显示 WorkBuddy，状态栏及主题上方的信息区保持显示，普通返回保留首页页码；首页第二页按 Home 键返回第一页，WorkBuddy 内退出返回进入前的应用页。像素视界下名称显示为 buddy，图标与其他应用采用相同显示尺寸；其他主题下名称显示为 workbuddy。

WorkBuddy 支持与电脑端本地助理扫码绑定、重新绑定、长按语音交互、自动读取回复和同步消息。用户与助理分别显示纯文本气泡，支持屏幕分页和实体键长按十页连翻。历史缓存按 PSRAM 实际分配量裁剪旧消息；进入待机停止录音和查询，唤醒后恢复连接，不重复发送已受理指令。

服务端需启用 WorkBuddy 本地助理模式、百度实时 ASR 和 WebSocket 转发，电脑端需保持对应账号的本地助理在线。独立服务地址在 `main/cloudzao_endpoints.c` 配置。使用方法、接口、历史范围和内存预算见 [WorkBuddy 模块说明](main/workbuddy/README.md)。

| 应用 | 说明 |
|:---|:---|
| **每日清单** | 云端清单 + 本地缓存；完成 / 删除 / 多选；虚拟键翻页 |
| **百问 AI** | 语音助手 + A2UI 分页；可语音创建每日清单（§11.2） |
| **阅读** | 书库九宫格 → 详情 → 正文；音量键（P1.0 / P1.1）翻页（§11.3） |
| **壁纸** | SD 图库；关机 / 待机（§11.4） |
| **传输** | 书籍 / 壁纸 / 字体统一推送列表（§11.5） |
| **设置** | 网络 / 主题 / 语言 / 震动 / 功耗 / 充电 / 屏幕 / 对话 / 蓝牙 / 测试 / 关于（§11.6） |

系统页（无首页图标）：**待机**、**OTA 升级**、设置内嵌 **蓝牙**。

---

### 11.1 每日清单（task）

- 读 `checklist_cache`；可从 API 刷新
- 完成 / 删除 / 多选；不滚动 — 用虚拟键
- 经典待机只读展示待办摘要

### 11.2 百问 AI（assistant）

见 `main/display/screen/assistant_screen/README.md`。

- 进入页启动语音会话；离开停止
- **多源 PTT**：BOOT / 盖板 HOME / 屏上；按住约 **500 ms** 开始听，松手结束
- 服务端下发 **A2UI**；设备按字形度量分页；`vk_prev` / `vk_next`
- **语音创建每日清单**：对话可在云端建待办并刷新本地 `checklist_cache`
- 会话可落盘 `/sdcard/metalio/e-ink/chat_log/`（开机清空；无 SD 时仅 RAM）
- 图片缓存：`.../a2ui_cache/`（开机清空）
- 设置 → 对话 跟随服务端 TTS 偏好

### 11.3 阅读（book）

- 路径：`/sdcard/metalio/e-ink/books`（界面展示 `metalio/e-ink/books`）
- 格式：`.ebook` / `.epub` / `.txt`；`.txt.idx` 索引；侧车封面 `.a2i1`
- 字体：`.../fonts/*.ef`
- 排版偏好在 NVS；列表/正文 **禁止滚动**；虚拟键 / 音量键翻页
- 阅读设置 → 阅读方向：竖屏、左横、右横、自动；选项自动保存。自动模式在正文、阅读设置、字体、书签和目录中均可生效，根据 SC7A20H 重力数据判断方向，方向连续稳定 0.8 秒后旋转；数据不明确时保持当前方向，不自动切换至倒置竖屏。旋转后正文排版尚未就绪时，仅在阅读内容区显示加载提示，不遮挡菜单或阻止退出、待机；新方向的当前页准备好后替换提示。离开阅读页后恢复竖屏。
- 竖排正文采用大陆标点位置：逗号、顿号、句号居字格右上并占半字格，问号、叹号、冒号、分号居右侧；引号使用竖排直角形式，括号、书名号、省略号和破折号沿竖排方向显示。TXT、EPUB 和 `.ebook` 共用列首、列尾禁则，避免句末标点及闭合标号出现在列首、起始标号出现在列尾，并保持成对省略号与破折号不拆列。极窄列宽或不可分的连续标点超过整列容量时，按容量换列。横排规则保持不变。
- 阅读设置 → 翻页方式：左右滑动、推白翻页；选项自动保存。推白翻页在下一页时从右向左、上一页时从左向右，适配横竖屏；必要清屏时使用全刷。
- 开机可经 `library/sync` 同步进度
- PC 转换：`tools/ebook/README.md`

### 11.4 壁纸与待机

PNG 逐行解压，支持非交错 8/16 位灰度和 RGB/RGBA（含灰度透明度），以及 8 位索引色。16 位先按原位深还原滤波，再降为 8 位灰度；解压工作区固定，两个像素行按实际宽度分配到 PSRAM，不再限定原图宽度为 2048。文件直接流式读取，不缓存完整 RGBA 或整份 PNG；解码串行，退出／取消释放临时空间。隔行、低位深索引色、不受支持的格式及内存不足返回失败。

壁纸设置预览先按原图尺寸解码，再缩小到预览框；保存旋转／镜像结果不再压缩到屏幕分辨率。待机和关机图片也按原图尺寸解码，在完整 684×1216 面板内等比缩小、居中绘制，不应用普通 UI 的 2px 内缩，不放大小图。原尺寸入口使用 `max_w=max_h=0`，不受 PNG 缩放入口的 1 MiB 输出上限或通用文件入口的 4 MiB 压缩文件上限约束；仍受图片尺寸表示范围及实际可用内存约束。缩略图保持按格子大小解码。

LCD 帧尾等待末批 VSYNC_END 后结束扫描；DMA EOF 仅续填数据，尾部补无驱动数据，避免内部缓冲未输出完就停机。退出待机时恢复普通 UI 分辨率并清除原全屏边缘像素；屏幕效果仍需实机验收。

壁纸和电子书共用图片解码入口，支持常见 8 位渐进式 JPEG；普通 JPEG 保持原路径。渐进式解码仅使用 JPEG 功能，临时 PSRAM 预算 8 MiB，保留 2 MiB 空闲，超出预算返回失败；返回、Home、待机作废在途壁纸解码，唤醒后恢复当前页加载。尺寸限制及第三方授权见 [渐进式 JPEG 解码](main/reader/jpeg/README.md)。

- 图库：`/sdcard/metalio/e-ink/wallpaper`，九宫格分页浏览 / 启用；列表和批量删除不设固定数量上限，实际容量受 SD 卡和可用内存约束。缩略图仅缓存当前页及相邻页。
- 文件名：阅读、壁纸、阅读字体与传输统一支持最长 246 字节的 UTF-8 名称（含扩展名，中文通常占 3 字节）；目录扫描和文件操作使用同一上限。壁纸另存为会为时间戳及扩展名预留空间，并保留完整 UTF-8 字符。
- **待机路由**（`standby_screen`）：
  - 已启用待机壁纸 → 原图解码后全屏等比显示
  - 否则 → 经典（日期 / 农历 / 天气 / 待办）
- 关机图：优先 NVS 壁纸，否则内置 `bg_shutdown.a2i1`

**像素视界主题**：可在「设置 → 主题」选择，返回首页生效并保存选择。未保存主题选择时默认使用黑色描边，已有选择继续保留。首页采用大号衬线时间、单行日期、带硬阴影及黑底标题签的最近阅读／待办卡片，下方为阅读、壁纸、百问 AI、便签、传输、设置六个像素图标。最近阅读展示内存缓存中的书名和进度；今日统计仅计入计划日期等于当天的任务，未设日期的任务不计入，无缓存时显示 `--`。阅读卡片直接打开最近阅读的书籍，无有效记录时进入阅读首页，待办卡片进入清单。日期行「诸事皆宜」为主题固定文案。待机和离页暂停刷新，页面销毁时清理定时器。

六张主题 PNG 位于 `main/assets/embedded/pixel/ic_px_*.png`，构建时由首页图标生成器转为保留原始透明度的 A8 数据，随应用固件更新，不依赖 `resources` 分区；不展开重复 RGB 数据。标题签与卡片为页面同级节点，跨边框的文字和背景完整显示，并保留点击入口。整体参考图不嵌入固件。数字子集由 `tools/generate_pixel_theme_font.py` 从仓库已有 Latin Modern Math 生成，许可证见 `tools/ttf-fonts/LatinModernMath/GUST-FONT-LICENSE.txt`。

### 11.5 传输

- 传输方式：APP 推送 / AP 传输 / WIFI传输 / U 盘传输；APP 推送内按全部 / 壁纸 / 书籍 / 字体筛选，**刷新**获取待接收文件列表。
- APP 推送使用当前网络，可通过 Wi-Fi 或 4G 同步；AP 和 WIFI传输需先切换到 Wi-Fi 模式，4G 模式下页面显示切换提示。
- 保存目录：BOOK→`books`，BADGE→`wallpaper`，FONT→`fonts`（TTF/OTF→`fonts_ttf`）。
- APP 推送保存前按目标路径检查同名项目，使用带背景遮罩的弹窗选择**覆盖原文件 / 跳过此文件 / 保留两份**；保留两份在扩展名前追加 `-1`、`-2` 等编号。批量保存逐项询问，跳过不删除云端任务，只有保存成功才发送回执。
- 弹窗返回、取消或点击背景结束本次保存；Home、离页、进入待机会清除弹窗及待保存队列，唤醒后不会自动恢复选择。已保存的文件保留。
- 文件先下载到临时路径，覆盖时备份原文件，提交失败尝试恢复；遗留 `.cloud-backup` 不会被覆盖，目标缺失时下一次提交先恢复备份。存储故障导致恢复失败时保留备份供恢复。
- 重名判断不计算 MD5；服务端提供 SHA-256 时保留流式完整性校验，无校验值时不计算哈希。下载仍检查非空、文件长度和大小上限。
- 状态栏保持时钟。

### 11.6 设置

| 页签 | 内容 |
|:---|:---|
| **网络** | Wi-Fi 配网与连接；统一底部黑白错列键盘，支持大小写、两页数字/符号、Space 与图标确认；输入末尾显示静态 I 光标 |
| **主题** | 白底 / 网点灰底 / 黑色描边 / 斜切棋盘 / 像素视界 |
| **语言** | UI 语言 |
| **震动** | 按键震动开 / 关 |
| **充电** | 0.5A / 1A / 1.5A，默认 1A；选择后自动保存并在重启后恢复 |
| **功耗** | AppIdle CPU、待机延时、断网暂留、累计关机 |
| **屏幕** | 刷新模式及黑色深度；前光通过全局下滑面板调节 |
| **对话** | 百问 TTS 服务端偏好 |
| **蓝牙** | 模式 1/2/3、扫描/配对（§12.1） |
| **测试** | 工厂 / 老化入口 |
| **关于** | 型号、芯片、固件版本、MAC、Flash、PSRAM |

非上下滑动区域下滑可唤出快捷面板，列表滚动区域优先保留原有手势。面板同时显示顶部状态栏；从滑条外上滑、点击面板外或按返回、首页键可收起，并保留当前页面。OTA 升级及待机时自动关闭并禁止唤出。面板打开与关闭使用 GC16 全刷，会产生可见闪屏。

左侧为带猫爪图标的 Wi-Fi 开关和前光开关，右侧为亮度、冷暖比例滑条，均向上增大。图标保持原尺寸，在白底区域显示黑色、黑色填充区域显示白色；不显示滑条标题和数值百分比，面板不含蓝牙控制。亮度 0 关闭输出；冷暖比例 0 为纯冷光、50 为冷暖同开、100 为纯暖光。PWM 支持连续混光，GPIO 模式仅支持通道开关。关闭前光保留亮度和冷暖比例，调节任一滑条会自动开启前光；调节冷暖比例或重新开启时，若亮度为 0 则使用默认亮度 40。前光开关、亮度和冷暖比例均会保存；开机输出等待蓝牙共享电源复位完成。

Wi-Fi 模式下，手动关闭 Wi-Fi 会停止扫描、连接及无线文件传输，阻止业务自动联网和待机恢复；再次点击开关可开启。开关状态会保存，重启后保持上次状态，不删除已保存网络。4G 模式下，面板中的 Wi-Fi 显示已关闭且不可开启；切回 Wi-Fi 模式后使用已保存的开关状态。4G 联网业务不受已保存的 Wi-Fi 开关状态影响，天气缓存缺失时也可通过当前 4G 网络更新。Wi-Fi 扫描、连接和本地传输会说明当前处于 4G 模式或 Wi-Fi 已关闭；手动关闭时同时显示打开 Wi-Fi 后重试的顶部弹窗，只有驱动初始化失败才提示初始化失败。

### 11.7 SD 目录布局

产品根：`/sdcard/metalio/e-ink/`（`sd_paths.h`）

| 路径 | 用途 |
|:---|:---|
| `.../books` | 电子书、封面、索引 |
| `.../fonts` | 阅读 `.ef` 字体 |
| `.../wallpaper` | 壁纸 / 关机图 |
| `.../chat_log` | 百问会话 JSON（开机清空） |
| `.../a2ui_cache` | A2UI 图片缓存（开机清空） |
| `.../recordings` | Opus 录音（若启用录音应用） |

---

## 12. 通信协议

| 协议 | 用途 |
|:---|:---|
| **WebSocket** | 流式语音（ASR/LLM/TTS） |
| **MQTT + UDP** | 备选上行 |
| **MCP** | 向 LLM 暴露设备工具 |
| **HTTP** | 清单、天气、推送、书库、TTS 偏好、ASR |
| **BT AT** | 外置蓝牙音频模块 |

### 12.1 蓝牙音频与三种模式

ESP32-S31 经 **UART1**（115200，GPIO 39/38）发 AT。设置 → 蓝牙嵌入 `BluetoothScreen`。**非** ESP 经典蓝牙协议栈。

蓝牙设置分为版本号、蓝牙复位和模式切换三部分。进入页面时发送 `AT+GVER=1`，从 `AT+GVER=V1.03` 格式的应答读取固件版本；未收到版本时显示“未知”。点击“检测版本”可手动刷新，查询期间按钮显示“检测中”并禁用，收到有效版本或等待 5 秒后恢复。

| 模式 | AT 序列（每条以 `\r\n` 结尾） | 含义 | 如何进入 |
|:---:|:---|:---|:---|
| **1** | `AT+RX=2` →（约 700 ms）→ `AT+MODE=1` | 百问通话（开机默认） | 开机自动；设置 |
| **2** | `AT+TX=1` → → `AT+MODE=2` | 发射 / 配对；经蓝牙耳机对话 | 设置 → 蓝牙 → 模式 2 |
| **3** | `AT+RX=1` → → `AT+MODE=3` | 音乐接收（设备当音箱） | 设置 |

> 模式 2 对话需要 **带麦** 耳机。开机在 `MetalioAudio_Init` 后应用模式 1。

---

## 13. 开发环境

### 13.1 环境要求

| 项 | 要求 |
|:---|:---|
| **ESP-IDF** | **v6.1**（须与仓库 `sdkconfig` / S31 支持一致） |
| **Target** | `esp32s31`（已预配置；通常无需 `set-target`） |
| **板型** | Metalio E-Ink4-Plus（`main/boards/metalio-e-ink4-plus/`） |
| **系统** | Linux / macOS / Windows（推荐 WSL2） |
| **Python** | 3.8+（IDF 虚拟环境） |

### 13.2 安装 ESP-IDF

> [ESP32-S31 快速入门 — ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/zh_CN/v6.1/esp32s31/get-started/index.html)

```bash
# 推荐用 ESP-IDF Installation Manager（EIM），或：
git clone -b v6.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s31
. ./export.sh
idf.py --version
```

### 13.3 获取源码

```bash
git clone <your-repo-url>
cd xingzhi-ai-470
```

> 仓库自带板级调好的 `sdkconfig`；一般直接 `idf.py build` 即可。

### 13.4 关键配置摘要

| 项 | 值 | 说明 |
|:---|:---|:---|
| ESP-IDF | v6.1 | 必须匹配 |
| Target | esp32s31 | 已预配置 |
| Flash | 16MB | `partitions/v1/16m.csv` |
| PSRAM | Octal 200 MHz | |

---

## 14. 编译与烧录

### 14.1 编译

```bash
. ~/esp/v6.1/export.sh   # 按本机路径调整

idf.py build
```

### 14.2 关于 sdkconfig

默认使用 `CONFIG_COMPILER_OPTIMIZATION_SIZE=y`（`-Os`）控制固件体积。两个 OTA 应用分区各为 5 MiB；修改编译优化选项后，应确认固件通过分区大小检查。

修改 `sdkconfig` 前请确认相关配置的影响。 已针对墨水屏、PSRAM、分区调优；错误修改可能导致刷新、Wi-Fi 或 SD 挂载异常。

### 14.3 合并完整固件（发布）

```bash
./merge_firmware.sh
# 或：IDF_PATH=~/esp/v6.1 ./merge_firmware.sh
```

按 `build/flash_args` 编译并合并（含将 `use_font/font.fontpack` 写入 `font_data`）：

- `firmware/metalio-e-ink4-plus-{PROJECT_VER}.bin`
- 根目录 `metalio-e-ink4-plus.bin`
- `daily-builds/...`（本地归档）

### 14.4 烧录与监视

```bash
# 按本机改端口
idf.py -p /dev/ttyACM0 flash monitor
```

ESP32-S31 上 USB Serial/JTAG 与 HS MSC 使用不同 PHY；若已开虚拟 U 盘，烧录前仍建议安全弹出。

### 14.5 SD 卡与虚拟 U 盘

1. 格式化为 FAT32 后插入。
2. 按 §11.7 将资源放到 `metalio/e-ink/...`（PC 上不要再套一层名为 `sdcard` 的目录）。
3. 传输 → U 盘传输 → 开启虚拟 U 盘；拷贝后安全弹出；再关闭。

SD 卸载、USB 切换和待机恢复统一暂停新访问，并等待已有操作、事务及文件/目录句柄结束；等待超时会取消本次变更并保留文件系统，可关闭占用文件后重试。挂载恢复不会自动解除待机访问限制。

USB 描述符示例：`Metalio` / `Metalio Ink SD`（TinyUSB）。开启 MSC 时会将 **ANA_SW**（GPIO36）置低。

### 14.6 电子书转换（PC）

见 `tools/ebook/README.md`。输出拷到 SD 的 `metalio/e-ink/books/` 供阅读应用使用。

---

## 15. 调试与常见问题

### 15.1 常用日志 Tag

| Tag | 模块 |
|:---|:---|
| MetalioEInk4Plus | 板级初始化 |
| epd_eink4p | EPDiy / TCA / TPS65185 |
| PowerPolicy / power_hw | 低功耗 / 关机 |
| AssistantScreen | 百问 AI |
| BookScreen | 阅读 |
| CloudScreen | 传输 |
| BluetoothScreen | BT AT |
| Frontlight | 冷暖前光 |
| UsbVirtualDisk | MSC |

### 15.2 工厂测试入口

设置 → 测试：工厂 / 老化入口。普通用户可忽略。

### 15.3 常见问题

**Q: ESP-IDF 版本不对**

请用 **v6.1**，目标芯片 **esp32s31**，并执行 `export.sh`。

**Q: 屏幕不刷新 / 花屏**

1. 确认未破坏 `sdkconfig` 与面板参数
2. 检查经 TCA 的 TPS65185 上电（VCOM / PWRUP / WAKEUP / PWR_GOOD）
3. 核对 `config.h` 并行总线脚（版本相关的 XSTL / MODE）

**Q: 触摸失灵**

检查 FT6336 RST（TCA P1.2 为高）、INT（GPIO5）以及共用 I2C 0/1。

**Q: Wi-Fi 配网**

进入「设置 → 网络」，扫描并选择 Wi-Fi，输入密码后连接。

**Q: SD 挂载失败**

FAT32；检查 SDMMC 4-bit 脚（GPIO20–25）；S31 上必要时确认 CNNT SDIO pad 已释放给 GPIO。

**Q: 虚拟 U 盘**

在传输 → U 盘传输中开启；**ANA_SW** 会置低。关闭前请安全弹出。

**Q: 空闲后进待机或关机**

属预期行为 — 见 §2.5。可在设置 → 功耗中调整。

**Q: 电量跳变 / 充电图标异常**

见 `docs/battery-soc-and-charge.md`。UI SOC 为电压估算；默认 ICHG 为 1000 mA。

**Q: 推送 / 书库 API 失败**

确认网络；若用私有后端请填写 `cloudzao_endpoints.c`；在「传输」里点 **刷新**。

**Q: 与 Metalio E-Ink 4 搞混**

Plus 是 **S31 + 并行 4.7 寸 + FT6336 + 前光**。397 是 **S3 + SPI 3.97 寸 + CST816S**。引脚表不可互换。

---

_本文档随固件演进更新。引脚与行为以源码为准；若文档与硬件不符请提 Issue。_
