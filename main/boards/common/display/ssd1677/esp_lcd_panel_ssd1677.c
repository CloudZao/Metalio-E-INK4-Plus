/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_rom_sys.h"
#include "boards/common/display/ssd1677/lut/epd_lut_fastupdate.h"
#include "boards/common/display/ssd1677/lut/epd_lut_partial.h"
#include "boards/common/display/ssd1677/lut/epd_lut_4gray.h"
#include "esp_lcd_panel_ssd1677.h"
#include "esp_lcd_ssd1677_commands.h"

static const char* TAG = "lcd_panel.ssd1677";

// Keep DMA storage internal: PSRAM is not a safe replacement for this buffer.
// Whole rows are sent through this reusable buffer, rather than reserving 48 KB.
#define SSD1677_DMA_BOUNCE_BYTES 4096

typedef enum {
    EPD_LOADED_LUT_NONE = 0,  // 尚未下发 LUT
    EPD_LOADED_LUT_FAST,      // 快速全刷 LUT
    EPD_LOADED_LUT_PARTIAL,   // 局刷 LUT
    EPD_LOADED_LUT_GRAY,      // 灰刷 LUT
} epd_loaded_lut_t;

typedef struct {
    esp_lcd_epaper_panel_cb_t callback_ptr;  // 刷新完成回调
    void* args;                              // 回调用户数据
} epaper_panel_callback_t;

typedef struct {
    esp_lcd_panel_t base;                                      // panel 基类
    esp_lcd_panel_io_handle_t io;                              // panel IO
    int reset_gpio_num;                                        // RST 脚
    bool reset_level;                                          // RST 有效电平
    int busy_gpio_num;                                         // BUSY 脚
    int gap_x;                                                 // X 间隙
    int gap_y;                                                 // Y 间隙
    epaper_panel_callback_t epaper_refresh_done_isr_callback;  // BUSY ISR 回调
    esp_lcd_ssd1677_bitmap_color_t bitmap_color;               // 当前写 0x24/0x26
    esp_lcd_ssd1677_refresh_mode_t refresh_mode;               // 下次刷新模式
    epd_loaded_lut_t loaded_lut;                               // 芯片内已下发的 MCU LUT
    bool _non_copy_mode;                                       // true：不分配内部 framebuffer
    bool _mirror_x;                                            // X 镜像
    bool _mirror_y;                                            // Y 镜像
    bool _swap_xy;                                             // XY 交换（本驱动未用）
    bool _invert_color;                                        // 颜色反相
    uint8_t* _framebuffer;                                     // copy: full frame; non_copy: internal DMA bounce
} epaper_panel_t;

static inline uint8_t byte_reverse(uint8_t data);
static esp_err_t process_bitmap(esp_lcd_panel_t* panel, int len_x, int len_y, int src_stride, int dst_stride,
                                const void* color_data);
static esp_err_t panel_epaper_wait_busy(esp_lcd_panel_t* panel);
static void epaper_driver_gpio_isr_handler(void* arg);
static esp_err_t epaper_set_ram_area(esp_lcd_panel_io_handle_t io, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
static esp_err_t epaper_panel_load_fast_lut(esp_lcd_panel_t* panel);
static esp_err_t epaper_panel_load_partial_lut(esp_lcd_panel_t* panel);
static esp_err_t epaper_panel_load_gray_lut(esp_lcd_panel_t* panel);
static esp_err_t panel_epaper_set_vram(esp_lcd_panel_io_handle_t io, uint8_t cmd, const uint8_t* bitmap, size_t size);
static esp_err_t epaper_panel_del(esp_lcd_panel_t* panel);
static esp_err_t epaper_panel_reset(esp_lcd_panel_t* panel);
static esp_err_t epaper_panel_init(esp_lcd_panel_t* panel);
static esp_err_t epaper_panel_draw_bitmap(esp_lcd_panel_t* panel, int x_start, int y_start, int x_end, int y_end,
                                          const void* color_data);
static esp_err_t epaper_panel_invert_color(esp_lcd_panel_t* panel, bool invert_color_data);
static esp_err_t epaper_panel_mirror(esp_lcd_panel_t* panel, bool mirror_x, bool mirror_y);
static esp_err_t epaper_panel_swap_xy(esp_lcd_panel_t* panel, bool swap_axes);
static esp_err_t epaper_panel_set_gap(esp_lcd_panel_t* panel, int x_gap, int y_gap);
static esp_err_t epaper_panel_disp_on_off(esp_lcd_panel_t* panel, bool on_off);

static void epaper_driver_gpio_isr_handler(void* arg) {
    epaper_panel_t* epaper_panel = arg;
    gpio_intr_disable(epaper_panel->busy_gpio_num);
    if (epaper_panel->epaper_refresh_done_isr_callback.callback_ptr) {
        (epaper_panel->epaper_refresh_done_isr_callback.callback_ptr)(
            &(epaper_panel->base), NULL, epaper_panel->epaper_refresh_done_isr_callback.args);
    }
}

esp_err_t epaper_panel_register_event_callbacks(esp_lcd_panel_t* panel, epaper_panel_callbacks_t* cbs, void* user_ctx) {
    ESP_RETURN_ON_FALSE(panel && cbs, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    epaper_panel_t* epaper_panel = __containerof(panel, epaper_panel_t, base);
    epaper_panel->epaper_refresh_done_isr_callback.callback_ptr = cbs->on_epaper_refresh_done;
    epaper_panel->epaper_refresh_done_isr_callback.args = user_ctx;
    return ESP_OK;
}

esp_err_t epaper_panel_set_bitmap_color(esp_lcd_panel_t* panel, esp_lcd_ssd1677_bitmap_color_t color) {
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "panel is NULL");
    epaper_panel_t* epaper_panel = __containerof(panel, epaper_panel_t, base);
    epaper_panel->bitmap_color = color;
    return ESP_OK;
}

esp_err_t epaper_panel_set_refresh_mode(esp_lcd_panel_t* panel, esp_lcd_ssd1677_refresh_mode_t mode) {
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "panel is NULL");
    epaper_panel_t* epaper_panel = __containerof(panel, epaper_panel_t, base);
    epaper_panel->refresh_mode = mode;
    return ESP_OK;
}

static esp_err_t epaper_set_ram_area(esp_lcd_panel_io_handle_t io, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    uint16_t yrev = SSD1677_PANEL_HEIGHT - y - h;

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, SSD1677_CMD_DATA_ENTRY_MODE,
                                                  (uint8_t[]){SSD1677_PARAM_DATA_ENTRY_MODE_XY_INC_Y_DEC}, 1),
                        TAG, "DATA_ENTRY_MODE err");

    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(
            io, SSD1677_CMD_SET_RAMX_START_END_POS,
            (uint8_t[]){x & 0xFF, (uint8_t)(x >> 8), (uint8_t)((x + w - 1) & 0xFF), (uint8_t)((x + w - 1) >> 8)}, 4),
        TAG, "SET_RAMX err");

    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(io, SSD1677_CMD_SET_RAMY_START_END_POS,
                                  (uint8_t[]){(uint8_t)((yrev + h - 1) & 0xFF), (uint8_t)((yrev + h - 1) >> 8),
                                              (uint8_t)(yrev & 0xFF), (uint8_t)(yrev >> 8)},
                                  4),
        TAG, "SET_RAMY err");

    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(io, SSD1677_CMD_SET_INIT_X_ADDR_COUNTER, (uint8_t[]){x & 0xFF, (uint8_t)(x >> 8)}, 2),
        TAG, "SET_X_COUNTER err");

    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(io, SSD1677_CMD_SET_INIT_Y_ADDR_COUNTER,
                                  (uint8_t[]){(uint8_t)((yrev + h - 1) & 0xFF), (uint8_t)((yrev + h - 1) >> 8)}, 2),
        TAG, "SET_Y_COUNTER err");

    return ESP_OK;
}

// 对齐 demo Epaper_READBUSY：轮询到 BUSY=0 后再短延时（不只靠下降沿）
static esp_err_t panel_epaper_wait_busy(esp_lcd_panel_t* panel) {
    epaper_panel_t* epaper_panel = __containerof(panel, epaper_panel_t, base);
    while (gpio_get_level(epaper_panel->busy_gpio_num)) {
        vTaskDelay(1);
    }
    esp_rom_delay_us(100);
    return ESP_OK;
}

static esp_err_t panel_epaper_set_vram(esp_lcd_panel_io_handle_t io, uint8_t cmd, const uint8_t* bitmap, size_t size) {
    if (bitmap && size > 0) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, cmd, bitmap, size), TAG, "tx_color err");
    }
    return ESP_OK;
}

esp_err_t epaper_panel_refresh_screen(esp_lcd_panel_t* panel) {
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "panel is NULL");
    epaper_panel_t* epaper_panel = __containerof(panel, epaper_panel_t, base);

    // 对齐 demo：只写 0x22 + 0x20。
    uint8_t ctrl2 = SSD1677_PARAM_DISP_UPDATE_PARTIAL;

    switch (epaper_panel->refresh_mode) {
        case SSD1677_EPAPER_REFRESH_FULL:
            ctrl2 = SSD1677_PARAM_DISP_UPDATE_FULL;
            break;
        case SSD1677_EPAPER_REFRESH_STANDBY:
            ctrl2 = SSD1677_PARAM_DISP_UPDATE_FULL_STANDBY;  // 进/出待机与关机
            break;
        case SSD1677_EPAPER_REFRESH_FULL_FAST:
            ctrl2 = SSD1677_PARAM_DISP_UPDATE_FULL_FAST;
            ESP_RETURN_ON_ERROR(
                esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_WRITE_TEMP_REG, (uint8_t[]){0x6A}, 1), TAG,
                "WRITE_TEMP err");
            break;
        case SSD1677_EPAPER_REFRESH_PARTIAL:
            ctrl2 = SSD1677_PARAM_DISP_UPDATE_PARTIAL;
            break;
        case SSD1677_EPAPER_REFRESH_GRAY:
            ctrl2 = SSD1677_PARAM_DISP_UPDATE_GRAY;  // 0xCC：外置 LUT，不重载 OTP
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "refresh mode=%d 0x22=%02X opt=%d", (int)epaper_panel->refresh_mode, ctrl2, SSD1677_EPD_OPT_PATH);

    const bool full_sync = (epaper_panel->refresh_mode == SSD1677_EPAPER_REFRESH_FULL ||
                            epaper_panel->refresh_mode == SSD1677_EPAPER_REFRESH_STANDBY ||
                            epaper_panel->refresh_mode == SSD1677_EPAPER_REFRESH_FULL_FAST ||
                            epaper_panel->refresh_mode == SSD1677_EPAPER_REFRESH_GRAY);
    // 仅在全刷/局刷/灰刷切换时重载对应 LUT。
    if (epaper_panel->refresh_mode == SSD1677_EPAPER_REFRESH_GRAY) {
        if (epaper_panel->loaded_lut != EPD_LOADED_LUT_GRAY) {
            ESP_RETURN_ON_ERROR(epaper_panel_load_gray_lut(panel), TAG, "gray lut");
        }
        // 4 灰需使用 RED 平面，勿 bypass（Cross：单字节 CTRL1_NORMAL）
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_DISP_UPDATE_CTRL1,
                                                      (uint8_t[]){SSD1677_PARAM_DISP_UPDATE_CTRL1_NORMAL}, 1),
                            TAG, "CTRL1 normal err");
    } else if (full_sync) {
        if (epaper_panel->loaded_lut != EPD_LOADED_LUT_FAST) {
            ESP_RETURN_ON_ERROR(epaper_panel_load_fast_lut(panel), TAG, "fast lut");
        }
    } else if (epaper_panel->refresh_mode == SSD1677_EPAPER_REFRESH_PARTIAL) {
        if (epaper_panel->loaded_lut != EPD_LOADED_LUT_PARTIAL) {
            ESP_RETURN_ON_ERROR(epaper_panel_load_partial_lut(panel), TAG, "partial lut");
        }
    }

    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_DISP_UPDATE_CTRL2, (uint8_t[]){ctrl2}, 1), TAG,
        "DISP_UPDATE_CTRL2 err");

    if (full_sync) {
        // 全刷：同步轮询 BUSY，不挂下降沿 ISR。
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_MASTER_ACTIVATION, NULL, 0), TAG,
                            "MASTER_ACTIVATION err");
        ESP_RETURN_ON_ERROR(panel_epaper_wait_busy(panel), TAG, "busy after full act");
#if !SSD1677_EPD_OPT_PATH
        // 非优化：BUSY 完成后补 0x3F 0x01（6081325 位置）。
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_POST_FULL_UPDATE_OPT,
                                                      (uint8_t[]){SSD1677_PARAM_POST_FULL_UPDATE_OPT}, 1),
                            TAG, "POST_FULL_UPDATE_OPT err");
#endif
        return ESP_OK;
    }

    gpio_intr_enable(epaper_panel->busy_gpio_num);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_MASTER_ACTIVATION, NULL, 0), TAG,
                        "MASTER_ACTIVATION err");
    return ESP_OK;
}

esp_err_t esp_lcd_new_panel_ssd1677(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t* const panel_dev_config,
                                    esp_lcd_panel_handle_t* const ret_panel) {
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    esp_lcd_ssd1677_config_t* ssd1677_conf = panel_dev_config->vendor_config;
    ESP_RETURN_ON_FALSE(ssd1677_conf, ESP_ERR_INVALID_ARG, TAG, "vendor_config is NULL");

    epaper_panel_t* epaper_panel = calloc(1, sizeof(epaper_panel_t));
    ESP_RETURN_ON_FALSE(epaper_panel, ESP_ERR_NO_MEM, TAG, "no mem");

    epaper_panel->io = io;
    epaper_panel->reset_gpio_num = panel_dev_config->reset_gpio_num;
    epaper_panel->busy_gpio_num = ssd1677_conf->busy_gpio_num;
    epaper_panel->reset_level = panel_dev_config->flags.reset_active_high;
    epaper_panel->_non_copy_mode = ssd1677_conf->non_copy_mode;
    epaper_panel->bitmap_color = SSD1677_EPAPER_BITMAP_CURRENT;
    epaper_panel->refresh_mode = SSD1677_EPAPER_REFRESH_FULL_FAST;
    epaper_panel->loaded_lut = EPD_LOADED_LUT_NONE;
    epaper_panel->base.del = epaper_panel_del;
    epaper_panel->base.reset = epaper_panel_reset;
    epaper_panel->base.init = epaper_panel_init;
    epaper_panel->base.draw_bitmap = epaper_panel_draw_bitmap;
    epaper_panel->base.invert_color = epaper_panel_invert_color;
    epaper_panel->base.set_gap = epaper_panel_set_gap;
    epaper_panel->base.mirror = epaper_panel_mirror;
    epaper_panel->base.swap_xy = epaper_panel_swap_xy;
    epaper_panel->base.disp_on_off = epaper_panel_disp_on_off;
    *ret_panel = &(epaper_panel->base);

    if (!epaper_panel->_non_copy_mode) {
        epaper_panel->_framebuffer = heap_caps_malloc(SSD1677_PANEL_BUFFER_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        ESP_RETURN_ON_FALSE(epaper_panel->_framebuffer, ESP_ERR_NO_MEM, TAG, "framebuffer alloc failed");
    }

    if (epaper_panel->reset_gpio_num >= 0) {
        // 推挽：空闲必须真正拉高释放复位。开漏浮空无板上拉时 RST 会一直偏低，屏不起。
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .pin_bit_mask = 1ULL << epaper_panel->reset_gpio_num,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "RST gpio err");
        ESP_RETURN_ON_ERROR(gpio_set_level(epaper_panel->reset_gpio_num, !epaper_panel->reset_level), TAG,
                            "RST idle high err");
    }

    if (epaper_panel->busy_gpio_num >= 0) {
        // gpio_config(intr!=DISABLE) 会立刻 gpio_intr_enable；屏未复位前 BUSY 易抖，
        // 未挂 handler 或 enable→disable 窗口内会 ISR 风暴触发 Interrupt WDT。
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,  // 空闲偏 BUSY=0（ready）
            .pin_bit_mask = 1ULL << epaper_panel->busy_gpio_num,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "BUSY gpio err");
        ESP_RETURN_ON_ERROR(
            gpio_isr_handler_add(epaper_panel->busy_gpio_num, epaper_driver_gpio_isr_handler, epaper_panel), TAG,
            "BUSY isr err");
        // handler_add 会再 enable；先关掉，再设下降沿，仅局刷 MASTER_ACTIVATION 前打开
        gpio_intr_disable(epaper_panel->busy_gpio_num);
        ESP_RETURN_ON_ERROR(gpio_set_intr_type(epaper_panel->busy_gpio_num, GPIO_INTR_NEGEDGE), TAG,
                            "BUSY intr type err");
        gpio_intr_disable(epaper_panel->busy_gpio_num);
    }

    ESP_LOGI(TAG, "new ssd1677 panel @%p", epaper_panel);
    return ESP_OK;
}

static esp_err_t epaper_panel_del(esp_lcd_panel_t* panel) {
    epaper_panel_t* epaper_panel = __containerof(panel, epaper_panel_t, base);
    if (epaper_panel->reset_gpio_num >= 0) {
        gpio_reset_pin(epaper_panel->reset_gpio_num);
    }
    if (epaper_panel->busy_gpio_num >= 0) {
        gpio_isr_handler_remove(epaper_panel->busy_gpio_num);
        gpio_reset_pin(epaper_panel->busy_gpio_num);
    }
    if (epaper_panel->_framebuffer) {
        heap_caps_free(epaper_panel->_framebuffer);
        epaper_panel->_framebuffer = NULL;
    }
    free(epaper_panel);
    return ESP_OK;
}

static esp_err_t epaper_panel_reset(esp_lcd_panel_t* panel) {
    epaper_panel_t* epaper_panel = __containerof(panel, epaper_panel_t, base);
    if (epaper_panel->reset_gpio_num >= 0) {
        ESP_RETURN_ON_ERROR(gpio_set_level(epaper_panel->reset_gpio_num, epaper_panel->reset_level), TAG,
                            "RST low err");
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_RETURN_ON_ERROR(gpio_set_level(epaper_panel->reset_gpio_num, !epaper_panel->reset_level), TAG,
                            "RST high err");
        vTaskDelay(pdMS_TO_TICKS(10));
    } else {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_SWRST, NULL, 0), TAG, "SWRST err");
    }
    epaper_panel->loaded_lut = EPD_LOADED_LUT_NONE;  // HW/soft reset 后芯片 LUT 失效
    return panel_epaper_wait_busy(panel);
}

static esp_err_t epaper_panel_load_lut(esp_lcd_panel_t* panel, const uint8_t* lut, size_t wave_bytes) {
    ESP_RETURN_ON_FALSE(lut && wave_bytes > 0, ESP_ERR_INVALID_ARG, TAG, "lut null");
    epaper_panel_t* epaper_panel = __containerof(panel, epaper_panel_t, base);
    esp_lcd_panel_io_handle_t io = epaper_panel->io;

    // 对齐 Epaper_LUT_By_MCU：VCOM / Gate / Source → 0x32
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, SSD1677_CMD_WRITE_VCOM, (uint8_t[]){lut[109]}, 1), TAG,
                        "VCOM lut err");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, SSD1677_CMD_GATE_VOLTAGE_CTRL, (uint8_t[]){lut[105]}, 1), TAG,
                        "GATE lut err");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, SSD1677_CMD_SOURCE_VOLTAGE_CTRL,
                                                  (uint8_t[]){
                                                      lut[106],
                                                      lut[107],
                                                      lut[108],
                                                  },
                                                  3),
                        TAG, "SOURCE lut err");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, SSD1677_CMD_WRITE_LUT, lut, wave_bytes), TAG, "WRITE_LUT err");
    return panel_epaper_wait_busy(panel);
}

static esp_err_t epaper_panel_load_fast_lut(esp_lcd_panel_t* panel) {
    epaper_panel_t* epaper_panel = __containerof(panel, epaper_panel_t, base);
    ESP_RETURN_ON_ERROR(epaper_panel_load_lut(panel, kEpdLutFastUpdate, EPD_LUT_FAST_WAVE_BYTES), TAG, "fast lut load");
    epaper_panel->loaded_lut = EPD_LOADED_LUT_FAST;
    return ESP_OK;
}

static esp_err_t epaper_panel_load_partial_lut(esp_lcd_panel_t* panel) {
    epaper_panel_t* epaper_panel = __containerof(panel, epaper_panel_t, base);
    ESP_RETURN_ON_ERROR(epaper_panel_load_lut(panel, kEpdLutPartial, EPD_LUT_PARTIAL_WAVE_BYTES), TAG,
                        "partial lut load");
    epaper_panel->loaded_lut = EPD_LOADED_LUT_PARTIAL;
    return ESP_OK;
}

static esp_err_t epaper_panel_load_gray_lut(esp_lcd_panel_t* panel) {
    epaper_panel_t* epaper_panel = __containerof(panel, epaper_panel_t, base);
    ESP_RETURN_ON_ERROR(epaper_panel_load_lut(panel, kEpdLut4Gray, EPD_LUT_4GRAY_WAVE_BYTES), TAG, "gray lut load");
    epaper_panel->loaded_lut = EPD_LOADED_LUT_GRAY;
    return ESP_OK;
}

static esp_err_t epaper_panel_init(esp_lcd_panel_t* panel) {
    epaper_panel_t* epaper_panel = __containerof(panel, epaper_panel_t, base);
    esp_lcd_panel_io_handle_t io = epaper_panel->io;

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, SSD1677_CMD_SWRST, NULL, 0), TAG, "SWRST err");
    vTaskDelay(pdMS_TO_TICKS(10));
    epaper_panel->loaded_lut = EPD_LOADED_LUT_NONE;

    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(io, SSD1677_CMD_TEMP_SENSOR_CTRL, (uint8_t[]){SSD1677_PARAM_TEMP_SENSOR_INTERNAL}, 1),
        TAG, "TEMP_SENSOR err");
    // #if SSD1677_EPD_OPT_PATH
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, SSD1677_CMD_PING_PONG,
                                                  (uint8_t[]){
                                                      0x00,
                                                      0x00,
                                                      0x00,
                                                      0x00,
                                                      0x00,
                                                      0x40,
                                                      0x00,
                                                      0x00,
                                                      0x00,
                                                      0x00,
                                                  },
                                                  10),
                        TAG, "PING_PONG err");
    // #endif
    // Booster Soft-start（档位见 SSD1677_BOOSTER_SOFT_START_LEVEL）
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, SSD1677_CMD_BOOSTER_SOFT_START,
                                                  (uint8_t[]){
                                                      SSD1677_PARAM_BOOSTER_SOFT_START_A,
                                                      SSD1677_PARAM_BOOSTER_SOFT_START_B,
                                                      SSD1677_PARAM_BOOSTER_SOFT_START_C,
                                                      SSD1677_PARAM_BOOSTER_SOFT_START_D,
                                                      SSD1677_PARAM_BOOSTER_SOFT_START_E,
                                                  },
                                                  5),
                        TAG, "BOOSTER soft-start err");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, SSD1677_CMD_DRIVER_OUTPUT_CTRL,
                                                  (uint8_t[]){(uint8_t)((SSD1677_PANEL_HEIGHT - 1) & 0xFF),
                                                              (uint8_t)((SSD1677_PANEL_HEIGHT - 1) >> 8), 0x02},
                                                  3),
                        TAG, "DRIVER_OUTPUT err");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(io, SSD1677_CMD_SET_BORDER_WAVEFORM, (uint8_t[]){SSD1677_PARAM_BORDER_WAVEFORM}, 1),
        TAG, "BORDER err");

    ESP_RETURN_ON_ERROR(epaper_panel_load_fast_lut(panel), TAG, "fast lut err");
    ESP_RETURN_ON_ERROR(epaper_set_ram_area(io, 0, 0, SSD1677_PANEL_WIDTH, SSD1677_PANEL_HEIGHT), TAG, "RAM area err");
    return ESP_OK;
}

static esp_err_t epaper_panel_draw_bitmap(esp_lcd_panel_t* panel, int x_start, int y_start, int x_end, int y_end,
                                          const void* color_data) {
    epaper_panel_t* epaper_panel = __containerof(panel, epaper_panel_t, base);
    if (gpio_get_level(epaper_panel->busy_gpio_num)) {
        return ESP_ERR_NOT_FINISHED;
    }

    x_start += epaper_panel->gap_x;
    x_end += epaper_panel->gap_x;
    y_start += epaper_panel->gap_y;
    y_end += epaper_panel->gap_y;

    if (epaper_panel->_non_copy_mode) {
        ESP_RETURN_ON_FALSE(!epaper_panel->_swap_xy, ESP_ERR_INVALID_ARG, TAG, "swap_xy unavailable in non_copy_mode");
        ESP_RETURN_ON_FALSE(!epaper_panel->_mirror_y, ESP_ERR_INVALID_ARG, TAG,
                            "mirror_y unavailable in non_copy_mode");
    }

    ESP_RETURN_ON_FALSE(color_data, ESP_ERR_INVALID_ARG, TAG, "bitmap is null");
    ESP_RETURN_ON_FALSE(x_start < x_end && y_start < y_end, ESP_ERR_INVALID_ARG, TAG, "invalid area");

    int len_x = x_end - x_start;
    int len_y = y_end - y_start;
    int x_draw = x_start;
    int y_draw = y_start;
    int w_draw = len_x;
    int h_draw = len_y;

    x_draw -= x_draw % 8;
    w_draw = ((w_draw + 7) / 8) * 8;

    int src_stride = (len_x + 7) / 8;
    int dst_stride = w_draw / 8;
    int buffer_size = dst_stride * h_draw;
    const uint8_t vram_cmd = (epaper_panel->bitmap_color == SSD1677_EPAPER_BITMAP_PREVIOUS) ? SSD1677_CMD_WRITE_RED_VRAM
                                                                                            : SSD1677_CMD_WRITE_BW_VRAM;

    const bool direct_ok = esp_ptr_dma_capable(color_data) && !esp_ptr_external_ram(color_data);
    // copy/直发路径先设整帧窗口并排空上次 DMA；bounce 路径逐块设窗口。
    if (!epaper_panel->_non_copy_mode || direct_ok) {
        ESP_RETURN_ON_ERROR(epaper_set_ram_area(epaper_panel->io, x_draw, y_draw, w_draw, h_draw), TAG, "RAM area err");
    }

    const uint8_t* tx_buf = NULL;
    if (epaper_panel->_non_copy_mode) {
        // 仅内部 DMA 内存可直发。PSRAM（即使带 DMA cap）经 cache 写入后直 DMA
        // 易丢数据/鬼影；统一走 CPU memcpy → 内部 bounce，再 SPI DMA。
        if (direct_ok) {
            tx_buf = color_data;
        } else {
            ESP_RETURN_ON_FALSE(buffer_size <= SSD1677_PANEL_BUFFER_SIZE, ESP_ERR_INVALID_SIZE, TAG,
                                "bitmap too large");
            ESP_RETURN_ON_FALSE(dst_stride <= SSD1677_DMA_BOUNCE_BYTES, ESP_ERR_INVALID_SIZE, TAG,
                                "bitmap row too large");
            if (!epaper_panel->_framebuffer) {
                epaper_panel->_framebuffer =
                    heap_caps_malloc(SSD1677_DMA_BOUNCE_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
                ESP_RETURN_ON_FALSE(epaper_panel->_framebuffer, ESP_ERR_NO_MEM, TAG, "dma bounce alloc failed");
            }
            const int rows_per_chunk = SSD1677_DMA_BOUNCE_BYTES / dst_stride;
            for (int row = 0; row < h_draw; row += rows_per_chunk) {
                const int rows = (h_draw - row < rows_per_chunk) ? h_draw - row : rows_per_chunk;
                const size_t bytes = (size_t)rows * dst_stride;
                // Reset the controller window/counters for each stripe, including reversed Y.
                ESP_RETURN_ON_ERROR(epaper_set_ram_area(epaper_panel->io, x_draw, y_draw + row, w_draw, rows), TAG,
                                    "stripe RAM area err");
                memcpy(epaper_panel->_framebuffer, (const uint8_t*)color_data + (size_t)row * dst_stride, bytes);
                ESP_RETURN_ON_ERROR(
                    panel_epaper_set_vram(epaper_panel->io, vram_cmd, epaper_panel->_framebuffer, bytes), TAG,
                    "stripe VRAM err");
                // tx_color only queues DMA. Drain it before overwriting OR returning the bounce.
                // A command of -1 sends no bytes; IDF still waits for all queued transfers.
                ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, -1, NULL, 0), TAG,
                                    "stripe DMA wait err");
            }
            return ESP_OK;
        }
    } else {
        process_bitmap(panel, w_draw, h_draw, src_stride, dst_stride, color_data);
        tx_buf = epaper_panel->_framebuffer;
    }

    ESP_RETURN_ON_ERROR(panel_epaper_set_vram(epaper_panel->io, vram_cmd, tx_buf, buffer_size), TAG, "set_vram err");
    return ESP_OK;
}

static esp_err_t epaper_panel_invert_color(esp_lcd_panel_t* panel, bool invert_color_data) {
    epaper_panel_t* epaper_panel = __containerof(panel, epaper_panel_t, base);
    epaper_panel->_invert_color = invert_color_data;
    return ESP_OK;
}

static esp_err_t epaper_panel_mirror(esp_lcd_panel_t* panel, bool mirror_x, bool mirror_y) {
    epaper_panel_t* epaper_panel = __containerof(panel, epaper_panel_t, base);
    if (mirror_y && epaper_panel->_non_copy_mode) {
        return ESP_ERR_INVALID_ARG;
    }
    epaper_panel->_mirror_x = mirror_x;
    epaper_panel->_mirror_y = mirror_y;
    return ESP_OK;
}

static esp_err_t epaper_panel_swap_xy(esp_lcd_panel_t* panel, bool swap_axes) {
    epaper_panel_t* epaper_panel = __containerof(panel, epaper_panel_t, base);
    if (swap_axes && epaper_panel->_non_copy_mode) {
        return ESP_ERR_INVALID_ARG;
    }
    epaper_panel->_swap_xy = swap_axes;
    return ESP_OK;
}

static esp_err_t epaper_panel_set_gap(esp_lcd_panel_t* panel, int x_gap, int y_gap) {
    epaper_panel_t* epaper_panel = __containerof(panel, epaper_panel_t, base);
    epaper_panel->gap_x = x_gap;
    epaper_panel->gap_y = y_gap;
    return ESP_OK;
}

static esp_err_t epaper_panel_disp_on_off(esp_lcd_panel_t* panel, bool on_off) {
    epaper_panel_t* epaper_panel = __containerof(panel, epaper_panel_t, base);
    if (on_off) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_DISP_UPDATE_CTRL2,
                                                      (uint8_t[]){SSD1677_PARAM_DISP_UPDATE_POWER_ON}, 1),
                            TAG, "power on err");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_MASTER_ACTIVATION, NULL, 0), TAG,
                            "power on activation err");
        return panel_epaper_wait_busy(panel);
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_DISP_UPDATE_CTRL2,
                                                  (uint8_t[]){SSD1677_PARAM_DISP_UPDATE_POWER_OFF}, 1),
                        TAG, "power off err");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_MASTER_ACTIVATION, NULL, 0), TAG,
                        "power off activation err");
    return panel_epaper_wait_busy(panel);
}

esp_err_t epaper_panel_prepare_for_partial(esp_lcd_panel_t* panel) {
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "panel is NULL");
    epaper_panel_t* epaper_panel = __containerof(panel, epaper_panel_t, base);

    // 局刷前边框 floating 0x80（无 HW RST / soft reset）
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_SET_BORDER_WAVEFORM,
                                                  (uint8_t[]){SSD1677_PARAM_BORDER_WAVEFORM_VCOM}, 1),
                        TAG, "BORDER partial prep err");
    return ESP_OK;
}

esp_err_t epaper_panel_deep_sleep(esp_lcd_panel_t* panel) {
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "panel is NULL");
    epaper_panel_t* epaper_panel = __containerof(panel, epaper_panel_t, base);

    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(epaper_panel->io, SSD1677_CMD_DEEP_SLEEP, (uint8_t[]){SSD1677_PARAM_DEEP_SLEEP}, 1),
        TAG, "DEEP_SLEEP err");
    ESP_LOGI(TAG, "deep sleep (0x10/%02X)", SSD1677_PARAM_DEEP_SLEEP);
    return ESP_OK;
}

static esp_err_t process_bitmap(esp_lcd_panel_t* panel, int len_x, int len_y, int src_stride, int dst_stride,
                                const void* color_data) {
    epaper_panel_t* epaper_panel = __containerof(panel, epaper_panel_t, base);
    const uint8_t* src = color_data;
    uint8_t* dst = epaper_panel->_framebuffer;

    memset(dst, epaper_panel->_invert_color ? 0x00 : 0xFF, dst_stride * len_y);

    for (int row = 0; row < len_y; row++) {
        int src_row = epaper_panel->_mirror_y ? (len_y - 1 - row) : row;
        int dst_row = row;
        const uint8_t* src_line = &src[src_row * src_stride];
        uint8_t* dst_line = &dst[dst_row * dst_stride];

        for (int col = 0; col < dst_stride; col++) {
            uint8_t data = (col < src_stride) ? src_line[col] : 0xFF;
            if (epaper_panel->_mirror_x) {
                data = byte_reverse(data);
            }
            if (epaper_panel->_invert_color) {
                data = ~data;
            }
            dst_line[col] = data;
        }
    }

    (void)len_x;  // 形参：本路径按 stride 拷贝，不按像素宽裁剪
    return ESP_OK;
}

static inline uint8_t byte_reverse(uint8_t data) {
    static const uint8_t lut[] = {0x00, 0x08, 0x04, 0x0C, 0x02, 0x0A, 0x06, 0x0E,
                                  0x01, 0x09, 0x05, 0x0D, 0x03, 0x0B, 0x07, 0x0F};
    return (uint8_t)((lut[data & 0x0F] << 4) | lut[data >> 4]);
}
