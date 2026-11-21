# Plus 设备档案

适用基准：4.7 英寸 Metalio E-INK4 Plus / ESP32-S31；官方源码提交 `ad8edf4150a688737c0d7c00cae4413079b262ef`，ESP-IDF `fff9895c82d744c7237be8847347bdd1b07c6643`（6.1.0）。本表是该版本代码事实，不是重新测量的电气规格，也不是任意硬件批次的保证。工具和源码从本 Skill 的运维模块获取，安装与业务工程都在独立工作目录。

| 部分 | 已核对的配置 | 使用要点 |
| --- | --- | --- |
| 芯片 | ESP32-S31，RISC-V，镜像 chip ID `0x0020` | 不使用旧 ESP32-S3/Xtensa 构建缓存或 BIN；该 SDK 的 S31 命令使用 `--preview`，详见构建模块。 |
| 面板 | `E0470A14_AF`，物理 **1216×684**，34 pin，8-bit，20 MHz | `epd_board_metalio_eink4_plus` + `EPD_LUT_1K`；现有 Plus UI 常用旋转后的逻辑竖屏 **684×1216**。 |
| 坐标 | 竖屏逻辑 `(x,y)` → 物理 `(y,683-x)` | 横屏应用若接收上述竖屏 HAL 坐标，同样转换到横屏；实际 HAL 已经转换时不可再旋转。图像与命中区一同验算。 |
| 像素 | native 4 bpp，偶数物理 x 为低半字节，奇数为高半字节；0 黑、15 白 | 一帧 415,872 字节，物理每行 608 字节；资源格式另见显示文档。 |
| 波形 | 当前板级使用 `MY_WAVEFORM`，VCOM 配置 1200 mV | 不能因字体浅直接改电压、LUT 或温度参数；代码序列不等于光学校准。 |
| PSRAM | 当前 OCT 200 MHz 配置 | 大画面/音频片段可用 PSRAM；DMA、驱动内部块及要求内部 RAM 的栈单独预算。 |
| 触摸 | FT6336U，I2C 地址 `0x38`，共享 I2C1；SDA GPIO0、SCL GPIO1、IRQ GPIO5，复位 TCA P1.2 | 保留总线互斥、暂停代次、可信松手和屏外盖板键分类；不能把盖板越界坐标夹到屏幕边缘。 |
| TF | SDMMC 4-bit；D0–D3 GPIO20–23，CLK24，CMD25 | 保留对齐及 INTERNAL/DMA bounce buffer；挂载和 USB 所有权另见存储文档。 |
| USB | GPIO36 ANA_SW；高为普通串口/调试，低为 HS UTMI MSC | S31 的 UTMI 与 USJ 是不同硬件 PHY；不套旧 S3 切换序列。 |
| 声音 | BH1098G；BCLK42、WS43、DOUT44、DIN45；UART1 TX39/RX38，115200 | ESP 为 I2S 从机；只有需要声音时才加载音频文档。 |
| 实体键 | AI 为 GPIO61 低有效；音量+/− 为 TCA P10/P11；电源为 P17 | TCA 端口号不是同号 ESP GPIO。 |
| 共享控制 | PA=P04；P14 为共享电源/复位；Port0 同时涉及马达与显示供电 | 不能整字节写死或用陈旧影子覆盖其他位；普通停录不切低 P14。 |

板级公开入口：[固定提交的官方工程](https://github.com/CloudZao/Metalio-E-INK4-Plus/tree/ad8edf4150a688737c0d7c00cae4413079b262ef)。先在已获取工程中查看对应 `main/boards/metalio-e-ink4-plus/`、音频 `periph/metalio_audio.cc` 与所使用 EPD 组件；不要为了查一个参数全量加载官方文档。驱动配置若与本表不同，先查明版本/板型，不静默混用。

Flash、独立启动和 Plus 口袋分区属于 [集成模块](../../pocket-integration/MODULE.md) 的版本化合同，本表不另维护第二套布局。
