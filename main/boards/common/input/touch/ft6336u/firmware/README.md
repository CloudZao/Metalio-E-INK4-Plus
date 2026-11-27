# FT6336U V04 升级

[English](README.en.md)

本目录固件仅用于 LT0470GF0430M_V1.1、684×1216、FT6336U 面板。
来自供应商 `LT0470GF0430M_V1.1_FT6336U_684x1216_FT6336U_V04_20261106_app.bin`。

- 文件大小：47176 字节；按头部长度烧写前 47096 字节。
- 版本：0x04；应用 XOR：0xFA；整个文件 CRC32：0xAFF8E72B。
- SHA256：`60a660a4f80bf7aa8000a2c597f83620ea9b815bec1db31be2c8002cce1ee089`。
- 不使用 `all.bin`，不把触摸固件直接交给 esptool。

## 使用

在 Plus 板型 `main/boards/metalio-e-ink4-plus/config.h` 中将
`BOARD_TOUCH_UPGRADE_ON_BOOT` 设为 `1`（随仓默认），编译 ESP32 固件并烧录。
设为 `0` 时不执行版本检查或升级，也不嵌入升级固件。配置值须为单行字面量 `0` 或 `1`。
整个升级期间保持电源连接。升级在 EPD/I2C 初始化之后、触摸采样、
LVGL 和后台监控启动之前执行，独占 I2C1。

串口日志标签为 `touch_upgrade`。成功日志为 `updated and verified V04`；
已有 V04 时输出 `already current; skipped`。保留该选项可在后续启动时继续检查版本。
刷写前请确认面板型号及该选项；已为 V04 时跳过升级。

升级先检查文件 CRC 和头部，每隔 200 ms 读取应用 ID 64/26 与版本，最多等待 1 秒。
应用信息无效时发送 0x07 复位，转入 Boot ID 识别；应用有效时通过 0xBC 的 AA/55 序列进入升级模式。
最多读取 Boot ID 30 次，仅确认 Bootloader ID 79/1C 后才擦除应用区。
逐包检查状态、校验 XOR，并在重启后确认芯片 ID 和版本。失败后尝试复位退出 Bootloader，
每次启动最多执行两次完整升级；第二次不凭版本号跳过写入，以免将部分写入误判为成功。
Bootloader 无法识别时不擦除；两次尝试后仍失败时触摸可能不可用，需检查供电、复位及通信，必要时使用供应商恢复工具。

应用失效回退与两次升级尝试依据供应商 Qualcomm V3.4 驱动的 `fts_get_ic_information()`、
`fts_fwupg_enter_into_boot()` 和 `fts_fwupg_upgrade()`。刷写协议依据供应商 `Focaltech_Touch_FT6336U_I2C_V4.1_20230424(1).zip` 中
`focaltech_upgrade_ft6336u.c` 及 `focaltech_flash.c`；实现没有直接复制 Linux 驱动代码。
