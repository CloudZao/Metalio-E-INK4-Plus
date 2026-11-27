# 蓝牙芯片音频协议

[English](ble_chip-audio-protocol.en.md)

## 1. 功能概述

蓝牙芯片通过 UART AT 命令切换音频工作模式，通过 I2S 与主控传输音频。工作模式包括本机音频、蓝牙发送、蓝牙接收、低功耗、蓝牙语音与 HID，以及 USB 语音与 HID。具体能力取决于芯片型号、模块固件和功能配置，蓝牙芯片 G 型、蓝牙芯片 H 型与 dongle 固件的功能不能直接互相等同。

470 当前提供模式 1、2、3、5 的界面入口。外置蓝牙音频模块与 ESP 片上 BLE 翻页器是独立通路。以下分别说明各模式的命令、应答、时序及使用限制。

## 2. 当前接入位置

- [板级配置](../main/boards/metalio-e-ink4-plus/config.h)：引脚与采样率的权威源。
- [SimpleUart](../main/boards/common/audio/bluetooth/simple_uart.hpp)：UART 初始化、原始字节收发、页面回调与独立观察者。
- [BluetoothAudio](../main/boards/common/audio/bluetooth/bluetooth_audio.cc)：上电、模式 1 初始化与应答等待。
- [蓝牙协议处理](../main/business/bluetooth/bluetooth_control.cc)：模式、名称、版本、SCO、HID 与文本应答。
- [蓝牙页面](../main/ui/apps/settings/bluetooth/bluetooth_screen.cc)：扫描、连接、复位和模式入口。
- [I2S 音频驱动](../main/boards/common/audio/bluetooth/bt_audio_codec.cc)：音频数据传输。
- [电源管理](../main/sysmgmt/power/policy/README.md)：共享供电与硬件复位。

当前主控使用 UART1，115200、8N1、无流控。GPIO 引脚值以板级配置为准；扩展协议使用的 UART1 TX=GPIO7、RX=GPIO8 是 **蓝牙芯片侧**编号，不是 ESP 主控引脚编号。

AT 命令以 `\r` 结束；也接受 `\r\n`。当前普通 AT 发送使用 `\r\n`，HID 使用 `\r`。本文命令示例中的转义符表示实际控制字节，不是发送反斜杠文字。HID 应答以 `\r\n` 结束；业务控制层解析按 `\n` 分行并去掉首尾空白，开机模式应答观察者可按 `\r` 或 `\n` 分行。串口一次读取不等于一条完整应答。

音频走 I2S，不通过 AT 文本传输。当前主控为 I2S Slave，蓝牙芯片提供时钟；输入输出配置为 16 kHz、32 位立体声槽。输入按 32 位数据右移 12 位并限幅至 16 位；输出将单声道样本送到左右槽。I2S 有效位、左右槽来源及不同模式的麦克风路由需与模块固件匹配。

## 3. 工作模式

### 模式 1：本机音频

```text
AT+RX=2\r\n
等待 700 ms
AT+MODE=1\r\n
应答: SET MODE 1
```

通过本机功放输出音频，并将麦克风音频同步给主控。当前开机默认进入此模式。开机路径在恢复供电稳定后发命令，模式应答等待上限 1000 ms，首次失败后最多再试 3 次；设置页切换路径没有相同的应答超时重试机制。

### 模式 2：蓝牙发送

```text
AT+TX=1\r\n
等待 700 ms
AT+MODE=2\r\n
应答: SET MODE 2
```

连接周围蓝牙音箱或耳机播放音频；有连接记录时可能主动回连并上报 `RECONNECT DEVICE NAME`。支持耳机麦克风或板载麦克风采集给主控；具体路由选择命令及槽位定义尚未明确。当前接入扫描、连接与音乐/通话切换；对话需要具备麦克风和相应通话能力的耳机。

### 模式 3：蓝牙接收

```text
AT+RX=1\r\n
等待 700 ms
AT+MODE=3\r\n
应答: SET MODE 3
```

当前已接入，接收手机等设备播放的音频。歌词和封面传输的报文、编码、分片及通道定义尚未明确；当前未见歌词/封面专用处理。

### 模式 4：低功耗

进入命令 `AT+MODE=4\r`，应答 `SET MODE 4`。进入后要求外部 MCU 与蓝牙芯片通信的 IO 设为高阻或拉低；退出要求将 **蓝牙芯片 GPIO20** 拉低持续 700 ms。尚未明确唤醒后的模式、应答及完整时序。

当前未接入该模式及 GPIO20 唤醒流程。现有“复位蓝牙”走共享电源硬件复位，不能等同于此低功耗唤醒，也不能将蓝牙芯片 GPIO20 解释为 ESP GPIO20。

### 模式 5：蓝牙语音与 HID

当前发送 `AT+MODE=5\r\n`，识别 `SET MODE 5`，提供 HID 预设测试页。支持经典蓝牙麦克风，以及经典/BLE HID 自定义按键；HID 连接方式、配对安全要求与音频格式没有完整定义。可用性取决于模块固件和电脑连接状态。

### 模式 6：USB 语音与 HID

通过 USB 向电脑传输麦克风音频并发送 HID 按键。切换命令、应答、USB 描述符、音频格式及接线要求尚未明确。不能仅按模式编号推断 `AT+MODE=6` 一定有效。当前无模式 6 入口或专用处理。

## 4. 扫描、连接与 SCO

```text
AT+INQUIRING\r\n
  INQUIRING START
  AT+BT:363154329632ble_chip_CY
  INQ COMPLETE

AT+CONNECT=363154329632\r\n
  CONNECTING
  CONNECT SUCCESS
  或 / or: CONNECT TIMEOUT
```

扫描项为 `AT+BT:` 后紧跟 12 位十六进制地址，再直接拼接名称，无分隔符。当前按此格式解析，名称缓存为 64 字节（含结束符）。取消扫描、主动断开、忘记配对、完整断连事件及连接超时值尚未明确。

连接成功默认 A2DP 音乐路径；通话和音乐切换顺序如下：

```text
通话:
AT+PP=1\r\n
等待 200 ms
AT+BTSCO=1\r\n
  SETUP SCO

音乐 / music:
AT+BTSCO=0\r\n
  DISC SCO
等待 / wait 200 ms
AT+PP=1\r\n
```

`AT+PP=1` 的精确参数语义未解释；它与下节无参数 `AT+PP` 不应直接认定为相同语义。当前 SCO 任务按固定延时发送，不等待 `SETUP SCO` / `DISC SCO` 后才继续。

## 5. 其他 AT 命令

- **版本：**当前发送 `AT+GVER=1\r\n`，识别 `AT+GVER=V...`；页面要求版本以 `V` 开头、长度 2～63 字节，字符限英文字母、数字及 `.-_+`。无参数形式 `AT+GVER\r` 的兼容性及返回格式尚未明确。
- **名称：**`AT+GLBN=1\r` 查询，返回 `AT+LOCAL_NAME=<名称>`。`AT+SLBN=<名称>\r` 设置后约 700 ms 重启；设置应答可能回显为 `AT+LOCAL_NAME=AT+SLBN=...`。当前代码过滤此回显，重启后恢复模式并重新查询；设置接口限制名称为 1～63 字节、拒绝控制字符，这是主控接口限制，模块名称上限尚未明确。界面仅查看名称，无修改入口。
- **媒体：**媒体控制命令包括 `AT+PP\r`（播放/暂停）、`AT+PREV\r`、`AT+NEXT\r`、`AT+VOLDOWN\r`；音量加命令 `AT+VOLUP\r` 的准确拼写尚未确认，暂不作为可直接使用的命令。`MPAUSE` 应答的触发条件尚未明确。当前未见这些无参数媒体命令的专用发送路径或 `MPAUSE` 专用解析；本机软件音量不等于模块媒体音量。
- **采样率：**模式 1 使用 `AT+SAMPLE=N\r`，N 为 `8000`、`16000`、`44100`、`48000`、`88200`、`96000`；未定义应答、默认值、持久化及重启要求。当前没有发送路径，仍按板级 16 kHz 配置；接入时需同步核对 I2S 与整条音频处理链路，不能仅发送该命令。
- **串口速率：**`AT+SUARTBAUD` 用于串口速率配置，参数和应答格式尚未明确。当前固定初始化 115200，未接入动态改速率。

## 6. HID 快捷键

格式为 `AT+SHORTCUT=<修饰键>+<普通键>\r`：多个修饰键，最多一个普通键；也可仅普通键或修饰键，不含空格。大写字母键名不自动附加 Shift，不支持直接 HID 键码。

- 修饰键：`LCTRL/RCTRL`、`LSHIFT/RSHIFT`、`LALT/RALT`、`LGUI/RGUI`、`LWIN/RWIN`；`CTRL/SHIFT/ALT/GUI/WIN` 为左侧别名。
- 普通键：`A`～`Z`（大小写均可）、`0`～`9`、`F1`～`F12`；`ENTER ESC BACKSPACE TAB SPACE`。
- 符号：`MINUS EQUAL LBRACKET RBRACKET BACKSLASH SEMICOLON QUOTE GRAVE COMMA DOT SLASH`。加号用 `SHIFT+EQUAL`，实际字符受主机布局影响
- 系统及导航：`CAPSLOCK PRTSC SCROLLLOCK PAUSE MENU INSERT HOME PGUP DELETE DEL END PGDN RIGHT LEFT DOWN UP`。

```text
AT+SHORTCUT=CTRL+C\r
AT+SHORTCUT=CTRL+SHIFT+ESC\r
AT+SHORTCUT=ALT,DOWN\r
AT+SHORTCUT=ALT,UP\r
AT+SHORTCUT=RELEASE\r
```

单击自动按下并释放。`,DOWN` 为长按，`,UP` 为释放，必须配对；`RELEASE` 释放全部。以上长按序列使用 Alt；其他组合的长按支持范围尚未明确。`CTRL++C`、`CTRL+C+V`、`CTRL + C`、`F13` 为错误示例。

应答均以 `\r\n` 结尾：`AT+SHORTCUT=OK` 仅表示参数有效、存在 HID 连接且已提交异步发送，不保证电脑收到或执行；`ERR` 表示参数/格式错误；`DISCONNECTED` 表示 HID 未启用或无已连接主机。

当前模式 5 测试页使用固定预设，支持 Alt 按住/释放及释放全部；松开、触摸取消、离页和切换模式前有释放处理。未提供任意组合编辑，也未建立 HID 应答与每次请求的序号关联。

## 7. UART GPIO 扩展 V3.0

### 7.1 适用固件与共存

该协议适用于启用 `GPIO_EXT_ENABLE` 的 `ble_chip_dongle` 固件。默认地址 `01`，蓝牙芯片 GPIO 数量 32，GPIO0/1（UART0）及 GPIO7/8（UART1）保留，可额外配置保留掩码。不能据此认定 470 所装固件已启用。

与 AT 共用蓝牙芯片 UART1：二进制从 `A5` 起按 LEN 接收，帧内 `00/0D/0A/A5/AT+` 都是数据；完整 AT 前缀后按文本收至 CR，名称可含 UTF-8 `A5` 字节。二进制请求必须放在 AT 行结束后，支持分包及 AT/二进制交替。PAYLOAD 上限 8 字节；超长帧按声明长度丢弃后回 LEN 错，不执行其中 AT 文本。残帧会持续等待声明长度收齐；残帧超时和重新同步规则尚未明确，不能保证主机超时重发后立即恢复。

### 7.2 帧与状态

```text
请求:
A5 ADDR SEQ CMD LEN PAYLOAD[LEN] CRC_H CRC_L
应答:
A5 ADDR SEQ (CMD | 80) STATUS LEN PAYLOAD[LEN] CRC_H CRC_L
事件:
A5 ADDR 00 C0 00 02 PIN MODE CRC_H CRC_L
```

CRC 为 CRC-16/CCITT-FALSE，初值 `FFFF`、多项式 `1021`，不反射、最终异或 `0000`，覆盖 ADDR 至 PAYLOAD（应答包含 STATUS），高字节先发。EVENT 的 `STATUS=00`，其 `CMD=C0`、`SEQ=00`，是主动事件，不能作为上一请求的应答。错误地址静默。

STATUS：`00` 成功；`01` CRC 错（应答 CMD=`FF`）；`02` 未知命令；`03` LEN 错；`04` PIN 非法；`05` 参数非法（含 UART 保留脚、非法 VAL/MODE）。

### 7.3 六条命令

- `00 PING`：无请求参数，成功应答无负载。
- `01 WRITE_PIN`：请求 `PIN,VAL`，应答同样两字节；改为输出，关闭该脚中断并驱动电平。`VAL=00/01` 对应低/高。
- `02 READ_PIN`：请求 `PIN`，应答 `VAL`；不改变方向。
- `03 CONFIG_INT`：请求、应答均为 `PIN,MODE`；改为输入、开启内部上拉，`MODE=00` 关中断、`01` 上升沿、`02` 下降沿，无双沿。
- `04 GET_INFO`：无参数，应答 `VER,GPIO_COUNT,FLAGS`；V3 为 `03,20,01`，FLAGS bit0 表示支持中断上报。
- `05 RESET`：无参数、无成功负载，应答成功发送后恢复全输入、中断全关；不是芯片重启或恢复音频模式。

中断事件负载为 `PIN,MODE`，发送后清除该脚 pending。下降沿保持等待 1→0，不会自动换上升沿；配置时已为低电平，需要先升高再降低。初始化仅建立软件输入默认状态，不修改既有 pinmux；实际改变发生于 WRITE_PIN、CONFIG_INT 或 RESET。该接口不保证按键去抖、逐沿计数或无事件丢失；除 UART 外引脚的实际用途必须结合板级电路确认。

EVENT 通过任务发送，ISR 仅置 pending、释放信号量；UART 全双工，事件可与主机发送并行。RESET 在应答发送成功后按当前波特率等待最多 129 个 UART 字节的发送时间并补偿 tick 取整，再恢复默认；应答发送失败则取消复位。

### 7.4 帧示例

以下均为十六进制字节，ADDR=`01`，示例脚为 **蓝牙芯片 GPIO18**；操作前确认未连接受保护外设。二进制帧不追加 CR/LF。

```text
PING TX:          A5 01 01 00 00 C5 44
PING RX:          A5 01 01 80 00 00 F6 B3
GET_INFO TX:      A5 01 02 04 00 50 D0
GET_INFO RX:      A5 01 02 84 00 03 03 20 01 10 C4
WRITE high TX:    A5 01 03 01 02 12 01 C8 86
WRITE high RX:    A5 01 03 81 00 02 12 01 14 9E
WRITE low TX:     A5 01 03 01 02 12 00 D8 A7
WRITE low RX:     A5 01 03 81 00 02 12 00 04 BF
READ TX:          A5 01 05 02 01 12 68 3A
READ high RX:     A5 01 05 82 00 01 01 7B A7
READ low RX:      A5 01 05 82 00 01 00 6B 86
FALLING TX:       A5 01 04 03 02 12 02 72 59
FALLING RX:       A5 01 04 83 00 02 12 02 A8 3F
FALLING EVENT:    A5 01 00 C0 00 02 12 02 51 24
RISING TX:        A5 01 04 03 02 12 01 42 3A
RISING EVENT:     A5 01 00 C0 00 02 12 01 61 47
INT disable TX:   A5 01 06 03 02 12 00 16 98
INT disable RX:   A5 01 06 83 00 02 12 00 03 3D
```

当前 `SimpleUart` 可以收发原始字节，但上层仅有 AT 文本解析，未见此协议的帧解析、CRC、序号匹配或 EVENT 分发。接入前应在共享 UART 接收层分流文本与二进制，并处理并发事件、超时及残帧；不能把二进制直接交给按换行解析的页面处理器。

## 8. 自定义键值持久化

需要 **蓝牙芯片固件 1.08 及以上**。本接口以数字编号 X 作为键、用户数据 Y 作为值；不是主控 NVS，也不写入 eFuse。旧版蓝牙固件不支持本节的用户数据存储命令。

```text
AT+WDATA=X,Y\r\n
  AT+WDATA=OK
AT+RDATA=X\r\n
  AT+RDATA=X,Y
AT+FLUSHDATA=1\r\n
  AT+FLUSHDATA=OK
```

X 为 0～999，Y 不超过 200 字节。断电前必须执行 FLUSHDATA 并确认成功，WDATA 应答不等于已持久保存。协议未提供独立的修复、删除或恢复默认命令，也未定义空记录、完整错误应答、Flash 原子性和寿命。

设置、持久保存与读取示例（编号 123 仅为示例，使用前确认未被其他功能占用）：

```text
AT+WDATA=123,factory_value\r\n
  AT+WDATA=OK
AT+FLUSHDATA=1\r\n
  AT+FLUSHDATA=OK
AT+RDATA=123\r\n
  AT+RDATA=123,factory_value
```

修改同一键时重新执行 WDATA 和 FLUSHDATA，再通过 RDATA 核对。读取不会保存待写数据；验证断电持久性需要重新上电后再次读取。

470 使用编号 470 保存屏幕校准记录，依次写入、刷 Flash、回读比对；持续观察者处理应答，多指令事务与模式命令、共享电源复位共用串口锁。数据格式及工具见 [VCOM 产测说明](../tools/vcom/README_zn.md)。
