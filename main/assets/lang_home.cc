#include "assets/lang_config.h"

#include "settings.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "Lang"

// X(name, zh, en)
#define LANG_STRING_LIST(X)                                                                                    \
    X(HOME_APP_TASK, "每日清单", "Tasks")                                                                      \
    X(HOME_APP_ASSISTANT, "百问AI", "Ask AI")                                                                  \
    X(HOME_APP_BOOK, "阅读", "Books")                                                                          \
    X(HOME_APP_WALLPAPER, "壁纸", "Walls")                                                                     \
    X(HOME_APP_CLOUD, "传输", "Transfer")                                                                      \
    X(HOME_APP_SETTINGS, "设置", "Settings")                                                                   \
    X(HOME_DATE_PLACEHOLDER, "--月--日", "--/--")                                                              \
    X(HOME_DATE_FMT, "%02d月%02d日", "%02d/%02d")                                                              \
    X(HOME_DATE_SLASH_PLACEHOLDER, "----.-.--", "----.-.--")                                                   \
    X(HOME_DATE_SLASH_FMT, "%d.%d.%d", "%d.%d.%d")                                                             \
    X(HOME_WDAY_SUN, "周日", "Sun")                                                                            \
    X(HOME_WDAY_MON, "周一", "Mon")                                                                            \
    X(HOME_WDAY_TUE, "周二", "Tue")                                                                            \
    X(HOME_WDAY_WED, "周三", "Wed")                                                                            \
    X(HOME_WDAY_THU, "周四", "Thu")                                                                            \
    X(HOME_WDAY_FRI, "周五", "Fri")                                                                            \
    X(HOME_WDAY_SAT, "周六", "Sat")                                                                            \
    X(BATTERY_NEED_CHARGE, "电量不足，请充电", "Low battery, please charge")                                   \
    X(COMMON_UNKNOWN, "未知", "Unknown")                                                                       \
    X(POWERED_OFF, "已关机", "Powered off")                                                                    \
    X(SETTINGS_TAB_NETWORK, "网络", "Network")                                                                 \
    X(SETTINGS_TAB_THEME, "主题", "Theme")                                                                     \
    X(SETTINGS_TAB_LANGUAGE, "语言", "Language")                                                               \
    X(SETTINGS_TAB_HAPTIC, "震动", "Haptic")                                                                   \
    X(SETTINGS_TAB_POWER, "功耗", "Power")                                                                     \
    X(SETTINGS_TAB_FRONTLIGHT, "前光", "Light")                                                                \
    X(SETTINGS_TAB_CONVERSATION, "对话", "Talk")                                                               \
    X(SETTINGS_TAB_STORAGE, "存储", "Storage")                                                                 \
    X(SETTINGS_TAB_BLUETOOTH, "蓝牙", "Bluetooth")                                                             \
    X(SETTINGS_TAB_TEST, "测试", "Test")                                                                       \
    X(SETTINGS_TAB_ABOUT, "关于", "About")                                                                     \
    X(SETTINGS_THEME_TITLE, "首页主题", "Home theme")                                                          \
    X(SETTINGS_THEME_HINT, "返回首页后立即生效", "Applies when you return home")                               \
    X(SETTINGS_THEME_WHITE, "白底", "White")                                                                   \
    X(SETTINGS_THEME_GRAY, "网点灰底", "Dotted gray")                                                          \
    X(SETTINGS_THEME_BORDER, "黑色描边", "Black border")                                                       \
    X(SETTINGS_THEME_SLASH, "斜切棋盘", "Slash grid")                                                          \
    X(SETTINGS_THEME_CURRENT_WHITE, "当前：白底", "Current: White")                                            \
    X(SETTINGS_THEME_CURRENT_GRAY, "当前：网点灰底", "Current: Dotted gray")                                   \
    X(SETTINGS_THEME_CURRENT_BORDER, "当前：黑色描边", "Current: Black border")                                \
    X(SETTINGS_THEME_CURRENT_SLASH, "当前：斜切棋盘", "Current: Slash grid")                                   \
    X(SETTINGS_LANG_TITLE, "界面语言", "Language")                                                             \
    X(SETTINGS_LANG_HINT, "切换后立即生效", "Takes effect immediately")                                        \
    X(SETTINGS_LANG_ZH_CN, "简体中文", "简体中文")                                                             \
    X(SETTINGS_LANG_EN_US, "English", "English")                                                               \
    X(SETTINGS_LANG_CURRENT_ZH, "当前：简体中文", "Current: 简体中文")                                         \
    X(SETTINGS_LANG_CURRENT_EN, "当前：English", "Current: English")                                           \
    X(SETTINGS_HAPTIC_TITLE, "按键震动", "Button haptic")                                                      \
    X(SETTINGS_HAPTIC_HINT, "界面按钮短震反馈", "UI buttons and cover keys")                                   \
    X(SETTINGS_HAPTIC_ON, "开启", "On")                                                                        \
    X(SETTINGS_HAPTIC_OFF, "关闭", "Off")                                                                      \
    X(SETTINGS_HAPTIC_CURRENT_ON, "当前：开启", "Current: On")                                                 \
    X(SETTINGS_HAPTIC_CURRENT_OFF, "当前：关闭", "Current: Off")                                               \
    X(SETTINGS_POWER_3_MIN, "3 分", "3 min")                                                                   \
    X(SETTINGS_POWER_10_MIN, "10 分", "10 min")                                                                \
    X(SETTINGS_POWER_30_MIN, "30 分", "30 min")                                                                \
    X(SETTINGS_POWER_3_HOUR, "3 时", "3 h")                                                                    \
    X(SETTINGS_POWER_6_HOUR, "6 时", "6 h")                                                                    \
    X(SETTINGS_POWER_30_SEC, "30 秒", "30 s")                                                                  \
    X(SETTINGS_POWER_60_SEC, "60 秒", "60 s")                                                                  \
    X(SETTINGS_POWER_120_SEC, "120 秒", "120 s")                                                               \
    X(SETTINGS_POWER_OFF_NEVER, "永不", "Never")                                                               \
    X(SETTINGS_POWER_IDLE_MHZ_TITLE, "空闲降频（默认320Mhz）", "Idle CPU (def 320 MHz)")                       \
    X(SETTINGS_POWER_IDLE_MHZ_HINT, "无操作空闲时的 CPU 频率", "CPU clock when idle")                            \
    X(SETTINGS_POWER_NET_GRACE_TITLE, "保网时长（默认 30 秒）", "Keep-alive (def 30 s)")                        \
    X(SETTINGS_POWER_NET_GRACE_HINT, "无保网需求后，仍保持联网的时长", "How long to stay online after last need") \
    X(SETTINGS_POWER_STANDBY_TITLE, "浅睡待机（默认 3 分钟）", "Light sleep (def 3 min)")                       \
    X(SETTINGS_POWER_STANDBY_HINT, "无操作多久进入浅睡待机", "Idle time before light sleep")                     \
    X(SETTINGS_POWER_OFF_TITLE, "自动关机（默认 30 分钟）", "Auto power-off (def 30 min)")                      \
    X(SETTINGS_POWER_OFF_HINT, "浅睡累计多久自动关机", "Light-sleep time before power-off")                      \
    X(SETTINGS_CONV_TITLE, "语音播报", "Voice playback")                                                       \
    X(SETTINGS_CONV_HINT, "控制百问AI是否播报语音\n关闭后仅显示文字", "Whether Ask AI speaks aloud\nOff = text only") \
    X(SETTINGS_CONV_TTS_ON, "开启 TTS", "TTS on")                                                              \
    X(SETTINGS_CONV_TTS_OFF, "关闭 TTS", "TTS off")                                                            \
    X(SETTINGS_CONV_CUR_ON, "当前：开启 TTS", "Current: TTS on")                                               \
    X(SETTINGS_CONV_CUR_OFF, "当前：关闭 TTS", "Current: TTS off")                                             \
    X(SETTINGS_CONV_CUR_DASH, "当前：—", "Current: —")                                                         \
    X(SETTINGS_CONV_PARSE_FAIL, "响应解析失败", "Response parse failed")                                       \
    X(SETTINGS_CONV_REQUEST_FAIL, "请求失败", "Request failed")                                                \
    X(SETTINGS_CONV_MISSING_ENABLED, "缺少 enabled", "Missing enabled")                                        \
    X(SETTINGS_CONV_NO_NETWORK, "无网络", "No network")                                                         \
    X(SETTINGS_CONV_CONN_FAIL, "创建连接失败", "Connection failed")                                           \
    X(SETTINGS_CONV_NET_NOT_READY, "网络未就绪", "Network not ready")                                          \
    X(SETTINGS_CONV_ENABLING, "正在开启…", "Enabling…")                                                        \
    X(SETTINGS_CONV_DISABLING, "正在关闭…", "Disabling…")                                                      \
    X(SETTINGS_CONV_BUSY, "忙，请稍候", "Busy, please wait")                                                   \
    X(SETTINGS_CONV_CONNECTING, "连接网络…", "Connecting…")                                                    \
    X(SETTINGS_STORAGE_TITLE, "内存卡", "Storage")                                                              \
    X(SETTINGS_STORAGE_PC_BUSY, "内存卡：电脑占用中", "SD: in use by PC")                                        \
    X(SETTINGS_STORAGE_INSERTED, "内存卡：已插入", "SD: inserted")                                               \
    X(SETTINGS_STORAGE_MISSING, "内存卡：未检测到", "SD: not detected")                                          \
    X(SETTINGS_STORAGE_CAP_DASH, "容量：—", "Capacity: —")                                                       \
    X(SETTINGS_STORAGE_CAP_EXPORT, "容量：导出中，停用后可查看", "Capacity: exporting; disable to view")         \
    X(SETTINGS_STORAGE_CAP_FMT, "剩余 %s / 总容量 %s", "%s free / %s total")                                     \
    X(SETTINGS_STORAGE_CAP_FAIL, "容量：读取失败", "Capacity: read failed")                                      \
    X(SETTINGS_STORAGE_USB_OFF, "停用模拟 U 盘", "Disable USB disk")                                             \
    X(SETTINGS_STORAGE_USB_ON, "启用模拟 U 盘", "Enable USB disk")                                               \
    X(SETTINGS_STORAGE_USB_DISABLED, "当前固件未启用模拟 U 盘", "USB disk not enabled in this firmware")         \
    X(SETTINGS_STORAGE_USB_HINT_IDLE, "启用后电脑可将本机识别为 U 盘", "When enabled, PC sees this device as a USB disk") \
    X(SETTINGS_STORAGE_USB_HINT_SWITCHING, "正在切换，请稍候…", "Switching, please wait…")                       \
    X(SETTINGS_STORAGE_USB_HINT_ENABLING, "正在启用模拟 U 盘…", "Enabling USB disk…")                            \
    X(SETTINGS_STORAGE_USB_HINT_DISABLING, "正在停用模拟 U 盘…", "Disabling USB disk…")                          \
    X(SETTINGS_STORAGE_USB_HINT_HOST, "已启用，请在电脑访问；停用前请先弹出", "Enabled — access from PC; eject before disable") \
    X(SETTINGS_STORAGE_USB_HINT_LOCAL, "已启用，主机未占用；点停用恢复 USB 调试", "Enabled — host idle; disable to restore USB debug") \
    X(SETTINGS_STORAGE_USB_HINT_DISABLED, "启用后占用 USB；停用后恢复 USB 调试口", "Uses USB when on; disable restores debug port") \
    X(SETTINGS_STORAGE_USB_HINT_ENABLE_FAIL, "启用失败，请检查 USB 线并重试", "Enable failed — check USB cable and retry") \
    X(SETTINGS_STORAGE_USB_HINT_DISABLE_FAIL, "停用失败，请先在电脑上弹出 U 盘", "Disable failed — eject the disk on PC first") \
    X(SETTINGS_STORAGE_USB_HINT_NO_SD, "未检测到内存卡", "No SD card detected")                                  \
    X(SETTINGS_STORAGE_USB_HINT_FORMAT, "内存卡需要格式化 (FAT32)", "SD card needs formatting (FAT32)")           \
    X(SETTINGS_STORAGE_USB_HINT_HOST_BUSY, "操作失败，请先在电脑上弹出 U 盘", "Failed — eject the disk on PC first") \
    X(BT_TITLE, "蓝牙", "Bluetooth")                                                                           \
    X(BT_DESC, "外置蓝牙音频解码芯片设置（非 ESP32 内置蓝牙）", "External BT audio chip settings (not ESP32 BLE)") \
    X(BT_RESET_HINT, "烧录蓝牙固件时使用", "Use when flashing BT firmware")                                     \
    X(BT_RESET_BTN, "复位蓝牙", "Reset BT")                                                                    \
    X(BT_MODE1, "模式1", "Mode 1")                                                                             \
    X(BT_MODE2, "模式2", "Mode 2")                                                                             \
    X(BT_MODE3, "模式3", "Mode 3")                                                                             \
    X(BT_SELECT_MODE, "请选择蓝牙模式", "Select a Bluetooth mode")                                              \
    X(BT_MODE1_ACTIVE, "模式1 已激活\n(AT+RX=2 / AT+MODE=1)", "Mode 1 active\n(AT+RX=2 / AT+MODE=1)")             \
    X(BT_SCAN_BTN, "扫描设备", "Scan")                                                                         \
    X(BT_MUSIC_BTN, "音乐模式", "Music")                                                                       \
    X(BT_CALL_BTN, "通话模式", "Call")                                                                         \
    X(BT_CONNECTING_FMT, "连接中: %s...", "Connecting: %s...")                                                  \
    X(BT_MODE1_SET, "模式1 已设置", "Mode 1 set")                                                               \
    X(BT_MODE2_SET, "模式2 已设置，可扫描设备", "Mode 2 set, ready to scan")                                     \
    X(BT_MODE3_SET, "模式3 已设置", "Mode 3 set")                                                               \
    X(BT_SCANNING, "正在扫描...", "Scanning...")                                                               \
    X(BT_FOUND_FMT, "发现设备: %s", "Found: %s")                                                                \
    X(BT_SCAN_DONE_FMT, "扫描完成，共 %d 个设备", "Scan done, %d device(s)")                                     \
    X(BT_CONNECTING, "正在连接...", "Connecting...")                                                           \
    X(BT_CONNECT_OK, "连接成功", "Connected")                                                                  \
    X(BT_CONNECT_TIMEOUT, "连接失败 (超时)", "Connect failed (timeout)")                                        \
    X(BT_CALL_MODE_SCO, "通话模式 (SCO 已建立)", "Call mode (SCO up)")                                          \
    X(BT_MUSIC_MODE_SCO, "音乐模式 (SCO 已断开)", "Music mode (SCO down)")                                      \
    X(BT_SWITCH_MODE1, "切换模式1...", "Switching to mode 1...")                                               \
    X(BT_SWITCH_MODE2, "切换模式2...", "Switching to mode 2...")                                               \
    X(BT_SWITCH_MODE3, "切换模式3...", "Switching to mode 3...")                                               \
    X(BT_UART_NOT_INIT, "UART 未初始化", "UART not initialized")                                               \
    X(BT_SWITCH_CALL, "切换通话模式...", "Switching to call mode...")                                          \
    X(BT_SWITCH_MUSIC, "切换音乐模式...", "Switching to music mode...")                                        \
    X(BT_PWR_RESET_UNSUP, "当前硬件不支持蓝牙电源复位", "BT power reset not supported")                          \
    X(BT_PWR_RESET_OK, "蓝牙电源已复位", "BT power reset")                                                      \
    X(BT_NEED_MODE2, "请先切换到模式2", "Switch to mode 2 first")                                               \
    X(BT_SCAN_START, "开始扫描...", "Starting scan...")                                                        \
    X(BT_NEED_CONNECT, "请先连接蓝牙设备", "Connect a Bluetooth device first")                                  \
    X(SETTINGS_FRONTLIGHT_TITLE, "前光", "Front light")                                                        \
    X(SETTINGS_FRONTLIGHT_HINT, "色温与亮度 0–100", "CCT and brightness 0–100")                                \
    X(SETTINGS_FRONTLIGHT_COOL, "冷光", "Cool")                                                                \
    X(SETTINGS_FRONTLIGHT_WARM, "暖光", "Warm")                                                                \
    X(SETTINGS_FRONTLIGHT_BOTH, "冷暖光", "Cool + warm")                                                       \
    X(SETTINGS_FRONTLIGHT_OFF, "关闭", "Off")                                                                  \
    X(SETTINGS_FRONTLIGHT_CURRENT_COOL, "当前：冷光", "Current: Cool")                                         \
    X(SETTINGS_FRONTLIGHT_CURRENT_WARM, "当前：暖光", "Current: Warm")                                         \
    X(SETTINGS_FRONTLIGHT_CURRENT_BOTH, "当前：冷暖光", "Current: Cool + warm")                                \
    X(SETTINGS_FRONTLIGHT_CURRENT_OFF, "当前：关闭", "Current: Off")                                           \
    X(SETTINGS_FRONTLIGHT_BRIGHTNESS_FMT, "亮度：%d", "Brightness: %d")                                        \
    X(SETTINGS_FRONTLIGHT_BRIGHTNESS_DEC, "-", "-")                                                            \
    X(SETTINGS_FRONTLIGHT_BRIGHTNESS_INC, "+", "+")                                                            \
    X(SETTINGS_ABOUT_MODEL, "设备型号", "Device model")                                                        \
    X(SETTINGS_ABOUT_CHIP, "芯片型号", "Chip")                                                                 \
    X(SETTINGS_ABOUT_CORES, "CPU 核心", "CPU cores")                                                           \
    X(SETTINGS_ABOUT_CORES_FMT, "%u 核", "%u cores")                                                           \
    X(SETTINGS_ABOUT_FW, "固件版本", "Firmware")                                                               \
    X(SETTINGS_ABOUT_FW_VER_FMT, "固件版本:%s", "Firmware:%s")                                                 \
    X(SETTINGS_ABOUT_BUILD, "编译时间", "Build time")                                                          \
    X(SETTINGS_ABOUT_MAC, "MAC 地址", "MAC")                                                                   \
    X(SETTINGS_ABOUT_FLASH, "Flash 容量", "Flash")                                                             \
    X(SETTINGS_ABOUT_PSRAM, "PSRAM 总大小", "PSRAM")                                                           \
    X(SETTINGS_ABOUT_NONE, "无", "None")                                                                       \
    X(SETTINGS_TEST_AUTO, "自动测试", "Auto test")                                                             \
    X(SETTINGS_TEST_TOUCH, "触摸测试", "Touch test")                                                           \
    X(SETTINGS_TEST_BATTERY, "电池测试", "Battery test")                                                       \
    X(SETTINGS_TEST_AGING, "老化测试", "Aging test")                                                           \
    X(SETTINGS_TEST_SIGNAL_SKIP, "网络信号：本轮未接 WiFi/4G", "Signal: WiFi/4G not wired this round")          \
    X(SETTINGS_TEST_OK, "正常", "OK")                                                                          \
    X(SETTINGS_TEST_FAIL, "失败", "Failed")                                                                    \
    X(SETTINGS_TEST_SKIP, "跳过", "Skip")                                                                      \
    X(SETTINGS_TEST_NOT_DETECTED, "未检测到", "Not found")                                                     \
    X(SETTINGS_TEST_READ_FAIL, "读取失败", "Read failed")                                                      \
    X(SETTINGS_TEST_SDCARD, "SD卡", "SD card")                                                                 \
    X(SETTINGS_TEST_SD_MOUNT_FAIL, "未插卡/挂载失败", "No card / mount fail")                                  \
    X(SETTINGS_TEST_AUDIO_TITLE, "音频", "Audio")                                                              \
    X(SETTINGS_TEST_AUDIO_HOLD, "按住说话", "Hold to talk")                                                    \
    X(SETTINGS_TEST_AUDIO_RECORDING, "录音中…", "Recording…")                                                  \
    X(SETTINGS_TEST_AUDIO_CONFIRM, "录音、喇叭是否正常？", "Mic and speaker OK?")                               \
    X(SETTINGS_TEST_YES, "是", "Yes")                                                                          \
    X(SETTINGS_TEST_NO, "否", "No")                                                                            \
    X(RECORD_PLAYING, "播放中…", "Playing…")                                                                   \
    X(SETTINGS_TEST_CAMERA, "摄像头", "Camera")                                                                \
    X(SETTINGS_TEST_BACK, "返回", "Back")                                                                      \
    X(SETTINGS_TEST_TOUCH_START, "点击屏幕，开始测试", "Tap screen to start")                                  \
    X(SETTINGS_TEST_BATTERY_HINT, "SOC: 3.30~4.33V; ≥4.33需稳30s→100%",                                         \
      "SOC: 3.30~4.33V; ≥4.33 hold 30s→100%")                                                                   \
    X(SETTINGS_TEST_BATTERY_GAUGE, "电量计", "Fuel gauge")                                                     \
    X(SETTINGS_TEST_BATTERY_SOC_SMOOTH, "电量(平滑)", "SOC (smooth)")                                          \
    X(SETTINGS_TEST_BATTERY_SOC_RAW, "电量(瞬时)", "SOC (instant)")                                            \
    X(SETTINGS_TEST_BATTERY_VOLT, "电压", "Voltage")                                                           \
    X(SETTINGS_TEST_BATTERY_VOLT_RANGE, "电压波动", "Voltage swing")                                           \
    X(SETTINGS_TEST_BATTERY_CURR, "电流", "Current")                                                           \
    X(SETTINGS_TEST_BATTERY_GAUGE_STAT, "表计状态", "Gauge state")                                             \
    X(SETTINGS_TEST_BATTERY_CHARGING, "充电中", "Charging")                                                    \
    X(SETTINGS_TEST_BATTERY_DISCHARGING, "放电中", "Discharging")                                              \
    X(SETTINGS_TEST_BATTERY_IDLE_LOAD, "空载", "Idle")                                                         \
    X(SETTINGS_TEST_BATTERY_VBUS, "VBUS", "VBUS")                                                              \
    X(SETTINGS_TEST_BATTERY_CHIP_CHG, "芯片充电", "Chip charge")                                               \
    X(SETTINGS_TEST_BATTERY_CHG_EN, "充电使能", "Charge enable")                                               \
    X(SETTINGS_TEST_BATTERY_ICHG, "设定电流", "Charge current")                                                \
    X(SETTINGS_TEST_BATTERY_VREG, "截止电压", "Charge voltage")                                                \
    X(SETTINGS_TEST_BATTERY_MATCH, "一致性", "Consistency")                                                    \
    X(SETTINGS_TEST_BATTERY_CHG_NOT, "未充电/已满", "Not charging / full")                                     \
    X(SETTINGS_TEST_BATTERY_CHG_CC, "涓流/预充/CC", "Trickle/pre/CC")                                          \
    X(SETTINGS_TEST_BATTERY_CHG_CV, "恒压降流", "CV taper")                                                    \
    X(SETTINGS_TEST_BATTERY_CHG_TOPOFF, "Top-off", "Top-off")                                                  \
    X(SETTINGS_TEST_BATTERY_EN_ON, "开", "On")                                                                 \
    X(SETTINGS_TEST_BATTERY_EN_OFF, "关", "Off")                                                               \
    X(SETTINGS_TEST_BATTERY_VBUS_NONE, "无输入", "No input")                                                   \
    X(SETTINGS_TEST_BATTERY_VBUS_SDP, "USB SDP", "USB SDP")                                                    \
    X(SETTINGS_TEST_BATTERY_VBUS_CDP, "USB CDP", "USB CDP")                                                    \
    X(SETTINGS_TEST_BATTERY_VBUS_DCP, "USB DCP", "USB DCP")                                                    \
    X(SETTINGS_TEST_BATTERY_VBUS_UNK_ADP, "未知适配器", "Unknown adapter")                                     \
    X(SETTINGS_TEST_BATTERY_VBUS_NONSTD, "非标适配器", "Non-std adapter")                                      \
    X(SETTINGS_TEST_BATTERY_VBUS_OTG, "OTG", "OTG")                                                            \
    X(SETTINGS_TEST_BATTERY_VBUS_ADP, "适配器", "Adapter")                                                     \
    X(SETTINGS_TEST_BATTERY_VBUS_UNKNOWN_IN, "有输入(未识别)", "Input (unknown)")                              \
    X(SETTINGS_TEST_BATTERY_MATCH_OK, "一致", "Match")                                                         \
    X(SETTINGS_TEST_BATTERY_MATCH_DPDM, "一致(DPDM关)", "Match (DPDM off)")                                    \
    X(SETTINGS_TEST_BATTERY_MISMATCH_CHIP, "芯片在充/表计否", "Chip chg / gauge no")                           \
    X(SETTINGS_TEST_BATTERY_MISMATCH_GAUGE, "表计在充/芯片否", "Gauge chg / chip no")                          \
    X(SETTINGS_TEST_BATTERY_MISMATCH_NO_CHG, "有输入未在充", "Input, not charging")                            \
    X(SETTINGS_TEST_BATTERY_MISMATCH_NO_IN, "无输入却在充?", "No input but charging?")                         \
    X(SETTINGS_TEST_AGING_RUNNING, "老化测试中（震动循环）", "Aging in progress")                              \
    X(SETTINGS_TEST_AGING_TICK_FMT, "运行 %u s  震=%d", "Running %u s  vibe=%d")                               \
    X(STANDBY, "待命", "Standby")                                                                              \
    X(CONNECTING, "连接中...", "Connecting...")                                                                \
    X(LISTENING, "聆听中...", "Listening...")                                                                  \
    X(SPEAKING, "回答中...", "Speaking...")                                                                    \
    X(PLEASE_WAIT, "请稍候...", "Please wait...")                                                              \
    X(LOADING_PROTOCOL, "登录服务...", "Logging in...")                                                        \
    X(VOICE_STARTING_NET, "正在联网启动，完成后自动开始", "Connecting; will start automatically")           \
    X(SERVER_NOT_CONNECTED, "无法连接服务，请稍后再试", "Unable to connect to service")                      \
    X(WIFI_CONFIG_MODE, "配网模式", "Wi-Fi Setup")                                                             \
    X(NEED_WIFI_CFG, "请先完成配网", "Set up Wi-Fi first")                                                     \
    X(VERSION, "版本 ", "Ver ")                                                                                \
    X(FOUND_NEW_ASSETS, "发现新资源: %s", "Found new assets: %s")                                              \
    X(LOADING_ASSETS, "加载资源...", "Loading assets...")                                                      \
    X(ERROR, "错误", "Error")                                                                                  \
    X(DOWNLOAD_ASSETS_FAILED, "下载资源失败", "Failed to download assets")                                     \
    X(CHECKING_NEW_VERSION, "检查新版本...", "Checking for new version...")                                    \
    X(CHECK_NEW_VERSION_FAILED, "检查新版本失败，将在 %d 秒后重试: %s", "Check version failed, retry in %d s: %s") \
    X(ACTIVATION, "激活设备", "Activation")                                                                    \
    X(ACTIVATION_CODE_FMT, "验证码:%s", "Code:%s")                                                             \
    X(OTA_UPGRADE, "OTA 升级", "OTA Upgrade")                                                                  \
    X(UPGRADING, "正在升级系统...", "System is upgrading...")                                                  \
    X(UPGRADE_FAILED, "升级失败", "Upgrade failed")                                                            \
    X(OTA_SUCCESS_REBOOT, "升级成功，即将重启", "Upgrade OK, rebooting")                                       \
    X(OTA_MANUAL, "手动升级", "Manual upgrade")                                                                \
    X(RTC_MODE_OFF, "AEC 关闭", "AEC Off")                                                                     \
    X(RTC_MODE_ON, "AEC 开启", "AEC On")                                                                       \
    X(SERVER_ERROR, "服务错误", "Server error")                                                                \
    X(SERVER_TIMEOUT, "服务超时", "Server timeout")                                                            \
    X(OTA_TITLE, "系统升级", "System upgrade")                                                                 \
    X(OTA_CUR_VER_FMT, "当前版本: %s", "Current: %s")                                                          \
    X(OTA_NEW_VER_FMT, "新版本: %s", "New: %s")                                                                \
    X(OTA_HINT, "升级过程请勿断电", "Do not power off during upgrade")                                         \
    X(OTA_CONFIRM_FMT, "发现新版本 %s\n是否升级？", "New version %s\nUpgrade?")                                 \
    X(OTA_SD_CONFIRM_FMT, "SD 固件升级\n当前 %s\n是否升级？", "SD firmware\nCurrent %s\nUpgrade?")              \
    X(OTA_UPGRADE_NOW, "立即升级", "Upgrade now")                                                              \
    X(OTA_IGNORE_VERSION, "忽略此版本", "Ignore version")                                                      \
    X(OTA_REMIND_LATER, "稍后提醒", "Remind later")                                                            \
    X(COMMON_CANCEL, "取消", "Cancel")                                                                         \
    X(COMMON_REMOVE, "移除", "Remove")                                                                         \
    X(COMMON_SELECT_ALL, "全选", "Select all")                                                                 \
    X(TASK_API_ERROR, "接口返回错误", "API error")                                                             \
    X(TASK_BATCH_COMPLETING, "批量完成中...", "Batch completing...")                                           \
    X(TASK_BATCH_FAIL, "批量操作失败", "Batch failed")                                                         \
    X(TASK_BATCH_REMOVING, "批量移除中...", "Batch removing...")                                               \
    X(TASK_BATCH_RESULT_FMT, "成功 %d，失败 %d", "%d ok, %d failed")                                            \
    X(TASK_BATCH_START_FAIL, "批量任务启动失败", "Failed to start batch")                                      \
    X(TASK_CHOOSE_ACTION, "选择操作", "Choose action")                                                         \
    X(TASK_COMPLETE, "完成", "Done")                                                                           \
    X(TASK_COMPLETED, "已完成", "Completed")                                                                   \
    X(TASK_COMPLETED_N_FMT, "已完成 %d 项", "Completed %d")                                                    \
    X(TASK_COMPLETE_FAIL, "完成失败", "Complete failed")                                                       \
    X(TASK_COMPLETE_START_FAIL, "完成任务启动失败", "Failed to start complete")                                \
    X(TASK_COMPLETE_TODO, "完成待办", "Complete")                                                              \
    X(TASK_COMPLETING, "完成中...", "Completing...")                                                           \
    X(TASK_CONNECTING_NET, "正在联网…", "Connecting…")                                                         \
    X(TASK_CONN_FAIL, "创建连接失败", "Connection failed")                                                     \
    X(TASK_DATA_FORMAT_ERR, "数据格式错误", "Bad data format")                                                 \
    X(TASK_DELETED, "已删除", "Deleted")                                                                       \
    X(TASK_DELETE_FAIL, "删除失败", "Delete failed")                                                           \
    X(TASK_DELETE_START_FAIL, "删除任务启动失败", "Failed to start delete")                                    \
    X(TASK_DELETE_TODO, "删除待办", "Delete")                                                                  \
    X(TASK_DELETING, "删除中...", "Deleting...")                                                               \
    X(TASK_DONE_OK, "任务已完成", "Task completed")                                                            \
    X(TASK_EMPTY_DONE, "暂无已完成任务", "No completed tasks")                                                 \
    X(TASK_EMPTY_TODO, "暂无待办", "No todos")                                                                 \
    X(TASK_INVALID, "无效待办", "Invalid todo")                                                                \
    X(TASK_LOAD_FAIL, "加载失败", "Load failed")                                                               \
    X(TASK_NEED_WIFI_CFG, "请先完成配网", "Set up Wi-Fi first")                                                 \
    X(TASK_NET_NOT_READY, "网络未就绪", "Network not ready")                                                   \
    X(TASK_NO_COMPLETABLE, "没有可完成的待办", "Nothing to complete")                                          \
    X(TASK_NO_NETWORK, "无网络", "No network")                                                                 \
    X(TASK_PARSE_FAIL, "响应解析失败", "Response parse failed")                                                \
    X(TASK_PLEASE_WAIT, "请稍候...", "Please wait...")                                                         \
    X(TASK_REFRESH, "刷新", "Refresh")                                                                         \
    X(TASK_REFRESHED, "已刷新", "Refreshed")                                                                   \
    X(TASK_REFRESHING, "刷新中...", "Refreshing...")                                                           \
    X(TASK_REFRESH_START_FAIL, "刷新启动失败", "Failed to start refresh")                                      \
    X(TASK_REMOVED_N_FMT, "已移除 %d 项", "Removed %d")                                                        \
    X(TASK_REQUEST_FAIL, "请求失败", "Request failed")                                                         \
    X(TASK_SELECTED_FMT, "已选 %d", "Selected %d")                                                             \
    X(TASK_SELECT_FIRST, "请先选择", "Select items first")                                                     \
    X(TASK_TAB_DONE, "已完成", "Done")                                                                         \
    X(TASK_TAB_TODO, "待办", "Todo")                                                                           \
    X(TASK_TITLE_DONE, "已完成", "Done")                                                                       \
    X(TASK_TITLE_TODO, "日程待办", "Schedule")                                                                 \
    X(CONNECT_TO, "连接 ", "Connect ")                                                                         \
    X(CONNECTED_TO, "已连接 ", "Connected ")                                                                   \
    X(SETTINGS_NET_WIFI_CFG_TITLE, "WiFi 网络", "Wi-Fi network")                                               \
    X(SETTINGS_NET_WIFI_CFG_HINT, "扫描附近 WiFi，选择后输入密码", "Scan nearby Wi-Fi and enter the password") \
    X(SETTINGS_NET_ENTER_CFG, "配置 WIFI 网络", "Configure Wi-Fi")                                             \
    X(SETTINGS_NET_CUR_WIFI, "当前：WiFi 上网", "Current: Wi-Fi")                                               \
    X(SETTINGS_NET_REBOOT_COUNT_FMT, "%s\n%d 秒后重启…", "%s\nRebooting in %d s…")                             \
    X(SETTINGS_NET_REBOOTING, "正在重启…", "Rebooting…")                                                       \
    X(NETWORK_TITLE, "配置 WIFI", "Configure Wi-Fi")                                                           \
    X(NETWORK_TAB_NEARBY, "附近", "Nearby")                                                                    \
    X(NETWORK_TAB_SAVED, "已保存", "Saved")                                                                    \
    X(NETWORK_SCAN, "刷新", "Refresh")                                                                         \
    X(NETWORK_SCANNING, "正在扫描附近 WiFi…", "Scanning nearby Wi-Fi…")                                        \
    X(NETWORK_WIFI_INIT, "正在初始化 WiFi…", "Initializing Wi-Fi…")                                            \
    X(NETWORK_WIFI_INIT_FAIL, "WiFi 初始化失败", "Wi-Fi init failed")                                          \
    X(NETWORK_SCAN_FAIL, "启动扫描失败", "Failed to start scan")                                               \
    X(NETWORK_SCAN_TIMEOUT, "扫描超时", "Scan timed out")                                                      \
    X(NETWORK_SCAN_DONE_FMT, "扫描完成，共 %d 个网络", "Scan done, %d networks")                               \
    X(NETWORK_SCAN_TASK_FAIL, "无法启动扫描任务", "Cannot start scan task")                                    \
    X(NETWORK_BUSY_CONNECT, "正在连接，请稍后再扫描", "Connecting; scan later")                                \
    X(NETWORK_NEARBY_EMPTY, "未发现网络，点「刷新」重试", "No networks. Tap Refresh to retry")                  \
    X(NETWORK_SAVED_EMPTY, "暂无已保存的 WiFi", "No saved Wi-Fi yet")                                           \
    X(NETWORK_AUTH_OPEN, "[开放]", "[Open]")                                                                   \
    X(NETWORK_AUTH_SECURE, "[加密]", "[Secured]")                                                              \
    X(NETWORK_CONNECT_TO_FMT, "连接到：%s", "Connect to: %s")                                                  \
    X(NETWORK_PWD_HINT, "请输入 WiFi 密码（8~63 字符）", "Enter Wi-Fi password (8–63 chars)")                   \
    X(NETWORK_PWD_PLACEHOLDER, "WiFi 密码", "Wi-Fi password")                                                  \
    X(NETWORK_SHOW_PWD, "显示密码", "Show password")                                                           \
    X(NETWORK_CANCEL, "取消", "Cancel")                                                                        \
    X(NETWORK_CONNECT, "连接", "Connect")                                                                      \
    X(NETWORK_CONNECTING_FMT, "正在连接 %s …", "Connecting to %s …")                                           \
    X(NETWORK_CONNECTED_FMT, "已连接 %s", "Connected to %s")                                                   \
    X(NETWORK_CONNECT_FAIL, "连接失败", "Connection failed")                                                   \
    X(NETWORK_CONNECT_FAIL_REFRESH, "请重新刷新 WiFi 列表", "Please refresh the Wi-Fi list")                   \
    X(NETWORK_CONNECT_TIMEOUT, "连接超时", "Connection timed out")                                             \
    X(NETWORK_CONNECT_TIMEOUT_HINT, "未能在 15 秒内完成连接，请重试", "Could not connect within 15s; retry")   \
    X(NETWORK_CONNECT_TASK_FAIL, "无法启动连接任务", "Cannot start connect task")                              \
    X(NETWORK_SSID_INVALID, "SSID 不合法", "Invalid SSID")                                                     \
    X(NETWORK_PWD_TOO_LONG, "密码超长", "Password too long")                                                   \
    X(NETWORK_ERR_BAD_PASSWORD, "密码错误，请重新输入", "Wrong password; try again")                           \
    X(NETWORK_ERR_AP_GONE, "未找到该 WiFi（信号丢失）", "Wi-Fi not found (signal lost)")                       \
    X(NETWORK_ERR_ASSOC, "关联失败，路由器拒绝连接", "Association failed; AP rejected")                        \
    X(NETWORK_ERR_WEAK, "信号太弱，连接超时", "Signal too weak; timed out")                                    \
    X(NETWORK_ERR_REASON_FMT, "连接被拒绝 (reason=%u)", "Rejected (reason=%u)")                                \
    X(NETWORK_DEFAULT_FMT, "%s（默认）", "%s (default)")                                                       \
    X(NETWORK_SET_DEFAULT, "置顶", "Default")                                                                  \
    X(NETWORK_DELETE, "删除", "Delete")                                                                        \
    X(NETWORK_CLEAR_ALL, "清空", "Clear")                                                                      \
    X(NETWORK_SET_DEFAULT_OK, "已设为默认网络", "Set as default network")                                      \
    X(NETWORK_DELETED_OK, "已删除该网络", "Network deleted")                                                   \
    X(NETWORK_CLEARED_OK, "已清空已保存网络", "Cleared saved networks")                                        \
    X(SCANNING, "扫描中...", "Scanning...")                                                                    \
    X(SCANNING_WIFI, "扫描 WiFi...", "Scanning Wi-Fi...")                                                      \
    X(WIFI_STATUS_SCANNING, "WiFi扫描中", "WiFi scanning")                                                     \
    X(WIFI_STATUS_CONNECTING, "WiFi连接中", "WiFi connecting")                                                 \
    X(VOLUME, "音量 ", "Volume ")                                                                              \
    X(MAX_VOLUME, "最大音量", "Max volume")                                                                    \
    X(MUTED, "已静音", "Muted")                                                                    \
    X(BOOK_ANTIALIAS, "抗锯齿", "Anti-aliasing") \
    X(BOOK_BAD_ARG, "参数错误", "Invalid argument") \
    X(BOOK_BLANK, "（空白）", "(Blank)") \
    X(BOOK_BODY, "正文", "Body") \
    X(BOOK_CANCELLED, "已取消", "Cancelled") \
    X(BOOK_CHAPTER_FMT, "第%d章", "Ch.%d") \
    X(BOOK_CHAPTER_NO_TEXT, "（本章无文本）", "(No text in chapter)") \
    X(BOOK_CHAPTER_PROGRESS, "底栏信息", "Footer info") \
    X(BOOK_CHECKSUM_FAIL, "校验失败", "Checksum failed") \
    X(BOOK_CONN_FAIL, "创建连接失败", "Connection failed") \
    X(BOOK_CONTINUE, "继续阅读", "Continue") \
    X(BOOK_COUNT_FMT, "%d本", "%d") \
    X(BOOK_COVER_TOO_LARGE, "封面过大", "Cover too large") \
    X(BOOK_DELETE_FAILED, "删除失败", "Delete failed") \
    X(BOOK_DETAIL, "详情", "Details") \
    X(BOOK_DOWNLOAD_FAIL, "下载失败", "Download failed") \
    X(BOOK_EMPTY_COVER, "空封面", "Empty cover") \
    X(BOOK_EMPTY_FILE, "空文件", "Empty file") \
    X(BOOK_EMPTY_HOME_FMT, "暂无书籍\n请将 .epub / .txt / .ebook\n放到 %s", "No books\nPut .epub / .txt / .ebook\nin %s") \
    X(BOOK_EMPTY_SHELF_FMT, "书架为空\n请将 .epub / .txt / .ebook\n放到 %s", "Bookshelf empty\nPut .epub / .txt / .ebook\nin %s") \
    X(BOOK_FILE_TOO_LARGE, "文件过大", "File too large") \
    X(BOOK_FINISHED, "已看完", "Finished") \
    X(BOOK_FONT_EMPTY, "无可用字体", "No fonts") \
    X(BOOK_FONT_NAME_INVALID, "文件名无效", "Invalid filename") \
    X(BOOK_FONT_TITLE, "字体", "Font") \
    X(BOOK_FOOTER_BATTERY, "电量", "Battery") \
    X(BOOK_FOOTER_PROGRESS, "进度", "Progress") \
    X(BOOK_FOOTER_TIME, "时间", "Time") \
    X(BOOK_FOOTER_TITLE, "章节名", "Chapter") \
    X(BOOK_IMAGE_PLACEHOLDER, "[图片]", "[Image]") \
    X(BOOK_LAYOUT_BUSY, "编排中", "Building") \
    X(BOOK_LAYOUT_DONE, "完成", "Done") \
    X(BOOK_LINE_GAP, "行距", "Line spacing") \
    X(BOOK_MARGIN, "边距", "Margin") \
    X(BOOK_MARGIN_NARROW, "窄", "Narrow") \
    X(BOOK_MARGIN_STANDARD, "标准", "Standard") \
    X(BOOK_MARGIN_VERY_WIDE, "很宽", "Very wide") \
    X(BOOK_MARGIN_WIDE, "宽", "Wide") \
    X(BOOK_NET_NOT_READY, "网络未就绪", "Network not ready") \
    X(BOOK_NO_CONTINUE, "暂无可续", "Nothing to continue") \
    X(BOOK_NO_COVER_URL, "无封面地址", "No cover URL") \
    X(BOOK_NO_DOWNLOAD_URL, "缺少下载地址", "Missing download URL") \
    X(BOOK_NO_LOCAL_FILE, "无本地文件", "No local file") \
    X(BOOK_NO_NETWORK, "无网络", "No network") \
    X(BOOK_NO_RECENT, "暂无最近阅读", "No recent books") \
    X(BOOK_NO_SD, "未检测到 SD 卡\n请插入后重试", "No SD card\nInsert and retry") \
    X(BOOK_NO_SD_SHORT, "未检测到 SD 卡", "No SD card") \
    X(BOOK_OPEN_FAILED, "打开失败", "Open failed") \
    X(BOOK_OPEN_FAILED_CHAPTER, "打开失败\n章节过大或内存不足", "Open failed\nChapter too large or out of memory") \
    X(BOOK_OPEN_FAILED_EPUB, "打开失败\n请检查 EPUB 完整性", "Open failed\nCheck EPUB integrity") \
    X(BOOK_OPEN_FAILED_TASK, "打开失败\n无法启动解析任务", "Open failed\nCannot start parse task") \
    X(BOOK_OPEN_FAILED_TXT, "打开失败\nTXT 过大或内存不足", "Open failed\nTXT too large or out of memory") \
    X(BOOK_OPENING, "正在打开", "Opening") \
    X(BOOK_ORIENT, "屏幕方向", "Orientation") \
    X(BOOK_ORIENT_LAND_LEFT, "左横", "Left") \
    X(BOOK_ORIENT_LAND_RIGHT, "右横", "Right") \
    X(BOOK_ORIENT_PORTRAIT, "竖屏", "Port.") \
    X(BOOK_OUT_OF_MEMORY, "内存不足", "Out of memory") \
    X(BOOK_PAGE_LOAD_FAILED, "无法加载页面", "Cannot load page") \
    X(BOOK_PATH_INVALID, "路径无效", "Invalid path") \
    X(BOOK_READ_DURATION, "阅读时长", "Total time") \
    X(BOOK_READ_FAIL, "读取失败", "Read failed") \
    X(BOOK_READ_FILE_FAIL, "（读文件失败）", "(Read failed)") \
    X(BOOK_READ_PCT_FMT, "已阅读%d.%d%%", "Read %d.%d%%") \
    X(BOOK_RECENT, "最近阅读", "Recent") \
    X(BOOK_SAVE_FAIL, "保存失败", "Save failed") \
    X(BOOK_SELECTED_FMT, "已选 %d", "Selected %d") \
    X(BOOK_SHELF_TITLE, "我的书架", "My bookshelf") \
    X(BOOK_SPACING_COMPACT, "紧凑", "Compact") \
    X(BOOK_SPACING_RELAXED, "宽松", "Relaxed") \
    X(BOOK_SPACING_STANDARD, "标准", "Standard") \
    X(BOOK_SPACING_VERY_RELAXED, "很宽松", "Very relaxed") \
    X(BOOK_START, "开始阅读", "Start reading") \
    X(BOOK_STORAGE_UNAVAIL, "存储目录不可用", "Storage unavailable") \
    X(BOOK_SYNC_EMPTY_BODY, "空请求体", "Empty request body") \
    X(BOOK_TAP_ZONE_BACK, "返回", "Back") \
    X(BOOK_TAP_ZONE_FULL_REFRESH, "全刷一次", "Full refresh") \
    X(BOOK_TAP_ZONE_MENU, "菜单", "Menu") \
    X(BOOK_TAP_ZONE_NEXT, "下一页", "Next") \
    X(BOOK_TAP_ZONE_NONE, "无", "None") \
    X(BOOK_TAP_ZONE_PREV, "上一页", "Prev") \
    X(BOOK_TAP_ZONES, "触摸分区", "Tap zones") \
    X(BOOK_TAP_ZONES_PICK, "选择动作", "Choose action") \
    X(BOOK_TAP_ZONES_RESET, "恢复默认", "Reset") \
    X(BOOK_TAP_ZONES_TITLE, "触摸分区", "Tap zones") \
    X(BOOK_TOC, "目录", "Contents") \
    X(BOOK_TOC_EMPTY, "暂无目录", "No contents") \
    X(BOOK_TOC_PAGE_FMT, "目录 %d/%d", "Contents %d/%d") \
    X(BOOK_TODAY_DURATION, "今日时长", "Today") \
    X(BOOK_UNDERLINE, "下划线", "Underline") \
    X(BOOK_UNDERLINE_DASHED, "虚线", "Dashed") \
    X(BOOK_UNDERLINE_SOLID, "实线", "Solid") \
    X(BOOK_WRITE_FAIL, "写文件失败", "Write failed") \
    X(CLOUD_API_ERROR, "接口返回错误", "API error") \
    X(CLOUD_CANCELLED, "已取消", "Cancelled") \
    X(CLOUD_CANCELLING, "正在取消…", "Cancelling…") \
    X(CLOUD_CONN_FAIL, "创建连接失败", "Connection failed") \
    X(CLOUD_CONNECT_NET, "连接网络…", "Connecting…") \
    X(CLOUD_CONNECTING_NET, "正在联网…", "Connecting…") \
    X(CLOUD_COVER_FAIL, "封面加载失败", "Cover load failed") \
    X(CLOUD_DATA_FORMAT_ERR, "数据格式错误", "Bad data format") \
    X(CLOUD_DECODE_FAIL, "解码失败", "Decode failed") \
    X(CLOUD_DELETE_FAIL, "删除失败", "Delete failed") \
    X(CLOUD_DOWNLOAD_FAIL, "下载失败", "Download failed") \
    X(CLOUD_DOWNLOADING, "正在下载", "Downloading") \
    X(CLOUD_EMPTY_ALL, "暂无待传输资源", "Nothing to transfer") \
    X(CLOUD_EMPTY_BOOK, "暂无书籍", "No books") \
    X(CLOUD_EMPTY_FONT, "暂无字体", "No fonts") \
    X(CLOUD_EMPTY_WALLPAPER, "暂无壁纸", "No Walls") \
    X(CLOUD_FETCHING_LIST, "正在拉取列表…", "Fetching list…") \
    X(CLOUD_FILE_INVALID, "文件无效", "Invalid file") \
    X(CLOUD_IN_QUEUE, "已在队列中", "Already queued") \
    X(CLOUD_JSON_CREATE_FAIL, "JSON 创建失败", "JSON create failed") \
    X(CLOUD_JSON_PARSE_FAIL, "JSON 解析失败", "JSON parse failed") \
    X(CLOUD_JSON_SERIALIZE_FAIL, "JSON 序列化失败", "JSON serialize failed") \
    X(CLOUD_LIST_TOO_LARGE, "列表过大", "List too large") \
    X(CLOUD_LOAD_COVER, "加载封面…", "Loading covers…") \
    X(CLOUD_LOAD_FAIL, "加载失败", "Load failed") \
    X(CLOUD_LOADING, "加载中…", "Loading…") \
    X(CLOUD_MISSING_TASK_ID, "缺少 taskId", "Missing taskId") \
    X(CLOUD_NEED_WIFI_CFG, "请先完成配网", "Set up Wi-Fi first") \
    X(CLOUD_NET_NOT_READY, "网络未就绪", "Network not ready") \
    X(CLOUD_NO_COVER, "暂无封面", "No cover") \
    X(CLOUD_NO_DOWNLOAD_URL, "缺少下载地址", "Missing download URL") \
    X(CLOUD_NO_LOCAL_FILE, "无本地文件", "No local file") \
    X(CLOUD_NO_NETWORK, "无网络", "No network") \
    X(CLOUD_NO_SD, "未检测到 SD 卡", "No SD card") \
    X(CLOUD_PATH_INVALID, "路径无效", "Invalid path") \
    X(CLOUD_PENDING, "待接收", "Incoming") \
    X(CLOUD_PLEASE_WAIT, "请稍候", "Please wait") \
    X(CLOUD_PREVIEW_FAIL, "无法预览", "Cannot preview") \
    X(CLOUD_PREVIEW_START_FAIL, "预览启动失败", "Failed to start preview") \
    X(CLOUD_PREVIEW_URL_LONG, "预览地址过长", "Preview URL too long") \
    X(CLOUD_PUSH_BOOK, "书籍推送", "Book push") \
    X(CLOUD_PUSH_FONT, "字体推送", "Font push") \
    X(CLOUD_PUSH_WALLPAPER, "壁纸推送", "Walls push") \
    X(CLOUD_READ_TIMEOUT, "读取超时或中断", "Read timeout or interrupted") \
    X(CLOUD_REFRESH, "刷新", "Refresh") \
    X(CLOUD_REFRESHED, "已刷新", "Refreshed") \
    X(CLOUD_REFRESHING, "刷新中…", "Refreshing…") \
    X(CLOUD_REQUEST_FAIL, "请求失败", "Request failed") \
    X(CLOUD_SAVE, "保存", "Save") \
    X(CLOUD_SAVE_FAIL, "保存失败", "Save failed") \
    X(CLOUD_SAVE_LOCAL, "保存到本地", "Save to device") \
    X(CLOUD_SAVED_0_1, "已保存 0 / 共 1", "Saved 0 / 1") \
    X(CLOUD_SAVED_FMT, "已保存 %d / 共 %d", "Saved %d / %d") \
    X(CLOUD_SAVED_SYNC_FAIL, "已保存(同步失败)", "Saved (sync failed)") \
    X(CLOUD_SAVING, "正在保存", "Saving") \
    X(CLOUD_SAVING_NAME_FMT, "正在保存\n%s", "Saving\n%s") \
    X(CLOUD_SELECT_FIRST, "请先选择", "Select items first") \
    X(CLOUD_SELECTED_FMT, "已选 %d", "Selected %d") \
    X(CLOUD_STORAGE_UNAVAIL, "存储目录不可用", "Storage unavailable") \
    X(CLOUD_SYNC_FAIL, "同步失败", "Sync failed") \
    X(CLOUD_TAB_ALL, "全部", "All") \
    X(CLOUD_TAB_BOOK, "书籍", "Books") \
    X(CLOUD_TAB_FONT, "字体", "Fonts") \
    X(CLOUD_TAB_WALLPAPER, "壁纸", "Walls") \
    X(CLOUD_TASK_FAIL, "任务失败", "Task failed") \
    X(CLOUD_TYPE_BOOK, "书籍", "Book") \
    X(CLOUD_TYPE_FONT, "字体", "Font") \
    X(CLOUD_TYPE_WALLPAPER, "壁纸", "Walls") \
    X(CLOUD_UNKNOWN_TYPE, "未知类型", "Unknown type") \
    X(CLOUD_WAIT_DOWNLOAD, "请等待下载任务结束", "Wait for download to finish") \
    X(COMMON_DELETE, "删除", "Delete") \
    X(COMMON_EMPTY, "暂无内容", "Empty") \
    X(COMMON_OFF, "关闭", "Off") \
    X(COMMON_ON, "开启", "On") \
    X(STANDBY_CONN_FAIL, "创建连接失败", "Connection failed") \
    X(STANDBY_DATE_FMT, "%02d月%02d日", "%02d/%02d") \
    X(STANDBY_DATE_PLACEHOLDER, "--月--日", "--/--") \
    X(STANDBY_DECODE_FAIL, "解码失败", "Decode failed") \
    X(STANDBY_EMPTY_TODO, "暂无待办", "No todos") \
    X(STANDBY_NO_DATA, "无数据", "No data") \
    X(STANDBY_NO_NETWORK, "无网络", "No network") \
    X(STANDBY_NO_TODO_CACHE, "暂无清单缓存", "No task cache") \
    X(STANDBY_NO_WEATHER, "无天气", "No weather") \
    X(STANDBY_PARSE_FAIL, "解析失败", "Parse failed") \
    X(STANDBY_REQUEST_FAIL, "请求失败", "Request failed") \
    X(STANDBY_WEATHER_FAIL, "天气失败", "Weather failed") \
    X(STANDBY_WP_NOT_FOUND, "未找到待机壁纸", "Standby wallpaper not found") \
    X(WALLPAPER_ADJUST, "调整", "Adjust") \
    X(WALLPAPER_CHIP_SHUTDOWN, "关机", "Power-off") \
    X(WALLPAPER_CHIP_STANDBY, "待机", "Standby") \
    X(WALLPAPER_DECODE_FAIL, "解码失败", "Decode failed") \
    X(WALLPAPER_DELETE_BTN, "删除壁纸", "Delete wallpaper") \
    X(WALLPAPER_DELETE_FAIL, "删除失败", "Delete failed") \
    X(WALLPAPER_DELETE_START_FAIL, "删除启动失败", "Failed to start delete") \
    X(WALLPAPER_DELETING, "正在删除…", "Deleting…") \
    X(WALLPAPER_EMPTY_FMT, "暂无壁纸\n请放到 %s", "No wallpapers\nPlace in %s") \
    X(WALLPAPER_ENABLE_FAIL, "启用失败", "Enable failed") \
    X(WALLPAPER_FILE_INVALID, "文件无效", "Invalid file") \
    X(WALLPAPER_FILE_MISSING, "文件不存在", "File not found") \
    X(WALLPAPER_LOADING, "加载中…", "Loading…") \
    X(WALLPAPER_MIRROR_H, "镜像", "Mirror") \
    X(WALLPAPER_NAME_INVALID, "文件名无效", "Invalid filename") \
    X(WALLPAPER_NO_SD, "未检测到 SD 卡", "No SD card") \
    X(WALLPAPER_OVERWRITE, "覆盖", "Overwrite") \
    X(WALLPAPER_PATH_INVALID, "路径无效", "Invalid path") \
    X(WALLPAPER_PREVIEW_FAIL, "无法预览", "Cannot preview") \
    X(WALLPAPER_PREVIEW_OOM, "内存不足，预览失败", "Out of memory, preview failed") \
    X(WALLPAPER_PREVIEW_START_FAIL, "预览启动失败", "Failed to start preview") \
    X(WALLPAPER_ROTATE_LEFT, "左转", "Left") \
    X(WALLPAPER_ROTATE_RIGHT, "右转", "Right") \
    X(WALLPAPER_SAVE, "保存", "Save") \
    X(WALLPAPER_SAVE_AS, "另存为", "Save as") \
    X(WALLPAPER_SAVE_FAIL, "保存失败", "Save failed") \
    X(WALLPAPER_SAVING, "正在保存…", "Saving…") \
    X(WALLPAPER_SELECTED_FMT, "已选 %d", "Selected %d") \
    X(WALLPAPER_SET_SHUTDOWN, "设为关机", "Set as power-off") \
    X(WALLPAPER_SET_STANDBY, "设为待机", "Set as standby") \
    X(WALLPAPER_SHUTDOWN_ON, "关机中", "Power-off on") \
    X(WALLPAPER_STANDBY_ON, "待机中", "Standby on")


namespace Lang {

enum class LangStringId : size_t {
#define LANG_ENUM(name, zh, en) name,
    LANG_STRING_LIST(LANG_ENUM)
#undef LANG_ENUM
    kCount,
};

static constexpr size_t kStringCount = static_cast<size_t>(LangStringId::kCount);

static const char* const kZh[kStringCount] = {
#define LANG_ZH(name, zh, en) zh,
    LANG_STRING_LIST(LANG_ZH)
#undef LANG_ZH
};

static const char* const kEn[kStringCount] = {
#define LANG_EN(name, zh, en) en,
    LANG_STRING_LIST(LANG_EN)
#undef LANG_EN
};

const char* CODE = LANG_DEFAULT_CODE;

namespace Strings {
#define LANG_DECL(name, zh, en) const char* name = "";
LANG_STRING_LIST(LANG_DECL)
#undef LANG_DECL
} // namespace Strings

static const char* NormalizeCode(const char* code) {
    if (code != nullptr && std::strcmp(code, "zh-CN") == 0) {
        return "zh-CN";
    }
    return "en-US";
}

static const char* const* LiteralsFor(const char* code) {
    if (code != nullptr && std::strcmp(code, "zh-CN") == 0) {
        return kZh;
    }
    return kEn;
}

static void ApplyLiterals(const char* const* literals) {
#define LANG_APPLY(name, zh, en)                                                                               \
    Strings::name = literals[static_cast<size_t>(LangStringId::name)];
    LANG_STRING_LIST(LANG_APPLY)
#undef LANG_APPLY
}

static void PersistTask(void* arg) {
    const char* code = static_cast<const char*>(arg);
    {
        Settings settings("ui", true);
        settings.SetString("language", code);
    }
    ESP_LOGI(TAG, "language persisted: %s", code);
    vTaskDelete(nullptr);
}

const char* Current() {
    return CODE;
}

bool SetLanguage(const char* code, bool persist) {
    const char* normalized = NormalizeCode(code);
    const bool changed = (CODE == nullptr) || (std::strcmp(CODE, normalized) != 0);
    ApplyLiterals(LiteralsFor(normalized));
    CODE = normalized;
    if (persist) {
        // LVGL worker 栈在 PSRAM：写 NVS 放到内部 RAM 栈任务
        if (xTaskCreate(PersistTask, "lang_nvs", 4096, const_cast<char*>(normalized), 5, nullptr) !=
            pdPASS) {
            ESP_LOGE(TAG, "xTaskCreate(lang_nvs) failed");
        }
    }
    return changed;
}

void InitFromNvs() {
    Settings settings("ui", false);
    const std::string saved = settings.GetString("language", "");
    if (saved.empty()) {
        SetLanguage(LANG_DEFAULT_CODE, false);
    } else {
        SetLanguage(saved.c_str(), false);
    }
}

namespace {
struct LangStaticInit {
    LangStaticInit() {
        Lang::SetLanguage(LANG_DEFAULT_CODE, false);
    }
};
LangStaticInit g_lang_static_init;
} // namespace

} // namespace Lang
