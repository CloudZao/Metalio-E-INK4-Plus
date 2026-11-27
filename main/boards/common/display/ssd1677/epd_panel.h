#pragma once

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

/**
 * @brief 初始化 EPD SPI 与 SSD1677 panel
 * @param panel_io 出参 panel IO
 * @param panel 出参 panel
 */
void EpdPanel_Init(esp_lcd_panel_io_handle_t* panel_io, esp_lcd_panel_handle_t* panel);
