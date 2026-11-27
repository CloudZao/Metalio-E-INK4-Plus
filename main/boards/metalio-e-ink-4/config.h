#ifndef METALIO_E_INK_4_CONFIG_H
#define METALIO_E_INK_4_CONFIG_H

#include <driver/gpio.h>
#include "sdkconfig.h"

#define AUDIO_INPUT_SAMPLE_RATE 16000   // 麦采样率
#define AUDIO_OUTPUT_SAMPLE_RATE 16000  // 喇叭采样率

#define AUDIO_I2S_MIC_GPIO_WS GPIO_NUM_43   // I2S 麦 WS
#define AUDIO_I2S_MIC_GPIO_DIN GPIO_NUM_17  // I2S 麦 DIN
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7  // I2S 喇叭 DOUT
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_6  // I2S 喇叭 BCLK

#define NT26_RX_PIN GPIO_NUM_11    // NT26 UART RX
#define NT26_TX_PIN GPIO_NUM_12    // NT26 UART TX
#define NT26_MRDY_PIN GPIO_NUM_21  // NT26 MRDY
#define NT26_SRDY_PIN GPIO_NUM_5   // NT26 SRDY

#define BT_AUDIO_TX_PIN GPIO_NUM_48  // 蓝牙音频 UART TX
#define BT_AUDIO_RX_PIN GPIO_NUM_47  // 蓝牙音频 UART RX

#define BOOT_BUTTON_GPIO GPIO_NUM_0          // BOOT（IO0）
#define POWER_BUTTON_GPIO GPIO_NUM_3         // 电源键（开关机芯片）
#define TOUCH_BUTTON_GPIO GPIO_NUM_NC        // 未用物理触摸键
#define VOLUME_UP_BUTTON_GPIO GPIO_NUM_NC    // 音量+（扩展器）
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC  // 音量-（扩展器）

#define BOARD_BOOT_LONG_PRESS_MS 500    // BOOT与侧键长按阈值，毫秒
#define BOARD_POWER_LONG_PRESS_MS 3000  // 电源键关机阈值，毫秒

#define SDMMC_CLK_PIN GPIO_NUM_38   // SD CLK
#define SDMMC_CMD_PIN GPIO_NUM_40   // SD CMD
#define SDMMC_D0_PIN GPIO_NUM_39    // SD DAT0
#define SDMMC_DAT3_PIN GPIO_NUM_46  // SD DAT3/CD（S3 仅输入）；1-bit 模式须保持高，防止进入 SPI 模式

#define USB_OTG_DM_PIN GPIO_NUM_19  // USB OTG D-
#define USB_OTG_DP_PIN GPIO_NUM_20  // USB OTG D+

#define DISPLAY_WIDTH 800               // GDEM0397T81（SSD1677）面板宽度
#define DISPLAY_HEIGHT 480              // 面板高
#define DISPLAY_CONTENT_LEFT_INSET 0    // 内容左边距
#define DISPLAY_CONTENT_RIGHT_INSET 0   // 内容右边距
#define DISPLAY_CONTENT_TOP_INSET 0     // 内容上边距
#define DISPLAY_CONTENT_BOTTOM_INSET 0  // 内容下边距

#define EPD_SPI_HOST SPI3_HOST    // 墨水屏 SPI 主机
#define EPD_SPI_CLK_HZ 10000000   // 墨水屏 SPI 时钟
#define EPD_PIN_MOSI GPIO_NUM_8   // EPD MOSI
#define EPD_PIN_SCLK GPIO_NUM_14  // EPD SCLK
#define EPD_PIN_CS GPIO_NUM_45    // EPD CS
#define EPD_PIN_DC GPIO_NUM_13    // EPD DC
#define EPD_PIN_RST GPIO_NUM_18   // EPD RST
#define EPD_PIN_BUSY GPIO_NUM_9   // EPD BUSY

#define I2C_SDA_PIN GPIO_NUM_41  // I2C SDA
#define I2C_SCL_PIN GPIO_NUM_42  // I2C SCL

#define TOUCH_INT_GPIO GPIO_NUM_1  // 触摸 INT
#define TOUCH_I2C_HZ 400000        // CST816S Fast Mode 速率，赫兹

#define IO_EXPANDER_INT_GPIO GPIO_NUM_2  // TCA9555 INT（ACCEL_INT 在扩展器 P1.4）

#define VIBRATION_MOTOR_GPIO GPIO_NUM_44                // 震动马达，高有效；禁止用 #if 与 GPIO_NUM_NC 比较
#define VIBRATION_MOTOR_PULSE_MS 35                     // 短震脉宽 ms
#define BOARD_VIBRATION_FEEDBACK_PULSE_MS {35, 50, 70}  // 弱、中、强触摸反馈脉宽，毫秒

#define TOUCH_VK_HOME_X 80   // HOME X，采用 CST816S 原生竖屏坐标（480×800）
#define TOUCH_VK_HOME_Y 900  // HOME Y
#define TOUCH_VK_PREV_X 400  // 上一页 X（盖板右）
#define TOUCH_VK_PREV_Y 900  // 上一页 Y
#define TOUCH_VK_NEXT_X 240  // 下一页 X（盖板中）
#define TOUCH_VK_NEXT_Y 900  // 下一页 Y

#define DISPLAY_MIRROR_X false  // 面板 X 镜像
#define DISPLAY_MIRROR_Y false  // 面板 Y 镜像

#define DISPLAY_CONTENT_W DISPLAY_HEIGHT  // 竖屏内容宽度
#define DISPLAY_CONTENT_H DISPLAY_WIDTH   // 竖屏内容高度

#define IO_EXPANDER_I2C_ADDR 0x20    // 扩展器7位地址
#define IOX_PIN_SCREEN_SOCKET_PWR 5  // 扩展器SCREEN_SOCKET_PWR连接
#define IOX_PIN_MAIN_PWR 6           // 扩展器MAIN_PWR连接
#define IOX_PIN_PA 4                 // 扩展器PA连接
#define IOX_PIN_PA_SWITCH 1          // 扩展器PA_SWITCH连接
#define IOX_PIN_VOLUME_DOWN 7        // 扩展器VOLUME_DOWN连接
#define IOX_PIN_VOLUME_UP 8          // 扩展器VOLUME_UP连接
#define IOX_PIN_TOUCH_RST 9          // 扩展器TOUCH_RST连接
#define IOX_PIN_PWR_KEY_PULSE 11     // 扩展器PWR_KEY_PULSE连接
#define IOX_PIN_ACCEL_INT 12         // 扩展器ACCEL_INT连接
#define IOX_PIN_USB_MUX_SEL 0        // 扩展器USB_MUX_SEL连接

#define BOARD_HAS_FRONTLIGHT 0                  // 前光硬件能力
#define BOARD_FRONTLIGHT_MAX_OUTPUT_PERCENT 10  // 前光各通道 PWM 占空比上限，百分比
#define BOARD_DISPLAY_GRAY_LEVELS 2             // 原生显示灰阶数，其他灰色使用抖点
#define BOARD_IDLE_CPU_MHZ 240                  // 默认空闲频率
#define BOARD_IDLE_CPU_LOW_MHZ 80               // 空闲低频档
#define BOARD_IDLE_CPU_MID_MHZ 160              // 空闲中频档
#define BOARD_IDLE_CPU_HIGH_MHZ 240             // 空闲高频档
#define BOARD_BATTERY_FULL_MV 4330              // 满电确认电压
#define BOARD_BATTERY_FULL_EXIT_MV 4280         // 退出满电显示的电压，毫伏；须低于满电确认电压
#define BOARD_BATTERY_STEP_PERCENT 1            // 电量显示步长
#define BOARD_BATTERY_LINEAR 1                  // 线性电压换算

#define BOARD_SUPPORTS_PAGE_WIPE 0  // 推白翻页能力

#define BOARD_REFRESH_PROFILE_MASK 1                       // 刷新档位位图：bit0清晰、bit1均衡、bit2快速
#define BOARD_REFRESH_PROFILE_DEFAULT 0                    // 默认刷新档位，保持持久化枚举值
#define BOARD_DISPLAY_PARTIALS_PER_FULL 15                 // 连续局刷达到此次数后允许周期全刷
#define BOARD_DISPLAY_FULL_REFRESH_MIN_INTERVAL_MS 0       // 周期全刷最短间隔，毫秒；0 表示不限制
#define BOARD_HAS_VCOM 0                                   // 支持读取和校准面板VCOM
#define BOARD_UI_FONT_SIZE 25                              // 本机系统界面字号
#define BOARD_UI_LARGE_FONT_SIZE 25                        // 大号内容及数值字号
#define BOARD_UI_EMPHASIS_NATIVE_BPP 0                     // 强调字体资源精度，0 表示直接合成加粗
#define BOARD_UI_EMPHASIS_EXPAND_PX 1                      // 无匹配强调资源时横向加粗宽度，0 关闭，支持 1、2px
#define BOARD_UI_CAPTION_FONT_SIZE 25                      // 辅助说明及紧凑区域字号
#define BOARD_READER_FONT_SIZE 25                          // 默认阅读字号
#define BOARD_READER_FONT_MIN 20 // 阅读矢量字号下限，像素
#define BOARD_READER_FONT_MAX 40                           // 阅读矢量字号上限
#define BOARD_READER_FONT_FILE "misans_25_2.ef"            // 默认阅读位图字体
#define BOARD_CHARGE_CURRENT_OPTIONS_MA {500, 1000, 1500}  // 产品允许的充电电流档位
#define BOARD_CHARGE_CURRENT_DEFAULT_MA 1000               // 默认充电电流，毫安

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
#define BOARD_THEME_DARK_DEFAULT 0                    // 默认深色模式开关
#define BOARD_HOME_STYLE_DEFAULT 2                    // 默认首页样式：0白底、1灰底、2描边、3斜线、4像素
#define BOARD_IDLE_TO_STANDBY_SECONDS (5 * 60) // 无操作进入浅睡待机的秒数
#define BOARD_NET_GRACE_OPTIONS_SECONDS {30, 60, 120, 0} // 空闲保网档位，秒；0表示永不因空闲断网
#define BOARD_NET_GRACE_SECONDS 30                    // 默认空闲保网时长，秒；0表示永不自动断网
#define BOARD_STANDBY_OFF_OPTIONS_SECONDS {1800, 3600, 10800, 86400} // 待机累计自动关机档位，秒；各项须大于0
#define BOARD_STANDBY_OFF_SECONDS 1800                // 默认待机累计自动关机时长，秒；须在档位内
#define BOARD_AGING_RESTART_SECONDS 300               // 老化测试定时重启间隔，秒

#if CONFIG_PRODUCT_GLOBAL
#define BOARD_DEFAULT_LANGUAGE "en-US"  // 未保存语言偏好时的默认语言
#define BOARD_CHAT_PROVIDER_DEFAULT 2   // 默认对话云服务：0百问、1小智、2网易
#else
#define BOARD_DEFAULT_LANGUAGE "zh-CN"  // 未保存语言偏好时的默认语言
#define BOARD_CHAT_PROVIDER_DEFAULT 0   // 默认对话云服务：0百问、1小智、2网易
#endif

#define BOARD_USB_MANUFACTURER "Metalio"  // USB 设备制造商标识
#define BOARD_AP_SSID_PREFIX "Metalio"    // 文件传输热点名称前缀

#define BOARD_WIFI_MAX_TX_POWER 0  // Wi-Fi 发射功率上限，0 不覆盖各模式原有配置，单位 0.25dBm

#endif  // METALIO_E_INK_4_CONFIG_H
