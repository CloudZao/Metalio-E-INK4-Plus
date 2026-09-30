/**
 * @file epd_board_common.h
 * @brief Common board-level functions for EPD boards.
 *
 * This module provides a simple ADC-based temperature measurement interface for
 * EPD (E-paper Display) power/refresh temperature compensation.
 *
 * Features:
 *  - ADC One-shot mode based temperature sampling
 *  - Optional ADC calibration using ESP-IDF 5.3+ curve-fitting scheme
 *  - Multi-sample averaging for noise reduction
 *  - TMP36-style analog temperature sensor support
 *
 * Usage:
 *  1. Call `epd_board_temperature_init_v2()` once during board initialization.
 *  2. Call `epd_board_ambient_temperature_v2()` whenever temperature is required.
 */

#pragma once

/**
 * @brief Initialize ADC hardware and temperature sensor.
 *
 * This function:
 *  - Creates ADC1 unit (One-shot mode)
 *  - Configures ADC1 Channel 7
 *  - Enables ADC calibration using curve fitting (IDF 5.3+)
 */
void epd_board_temperature_init_v2();

/**
 * @brief Read ambient temperature (°C) using the analog sensor.
 *
 * Performs:
 *  - 100-sample averaging ADC read
 *  - Raw to voltage conversion (with calibration if available)
 *  - TMP36 temperature formula:
 *        Temp(°C) = (Voltage_mV - 500) / 10
 *
 * @return float Temperature in Celsius.
 */
float epd_board_ambient_temperature_v2();
