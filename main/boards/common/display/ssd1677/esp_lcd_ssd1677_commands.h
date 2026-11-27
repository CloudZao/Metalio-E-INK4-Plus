/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

// SSD1677 命令/参数，适配 GDEM0397T81 800x480
// 0：旧流程（全刷 0xC7 / 局刷 0xCF；全刷 BUSY 后写 0x3F；无 0x37）
// 1：优化档（0xC4/0xCC + init 0x37；0x3F 不走旧位置）
#ifndef SSD1677_EPD_OPT_PATH
#define SSD1677_EPD_OPT_PATH 0
#endif

#define SSD1677_CMD_SWRST 0x12                    // 软复位
#define SSD1677_CMD_DRIVER_OUTPUT_CTRL 0x01       // 驱动输出控制
#define SSD1677_CMD_GATE_VOLTAGE_CTRL 0x03        // Gate 电压
#define SSD1677_CMD_SOURCE_VOLTAGE_CTRL 0x04      // Source 电压
#define SSD1677_CMD_BOOSTER_SOFT_START 0x0C       // Booster soft-start
#define SSD1677_CMD_DEEP_SLEEP 0x10               // Deep Sleep
#define SSD1677_CMD_DATA_ENTRY_MODE 0x11          // 数据进入模式
#define SSD1677_CMD_TEMP_SENSOR_CTRL 0x18         // 温度传感器控制
#define SSD1677_CMD_MASTER_ACTIVATION 0x20        // Master Activation
#define SSD1677_CMD_DISP_UPDATE_CTRL1 0x21        // Display Update Control 1
#define SSD1677_CMD_DISP_UPDATE_CTRL2 0x22        // Display Update Control 2
#define SSD1677_CMD_WRITE_BW_VRAM 0x24            // 写 BW RAM（当前帧）
#define SSD1677_CMD_WRITE_RED_VRAM 0x26           // 写 RED RAM（上一帧）
#define SSD1677_CMD_WRITE_VCOM 0x2C               // 写 VCOM
#define SSD1677_CMD_WRITE_LUT 0x32                // 写 LUT
#define SSD1677_CMD_PING_PONG 0x37                // Ping-Pong（优化档 init 用）
#define SSD1677_CMD_WRITE_TEMP_REG 0x1A           // 写温度寄存器
#define SSD1677_CMD_SET_BORDER_WAVEFORM 0x3C      // 边框波形
#define SSD1677_CMD_POST_FULL_UPDATE_OPT 0x3F     // 全刷后选项（旧流程：BUSY 后再写）
#define SSD1677_CMD_SET_RAMX_START_END_POS 0x44   // RAM X 起止
#define SSD1677_CMD_SET_RAMY_START_END_POS 0x45   // RAM Y 起止
#define SSD1677_CMD_SET_INIT_X_ADDR_COUNTER 0x4E  // X 地址计数初值
#define SSD1677_CMD_SET_INIT_Y_ADDR_COUNTER 0x4F  // Y 地址计数初值

#define SSD1677_PARAM_POST_FULL_UPDATE_OPT 0x01  // 非优化：全刷 BUSY 后 0x3F 参数

#define SSD1677_PARAM_DATA_ENTRY_MODE_XY_INC_Y_DEC 0x01  // X 增、Y 减
#define SSD1677_PARAM_BORDER_WAVEFORM 0x01               // 边框默认波形
// 边框与 VCOM 配合，减轻局刷/关机全刷边缘发灰
#define SSD1677_PARAM_BORDER_WAVEFORM_VCOM 0x80  // 边框 floating/VCOM
#define SSD1677_PARAM_TEMP_SENSOR_INTERNAL 0x80  // 使用内部温度传感器

// Booster soft-start：A~D 固定，E 由档位宏切换
#ifndef SSD1677_BOOSTER_SOFT_START_LEVEL
#define SSD1677_BOOSTER_SOFT_START_LEVEL 2  // 1→E=0x40；2→E=0x80
#endif
#define SSD1677_PARAM_BOOSTER_SOFT_START_A 0xAE  // Booster A
#define SSD1677_PARAM_BOOSTER_SOFT_START_B 0xC7  // Booster B
#define SSD1677_PARAM_BOOSTER_SOFT_START_C 0xC3  // Booster C
#define SSD1677_PARAM_BOOSTER_SOFT_START_D 0xC0  // Booster D
#if SSD1677_BOOSTER_SOFT_START_LEVEL == 1
#define SSD1677_PARAM_BOOSTER_SOFT_START_E 0x40  // Booster E（档位 1）
#else
#define SSD1677_PARAM_BOOSTER_SOFT_START_E 0x80  // Booster E（档位 2）
#endif

#if SSD1677_EPD_OPT_PATH
#define SSD1677_PARAM_DEEP_SLEEP 0x03  // Deep Sleep（优化档）
#else
#define SSD1677_PARAM_DEEP_SLEEP 0x01  // Deep Sleep（对齐 demo 0x10/0x01）
#endif

#define SSD1677_PARAM_DISP_UPDATE_CTRL1_BYPASS_RED 0x40  // CTRL1：旁路 RED
#define SSD1677_PARAM_DISP_UPDATE_CTRL1_NORMAL 0x00      // CTRL1：正常

#if SSD1677_EPD_OPT_PATH
#define SSD1677_PARAM_DISP_UPDATE_FULL 0xC4          // 0x22：MCU LUT 全刷（优化档）
#define SSD1677_PARAM_DISP_UPDATE_FULL_STANDBY 0xC4  // 0x22：待机/关机全刷（优化档）
#define SSD1677_PARAM_DISP_UPDATE_PARTIAL 0xCC       // 0x22：MCU 局刷 LUT（优化档）
#else
#define SSD1677_PARAM_DISP_UPDATE_FULL 0xC7          // 0x22：MCU LUT 全刷
#define SSD1677_PARAM_DISP_UPDATE_FULL_STANDBY 0xC7  // 0x22：进/出待机与关机（同全刷）
#define SSD1677_PARAM_DISP_UPDATE_PARTIAL 0xCF       // 0x22：MCU 局刷 LUT
#endif
// 外置灰 LUT：CLOCK|ANALOG|DISPLAY，不重载 OTP
#define SSD1677_PARAM_DISP_UPDATE_GRAY 0xCC       // 0x22：灰刷激活
#define SSD1677_PARAM_DISP_UPDATE_FULL_FAST 0xD7  // 0x22：快速全刷

#define SSD1677_PARAM_DISP_UPDATE_POWER_ON 0xE0   // 上电序列
#define SSD1677_PARAM_DISP_UPDATE_POWER_OFF 0x83  // 掉电序列

#define SSD1677_PANEL_WIDTH 800                                                     // 面板宽
#define SSD1677_PANEL_HEIGHT 480                                                    // 面板高
#define SSD1677_PANEL_BUFFER_SIZE (SSD1677_PANEL_WIDTH * SSD1677_PANEL_HEIGHT / 8)  // 1bpp 帧缓冲字节数
