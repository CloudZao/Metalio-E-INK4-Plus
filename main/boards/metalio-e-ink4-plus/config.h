#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

/* ED047TC2_1216：原生 1216x684；逻辑竖屏 684x1216 */
#define DISPLAY_WIDTH  684
#define DISPLAY_HEIGHT 1216
/* UI 可绘区相对逻辑全屏内缩（基础规范：LVGL/触摸/布局一律遵守，勿按面板全尺寸排版） */
#define DISPLAY_CONTENT_LEFT_INSET   8
#define DISPLAY_CONTENT_RIGHT_INSET  8
#define DISPLAY_CONTENT_TOP_INSET    6
#define DISPLAY_CONTENT_BOTTOM_INSET 10
#define DISPLAY_CONTENT_W \
    (DISPLAY_WIDTH - DISPLAY_CONTENT_LEFT_INSET - DISPLAY_CONTENT_RIGHT_INSET)
#define DISPLAY_CONTENT_H \
    (DISPLAY_HEIGHT - DISPLAY_CONTENT_TOP_INSET - DISPLAY_CONTENT_BOTTOM_INSET)
/* 逻辑触点微调（inset 之后再加）；负 Y=上移。指腹触点常比目视中心偏下约一指节 */
#define DISPLAY_TOUCH_OFFSET_X  0
#define DISPLAY_TOUCH_OFFSET_Y  (-10)
#define EPD_VCOM_MV    1200

/* 并行数据 / 控制（原理图 V1.2） */
// #define EPD_PIN_D0     GPIO_NUM_8
// #define EPD_PIN_D1     GPIO_NUM_9
// #define EPD_PIN_D2     GPIO_NUM_10
// #define EPD_PIN_D3     GPIO_NUM_11
// #define EPD_PIN_D4     GPIO_NUM_12
// #define EPD_PIN_D5     GPIO_NUM_13
// #define EPD_PIN_D6     GPIO_NUM_14
// #define EPD_PIN_D7     GPIO_NUM_15
// #define EPD_PIN_XCL    GPIO_NUM_16
// #define EPD_PIN_XLE    GPIO_NUM_17
// #define EPD_PIN_XOE    GPIO_NUM_18
// #define EPD_PIN_XSTL   GPIO_NUM_19
// #define EPD_PIN_SPV    GPIO_NUM_35
// #define EPD_PIN_MODE   GPIO_NUM_36
// #define EPD_PIN_BORDER GPIO_NUM_37
// #define EPD_PIN_CKV    GPIO_NUM_40



/* 并行数据 / 控制（原理图 V1.2） */
#define EPD_PIN_D0     GPIO_NUM_8
#define EPD_PIN_D1     GPIO_NUM_9
#define EPD_PIN_D2     GPIO_NUM_10
#define EPD_PIN_D3     GPIO_NUM_11
#define EPD_PIN_D4     GPIO_NUM_12
#define EPD_PIN_D5     GPIO_NUM_13
#define EPD_PIN_D6     GPIO_NUM_14
#define EPD_PIN_D7     GPIO_NUM_15
#define EPD_PIN_XCL    GPIO_NUM_16
#define EPD_PIN_XLE    GPIO_NUM_17
#define EPD_PIN_XOE    GPIO_NUM_18
// #define EPD_PIN_XSTL   GPIO_NUM_19
#define EPD_PIN_XSTL   GPIO_NUM_4 //1.3/1.4版本硬件
// #define EPD_PIN_MODE   GPIO_NUM_36 // 1.2版本硬件
#define EPD_PIN_MODE   GPIO_NUM_19 //1.4版本硬件
#define EPD_PIN_CKV    GPIO_NUM_40
#define EPD_PIN_SPV    GPIO_NUM_35
#define EPD_PIN_BORDER GPIO_NUM_37

/* TPS65185 ↔ TCA9555：编号=手册 P0x/P1x（P10=P1 bit0 → 代码脚号 8） */
#define EPD_TPS_IO_VCOM_CTRL 0  // P00
#define EPD_TPS_IO_PWRUP     1  // P01
#define EPD_TPS_IO_WAKEUP    2  // P02
#define EPD_TPS_IO_PWR_GOOD  7  // P07
#define EPD_TPS_IO_INT       15 // 仅取 Port1；P17 现作电源键输入
#define EPD_IO_MOTOR         3  // P03 震动马达，高有效
#define EPD_IO_PA_EN         4  // P04 功放使能（NS4150 CTRL），高有效
#define EPD_IO_CAM_SCR_EN    5  // P05 摄像头/屏幕电源，高有效
#define VIBRATION_MOTOR_PULSE_MS 50 // 短震脉宽 ms

#define I2C_SDA_PIN          GPIO_NUM_0
#define I2C_SCL_PIN          GPIO_NUM_1
#define IO_EXPANDER_INT_GPIO GPIO_NUM_2
/* 同 I2C1：BQ27220@0x55、CX25601N@0x6B、PCF8563@0x51、SC7A20H@0x19/0x18 */
/* 1.4：PCF8563 INT → TCA P16（低有效）；不再经 MOSFET 进 ESP GPIO */
#define EPD_IO_RTC_INT 14 // P16 RTC 中断输入
#define RTC_INT_GPIO   GPIO_NUM_NC
#define PCF8563_I2C_ADDR 0x51
/* SC7A20H INT1 经 TLR2 与 TCA INT 线与到 GPIO2；驱动内配开漏低有效 */
#define SC7A20H_I2C_ADDR_DEFAULT 0x19
#define SC7A20H_I2C_ADDR_ALT     0x18

/* FT6336U：I2C 与 TPS/TCA 同总线；INT=GPIO5；RST=IO_P12=P12（勿用 ESP GPIO12=EPD D4） */
#define TOUCH_I2C_ADDR 0x38
#define TOUCH_INT_PIN  GPIO_NUM_5
#define EPD_IO_TP_RST  10 // P12 触摸等电源/RST，高有效（低=断电）
#define EPD_IO_BT_PA_PWR 12 // P14 蓝牙+功放电源，高有效
#define ANA_SW_GPIO      GPIO_NUM_36 // 1.4：模拟开关（原 TCA P16）

/* 盖板虚拟键：FT 报点 Y≈1600（屏外）；X 实测 home=100 / 返回·上页=200 / 下页=300 */
#define TOUCH_VK_Y       1600
#define TOUCH_VK_TOL     30 // 命中容差
#define TOUCH_VK_HOME_X  100
#define TOUCH_VK_PREV_X  200 // 返回 / 上页
#define TOUCH_VK_NEXT_X  300 // 下页
#define TOUCH_VK_LONG_MS 500 // 长按阈值（对齐 397 BOOT 长按）

/* 按键：空闲采样定极性；IO_P1x = 手册 P1x */
#define BOOT_BUTTON_GPIO  GPIO_NUM_61
#define EPD_IO_POWER      15 // P17 电源键
#define EPD_IO_VOL_UP     8  // P10 音量+
#define EPD_IO_VOL_DOWN   9  // P11 音量-
#define EPD_IO_PWR_PULSE  11 // P13 关机脉冲，默认低
#define POWER_LONG_PRESS_MS     3000 // 电源键长按触发关机脉冲
#define PWR_PULSE_INTERVAL_MS   100  // 高低各保持时长
#define PWR_PULSE_COUNT         15   // 高低脉冲次数

/* 前光 YX6016N EN/PWM：GPIO6=冷，GPIO7=暖 */
#define FL_COOL_PIN GPIO_NUM_6
#define FL_WARM_PIN GPIO_NUM_7

/*
 * SDMMC Slot0 IOMUX = 原理图 U5 专用 SDIO（封装脚≠GPIO 号）：
 *   SDIO_CLK  脚32 → GPIO24
 *   SDIO_CMD  脚33 → GPIO25
 *   SDIO_DATA0 脚27 → GPIO20
 *   SDIO_DATA1 脚28 → GPIO21
 *   SDIO_DATA2 脚29 → GPIO22
 *   SDIO_DATA3 脚31 → GPIO23
 */
#define SDMMC_CLK_PIN GPIO_NUM_24
#define SDMMC_CMD_PIN GPIO_NUM_25
#define SDMMC_D0_PIN  GPIO_NUM_20
#define SDMMC_D1_PIN  GPIO_NUM_21
#define SDMMC_D2_PIN  GPIO_NUM_22
#define SDMMC_D3_PIN  GPIO_NUM_23

/*
 * 蓝牙音频 BH1098G（原理图 V1.2）
 * I2S：ESP Slave ← 外置芯片时钟；UART：AT 控模式
 * PA_EN：TCA IO_P4，1=开 0=关（NS4150B CTRL）
 */
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000
#define AUDIO_I2S_BCLK GPIO_NUM_42 // I2S_BCK
#define AUDIO_I2S_WS   GPIO_NUM_43 // I2S_WS
#define AUDIO_I2S_DOUT GPIO_NUM_44 // I2S_DO（主控→喇叭）
#define AUDIO_I2S_DIN  GPIO_NUM_45 // I2S_DI（麦→主控）
/* 38/39 接 BH 串口；无 BT RX 回显时对调（当前：39=TX，38=RX） */
#define BT_AUDIO_TX_PIN GPIO_NUM_39 // 主控 TX → 蓝牙芯片
#define BT_AUDIO_RX_PIN GPIO_NUM_38 // 主控 RX ← 蓝牙芯片

#endif /* _BOARD_CONFIG_H_ */
