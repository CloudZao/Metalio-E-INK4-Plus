#pragma once
// Read after a stopped scan; no ISR logging is needed for stream diagnosis.
void epd_lcd_get_stream_stats(int* max_eof_gap_us,int* max_eof_work_us);
/** @brief 获取已停止扫描的最近一相 LCD 扫描耗时，单位为微秒。 */
int epd_lcd_get_scan_time_us(void);

#include <driver/gpio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief 获取最近一次 SPV 下降沿的软件时间戳，单位为微秒；仅在扫描停止后读取。 */
int64_t epd_lcd_get_spv_start_us(void);

/**
 * LCD bus configuration parameters.
 */
typedef struct {
    // GPIO numbers of the parallel bus pins.
    gpio_num_t data[16];

    // horizontal clock pin.
    gpio_num_t clock;
    // vertical clock pin
    gpio_num_t ckv;

    // horizontal "Start Pulse", enabling data input on the line shift register
    gpio_num_t start_pulse;
    // latch enable
    gpio_num_t leh;
    // vertical start pulse, resetting the vertical line shift register.
    gpio_num_t stv;
} lcd_bus_config_t;

/// Configuration structure for the LCD-based Epd driver.
typedef struct {
    // high time for CKV in 1/10us.
    size_t pixel_clock;    // = 12000000
    int ckv_high_time;     // = 70
    int line_front_porch;  // = 4
    int le_high_time;      // = 4
    int bus_width;         // = 16
    lcd_bus_config_t bus;
    int dummy_clocks_after_data; // Minimum XCL clocks before the next XLE pulse.
    int white_border_pixels; // Multiple of four; output may only hold/whiten here.
} LcdEpdConfig_t;

typedef bool (*line_cb_func_t)(void*, uint8_t*);
typedef void (*frame_done_func_t)(void*);

void epd_lcd_init(const LcdEpdConfig_t* config, int display_width, int display_height);
void epd_lcd_deinit();
void epd_lcd_frame_done_cb(frame_done_func_t, void* payload);
void epd_lcd_line_source_cb(line_cb_func_t, void* payload);
void epd_lcd_start_frame();
void epd_lcd_prime_white_border(bool enable);
int epd_lcd_white_border_pixels(void);
/** @brief 获取驱动实际配置的扫描行数，须在 LCD 初始化后调用。 */
int epd_lcd_get_display_lines(void);
/**
 * @brief 在任务上下文中停止当前帧并等待旧中断处理结束，不调用完成回调
 * @note 返回后才可更改回调或复用扫描缓冲。
 */
void epd_lcd_abort_frame();
/**
 * Set the LCD pixel clock frequency in MHz.
 */
void epd_lcd_set_pixel_clock_MHz(int frequency);
