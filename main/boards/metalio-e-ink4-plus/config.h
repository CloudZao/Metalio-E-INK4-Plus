#ifndef METALIO_E_INK4_PLUS_CONFIG_H
#define METALIO_E_INK4_PLUS_CONFIG_H

#include <driver/gpio.h>
#include "sdkconfig.h"

// 显示接口与面板校准
#define DISPLAY_WIDTH 684    // ED047TC2_1216 逻辑竖屏宽度，原生面板 1216×684
#define DISPLAY_HEIGHT 1216  // 逻辑竖屏高度
#define DISPLAY_CONTENT_LEFT_INSET 2    // 普通 UI 左内缩；壁纸、待机和关机画面使用完整面板
#define DISPLAY_CONTENT_RIGHT_INSET 2   // 普通 UI 右内缩
#define DISPLAY_CONTENT_TOP_INSET 2     // 普通 UI 上内缩
#define DISPLAY_CONTENT_BOTTOM_INSET 2  // 普通 UI 下内缩
#define DISPLAY_WHITE_BORDER_PIXELS 0
#define DISPLAY_CONTENT_W (DISPLAY_WIDTH - DISPLAY_CONTENT_LEFT_INSET - DISPLAY_CONTENT_RIGHT_INSET)
#define DISPLAY_CONTENT_H (DISPLAY_HEIGHT - DISPLAY_CONTENT_TOP_INSET - DISPLAY_CONTENT_BOTTOM_INSET)
#define DISPLAY_TOUCH_OFFSET_X 0  // 内缩后的触点 X 偏移
#define DISPLAY_TOUCH_OFFSET_Y 0  // 内缩后的触点 Y 偏移，负值向上
#define EPD_VCOM_MV 1200
#define EPD_VCOM_MIN_MV 1000  // 屏幕校准电压绝对值下限，毫伏
#define EPD_VCOM_MAX_MV 3000  // 屏幕校准电压绝对值上限，毫伏
#define EPD_PIN_D0 GPIO_NUM_8
#define EPD_PIN_D1 GPIO_NUM_9
#define EPD_PIN_D2 GPIO_NUM_10
#define EPD_PIN_D3 GPIO_NUM_11
#define EPD_PIN_D4 GPIO_NUM_12
#define EPD_PIN_D5 GPIO_NUM_13
#define EPD_PIN_D6 GPIO_NUM_14
#define EPD_PIN_D7 GPIO_NUM_15
#define EPD_PIN_XCL GPIO_NUM_16
#define EPD_PIN_XLE GPIO_NUM_17
#define EPD_PIN_XOE GPIO_NUM_18
#define EPD_PIN_XSTL GPIO_NUM_4   // 硬件 1.3/1.4
#define EPD_PIN_MODE GPIO_NUM_19  // 硬件 1.4
#define EPD_PIN_CKV GPIO_NUM_40
#define EPD_PIN_SPV GPIO_NUM_35
#define EPD_PIN_BORDER GPIO_NUM_37
#define EPD_TPS_IO_VCOM_CTRL 0  // TCA9555 P00；P1x 对应编号 8+x
#define EPD_TPS_IO_PWRUP 1      // P01
#define EPD_TPS_IO_WAKEUP 2     // P02
#define EPD_TPS_IO_PWR_GOOD 7   // P07
#define EPD_TPS_IO_INT 15       // 仅取 Port1；P17 现作电源键输入

// 共享I2C与电源控制
#define EPD_IO_PA_EN 4          // P04 功放使能（NS4150 CTRL），高有效
#define EPD_IO_CAM_SCR_EN 5     // P05 摄像头/屏幕电源，高有效
#define POWER_DISCHARGE_MOTOR_PULSE_MS 3000             // 共享电源断电后马达持续置高的放电时长 ms
#define POWER_BT_RESET_LOW_MS 3000                     // 蓝牙手动复位时共享电源的最短断电时长 ms
#define POWER_RAIL_SETTLE_MS 80                         // 外围电源上电后、恢复下一路电源或显示总线前的稳定等待 ms
#define I2C_SDA_PIN GPIO_NUM_0  // 共享外设 I2C SDA
#define I2C_SCL_PIN GPIO_NUM_1
#define IO_EXPANDER_INT_GPIO GPIO_NUM_2
#define EPD_IO_TP_RST 10              // P12 触摸/4G 共享复位，低有效；4G 正常工作须为高，勿用 ESP GPIO12
#define EPD_IO_CAM_RST EPD_IO_TP_RST  // GC2145 共用触摸复位脚；运行期仅软件复位，禁止拉低共享脚
#define EPD_IO_BT_PA_PWR 12           // P14 4G+蓝牙+功放共享电源，高有效
#define ANA_SW_GPIO GPIO_NUM_36       // 1.4：模拟开关（原 TCA P16）

// 摄像头
#define CAM_SCCB_ADDR 0x3C
#define CAM_PIN_D0 GPIO_NUM_46
#define CAM_PIN_PCLK GPIO_NUM_54
#define CAM_PIN_MCLK GPIO_NUM_55
#define CAM_PIN_VSYNC GPIO_NUM_56
#define CAM_PIN_HREF GPIO_NUM_57
#define CAM_MCLK_HZ 20000000

// 运动传感器与RTC
#define EPD_IO_RTC_INT 14  // 硬件 1.4：PCF8563 INT 接 P16，低有效
#define RTC_INT_GPIO GPIO_NUM_NC
#define PCF8563_I2C_ADDR 0x51
#define SC7A20H_I2C_ADDR_DEFAULT 0x19  // 默认地址；INT1 经 TLR2 与 TCA INT 线与，驱动配置开漏低有效
#define SC7A20H_I2C_ADDR_ALT 0x18

// 触摸与按键
#define EPD_IO_MOTOR 3          // P03 震动马达，高有效
#define VIBRATION_MOTOR_PULSE_MS 50                     // 短震脉宽 ms
#define BOARD_VIBRATION_FEEDBACK_PULSE_MS {35, 50, 70}  // 弱、中、强触摸反馈脉宽，毫秒
#define BOARD_TOUCH_UPGRADE_ON_BOOT 1 // 启动时检查并升级FT6336U固件；仅适用本板面板，升级期间保持供电
#define TOUCH_I2C_ADDR 0x38  // FT6336U 地址，与 TPS65185、TCA9555 共用 I2C 总线
#define TOUCH_INT_PIN GPIO_NUM_5
#define TOUCH_VK_Y 1600      // 盖板虚拟键屏外 Y 坐标
#define TOUCH_VK_TOL 30      // 命中容差
#define TOUCH_VK_HOME_X 100  // HOME X
#define TOUCH_VK_PREV_X 200  // 返回 / 上页
#define TOUCH_VK_NEXT_X 300  // 下页
#define BOOT_BUTTON_GPIO GPIO_NUM_61  // BOOT；按键空闲采样确定极性
#define EPD_IO_POWER 15               // P17 电源键
#define EPD_IO_VOL_UP 8               // P10 音量+
#define EPD_IO_VOL_DOWN 9             // P11 音量-
#define EPD_IO_PWR_PULSE 11           // P13 关机脉冲，默认低
#define PWR_PULSE_INTERVAL_MS 100     // 高低各保持时长
#define PWR_PULSE_COUNT 15            // 高低脉冲次数
#define BOARD_BOOT_LONG_PRESS_MS 500    // BOOT与侧键长按阈值，毫秒
#define BOARD_POWER_LONG_PRESS_MS 3000  // 电源键关机阈值，毫秒

// 前光
#define FL_COOL_PIN GPIO_NUM_6  // YX6016N 冷光 EN/PWM
#define FL_WARM_PIN GPIO_NUM_7  // YX6016N 暖光 EN/PWM
#define BOARD_HAS_FRONTLIGHT 1                  // 前光硬件能力
#define BOARD_FRONTLIGHT_MAX_OUTPUT_PERCENT 10  // 前光各通道 PWM 占空比上限，百分比

// SD卡
#define SDMMC_CLK_PIN GPIO_NUM_24  // SDMMC Slot0 CLK，原理图 U5 封装脚 32
#define SDMMC_CMD_PIN GPIO_NUM_25  // SDMMC Slot0 CMD，原理图 U5 封装脚 33
#define SDMMC_D0_PIN GPIO_NUM_20   // SDMMC Slot0 DAT0，原理图 U5 封装脚 27
#define SDMMC_D1_PIN GPIO_NUM_21   // SDMMC Slot0 DAT1，原理图 U5 封装脚 28
#define SDMMC_D2_PIN GPIO_NUM_22   // SDMMC Slot0 DAT2，原理图 U5 封装脚 29
#define SDMMC_D3_PIN GPIO_NUM_23   // SDMMC Slot0 DAT3，原理图 U5 封装脚 31

// 音频与蓝牙
#define AUDIO_INPUT_SAMPLE_RATE 16000   // 蓝牙芯片麦克风采样率，赫兹
#define AUDIO_OUTPUT_SAMPLE_RATE 16000  // 蓝牙芯片喇叭采样率，赫兹
#define AUDIO_I2S_BCLK GPIO_NUM_42      // I2S BCK，主控为从设备，时钟由蓝牙芯片提供
#define AUDIO_I2S_WS GPIO_NUM_43        // I2S_WS
#define AUDIO_I2S_DOUT GPIO_NUM_44      // I2S_DO（主控→喇叭）
#define AUDIO_I2S_DIN GPIO_NUM_45       // I2S_DI（麦→主控）
#define BT_AUDIO_TX_PIN GPIO_NUM_39  // 主控 UART TX → 蓝牙芯片，AT 控制
#define BT_AUDIO_RX_PIN GPIO_NUM_38  // 主控 UART RX ← 蓝牙芯片

// 蜂窝通信
#define NT26_TX_PIN GPIO_NUM_58
#define NT26_RX_PIN GPIO_NUM_59
#define NT26_MRDY_PIN GPIO_NUM_60
#define NT26_SRDY_PIN GPIO_NUM_3

// 显示刷新与界面
#define BOARD_DISPLAY_GRAY_LEVELS 16            // 原生显示灰阶数
#define BOARD_REFRESH_PROFILE_MASK 7                       // 刷新档位位图：bit0清晰、bit1均衡、bit2快速
#define BOARD_REFRESH_PROFILE_DEFAULT 0                    // 默认刷新档位，保持持久化枚举值
#define BOARD_DISPLAY_PARTIALS_PER_FULL 24                 // 连续局刷达到此次数后允许周期全刷
#define BOARD_DISPLAY_FULL_REFRESH_MIN_INTERVAL_MS 60000   // 周期全刷最短间隔，毫秒；0 表示不限制
#define BOARD_HAS_VCOM 1                                   // 支持读取和校准面板VCOM
#define BOARD_UI_FONT_SIZE 30                              // 本机系统界面字号
#define BOARD_UI_LARGE_FONT_SIZE 30                        // 大号内容及数值字号
#define BOARD_UI_EMPHASIS_NATIVE_BPP 4                     // 强调字体资源精度，0 表示直接合成加粗
#define BOARD_UI_EMPHASIS_EXPAND_PX 1                      // 无匹配强调资源时横向加粗宽度，0 关闭，支持 1、2px
#define BOARD_UI_CAPTION_FONT_SIZE 25                      // 辅助说明及紧凑区域字号
#define BOARD_THEME_DARK_DEFAULT 0                    // 默认深色模式开关
#define BOARD_HOME_STYLE_DEFAULT 2                    // 默认首页样式：0白底、1灰底、2描边、3斜线、4像素

// 阅读默认值
#define BOARD_SUPPORTS_PAGE_WIPE 1  // 推白翻页能力
#define BOARD_READER_FONT_SIZE 40                          // 默认阅读字号
#define BOARD_READER_FONT_MIN 20 // 阅读矢量字号下限，像素
#define BOARD_READER_FONT_MAX 80                           // 阅读矢量字号上限
#define BOARD_READER_FONT_FILE "misans_40_2.ef"            // 默认阅读位图字体
#define BOARD_READER_MARGIN_DEFAULT {24, 24, 24, 16}  // 默认左、右、上、下边距，像素
#define BOARD_READER_MARGIN_MIN 0                     // 阅读边距下限，像素
#define BOARD_READER_MARGIN_MAX 64                    // 阅读边距上限，像素
#define BOARD_READER_LINE_SPACING_PERCENT 10          // 自动行距占字体行框的百分比
#define BOARD_READER_LINE_GAP_MIN 0                   // 手动行距下限，像素
#define BOARD_READER_LINE_GAP_MAX 48                  // 手动行距上限，像素
#define BOARD_READER_PARA_GAP_DEFAULT 56              // 默认额外段距，像素
#define BOARD_READER_PARA_GAP_MIN 0                   // 段距下限，像素
#define BOARD_READER_PARA_GAP_MAX 104                 // 段距上限，像素
#define BOARD_READER_AA_DEFAULT 0                     // 默认阅读抗锯齿开关
#define BOARD_READER_AA_STRENGTH_DEFAULT 1            // 默认抗锯齿强度：0低、1中、2高
#define BOARD_READER_AUTO_TURN_SECONDS 10             // 默认自动翻页间隔，秒；开关仅当前会话生效
#define BOARD_READER_AUTO_TURN_MIN_SECONDS 5          // 自动翻页间隔下限，秒
#define BOARD_READER_AUTO_TURN_MAX_SECONDS 45         // 自动翻页间隔上限，秒
#define BOARD_READER_PAGE_TURN_DEFAULT 0              // 默认翻页方式：0滑动、2推白；须符合板卡能力
#define BOARD_READER_FOOTER_MASK_DEFAULT 3            // 默认底栏信息：bit0章节、bit1进度、bit2电量、bit3时间
#define BOARD_READER_TOC_ROWS 9                       // 目录每页目标行数，含存在时的正文入口；空间不足时减少

// 电池与充电
#define BOARD_BATTERY_FULL_MV 4300              // 满电确认电压
#define BOARD_BATTERY_FULL_EXIT_MV 4280         // 退出满电显示的电压，毫伏；须低于满电确认电压
#define BOARD_BATTERY_STEP_PERCENT 5            // 电量显示步长
#define BOARD_BATTERY_LINEAR 0                  // 线性电压换算
#define BOARD_BATTERY_DISCHARGE_MV {3300, 3481, 3565, 3624, 3663, 3702, 3757, 3828, 3919, 4021, 4211} // 424860-2000实测曲线，按1870.7mAh归一化，每10%取点，单位毫伏
#define BOARD_CHARGE_CURRENT_OPTIONS_MA {500, 1000, 1500}  // 产品允许的充电电流档位
#define BOARD_CHARGE_CURRENT_DEFAULT_MA 1000               // 默认充电电流，毫安

// CPU与超时策略
#define BOARD_IDLE_CPU_MHZ 320                  // 默认空闲频率
#define BOARD_IDLE_CPU_LOW_MHZ 160              // 空闲低频档
#define BOARD_IDLE_CPU_MID_MHZ 240              // 空闲中频档
#define BOARD_IDLE_CPU_HIGH_MHZ 320             // 空闲高频档
#define BOARD_IDLE_TO_STANDBY_SECONDS (5 * 60) // 无操作进入浅睡待机的秒数
#define BOARD_NET_GRACE_OPTIONS_SECONDS {30, 60, 120, 0} // 空闲保网档位，秒；0表示永不因空闲断网
#define BOARD_NET_GRACE_SECONDS 30                    // 默认空闲保网时长，秒；0表示永不自动断网
#define BOARD_STANDBY_OFF_OPTIONS_SECONDS {1800, 3600, 10800, 86400} // 待机累计自动关机档位，秒；各项须大于0
#define BOARD_STANDBY_OFF_SECONDS 1800                // 默认待机累计自动关机时长，秒；须在档位内

// 产测
#define BOARD_AGING_RESTART_SECONDS 300               // 老化测试定时重启间隔，秒

// 产品默认项与设备标识
#define BOARD_USB_MANUFACTURER "Metalio"  // USB 设备制造商标识
#define BOARD_AP_SSID_PREFIX "Metalio"    // 文件传输热点名称前缀
#define BOARD_WIFI_MAX_TX_POWER 48  // Wi-Fi 发射功率上限，0 不覆盖各模式原有配置，单位 0.25dBm

#if CONFIG_PRODUCT_GLOBAL
#define BOARD_DEFAULT_LANGUAGE "en-US"  // 未保存语言偏好时的默认语言
#define BOARD_CHAT_PROVIDER_DEFAULT 2   // 默认对话云服务：0百问、1小智、2网易
#else
#define BOARD_DEFAULT_LANGUAGE "zh-CN"  // 未保存语言偏好时的默认语言
#define BOARD_CHAT_PROVIDER_DEFAULT 0   // 默认对话云服务：0百问、1小智、2网易
#endif

#endif /* METALIO_E_INK4_PLUS_CONFIG_H */
