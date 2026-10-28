/**
 * @file epd_board_metalio_eink4_plus.c
 * @brief Metalio ESP32-S31 4.7" 并行墨水屏板级（E0470A14-AF 1216x684）
 * @note 引脚来自板级 config.h / 原理图 V1.5；TPS 控制脚经 TCA9555
 */

#include "epd_board_metalio_eink4_plus.h"
#include "epdiy.h"

#include "lcd_driver.h"
#include "metalio_sd.h"
#include "pca9555.h"
#include "metalio_i2c1.h"
#include "tps65185.h"
#include "config.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_rom_sys.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <hal/gpio_ll.h>
#include <hal/uhci_ll.h>
#include <esp_private/periph_ctrl.h>

#define TAG "epd_eink4p"

/* I2C：TPS/TCA 同总线；TCA 开漏 INT → ESP */
#define CFG_SCL I2C_SCL_PIN
#define CFG_SDA I2C_SDA_PIN
#define CFG_INTR IO_EXPANDER_INT_GPIO
#define EPDIY_I2C_PORT I2C_NUM_1

/* TCA 位/口：一律由 config.h 的 EPD_TPS_IO_* / EPD_IO_* 推出 */
#define CFG_IO_BIT(io)  (1u << ((unsigned)(io) & 7u))
#define CFG_IO_PORT(io) ((int)((unsigned)(io) >> 3))

#define CFG_PIN_VCOM_CTRL CFG_IO_BIT(EPD_TPS_IO_VCOM_CTRL)
#define CFG_PIN_PWRUP     CFG_IO_BIT(EPD_TPS_IO_PWRUP)
#define CFG_PIN_WAKEUP    CFG_IO_BIT(EPD_TPS_IO_WAKEUP)
#define CFG_PIN_PWRGOOD   CFG_IO_BIT(EPD_TPS_IO_PWR_GOOD)
#define CFG_PIN_INT       CFG_IO_BIT(EPD_TPS_IO_INT)
#define CFG_PIN_MOTOR     CFG_IO_BIT(EPD_IO_MOTOR)
#define CFG_PIN_PA_EN     CFG_IO_BIT(EPD_IO_PA_EN)
#define CFG_PIN_CAM_SCR   CFG_IO_BIT(EPD_IO_CAM_SCR_EN)
#define CFG_PIN_TP_RST    CFG_IO_BIT(EPD_IO_TP_RST)
#define CFG_PIN_BT_PA_PWR CFG_IO_BIT(EPD_IO_BT_PA_PWR)
#define CFG_PIN_PWR_PULSE CFG_IO_BIT(EPD_IO_PWR_PULSE)

#define CFG_PORT_CTRL CFG_IO_PORT(EPD_TPS_IO_VCOM_CTRL) // Port0：VCOM/PWRUP/WAKEUP/PWR_GOOD
#define CFG_PORT_INT  CFG_IO_PORT(EPD_TPS_IO_INT)       // Port1：TPS_INT / TP_RST / 关机脉冲

/* Port0：TPS/Motor/PA/摄像屏电 输出；PWR_GOOD 等为输入 */
#define CFG_P0_TPS_OUT_MASK (CFG_PIN_VCOM_CTRL | CFG_PIN_PWRUP | CFG_PIN_WAKEUP)
#define CFG_P0_OUTPUT_MASK                                                                 \
    (CFG_P0_TPS_OUT_MASK | CFG_PIN_MOTOR | CFG_PIN_PA_EN | CFG_PIN_CAM_SCR)
#define CFG_P0_INPUT_MASK ((uint8_t)(~CFG_P0_OUTPUT_MASK))

/* Port1：TP_RST/P14/关机脉冲为输出；P16=RTC_INT 等为输入 */
#define CFG_P1_RAIL_MASK   (CFG_PIN_TP_RST | CFG_PIN_BT_PA_PWR)
#define CFG_P1_OUTPUT_MASK (CFG_P1_RAIL_MASK | CFG_PIN_PWR_PULSE)
#define CFG_P1_INPUT_MASK  ((uint8_t)(~CFG_P1_OUTPUT_MASK))

/* 并行数据 / 控制：板级 config.h */
#define D0 EPD_PIN_D0
#define D1 EPD_PIN_D1
#define D2 EPD_PIN_D2
#define D3 EPD_PIN_D3
#define D4 EPD_PIN_D4
#define D5 EPD_PIN_D5
#define D6 EPD_PIN_D6
#define D7 EPD_PIN_D7

#define CKH EPD_PIN_XCL
#define LEH EPD_PIN_XLE
#define EPD_OE EPD_PIN_XOE
#define STH EPD_PIN_XSTL
#define STV EPD_PIN_SPV
#define EPD_MODE EPD_PIN_MODE
#define EPD_BORDER EPD_PIN_BORDER
#define CKV EPD_PIN_CKV

typedef struct {
    i2c_port_t port;
    bool pwrup;
    bool vcom_ctrl;
    bool wakeup;
} epd_config_register_t;

static int vcom = EPD_VCOM_MV;
static epd_config_register_t config_reg;
static bool interrupt_done = false;
static SemaphoreHandle_t s_ioexp_irq_sem = NULL;
static TaskHandle_t s_gpio2_irq_task = NULL;

static void IRAM_ATTR interrupt_handler(void* arg) {
    (void)arg;
    interrupt_done = true;
    BaseType_t hp = pdFALSE;
    if (s_ioexp_irq_sem != NULL) {
        xSemaphoreGiveFromISR(s_ioexp_irq_sem, &hp);
    }
    if (s_gpio2_irq_task != NULL) {
        vTaskNotifyGiveFromISR(s_gpio2_irq_task, &hp);
    }
    if (hp == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static lcd_bus_config_t lcd_config = {
    .clock = CKH,
    .ckv = CKV,
    .leh = LEH,
    .start_pulse = STH,
    .stv = STV,
    .data[0] = D0,
    .data[1] = D1,
    .data[2] = D2,
    .data[3] = D3,
    .data[4] = D4,
    .data[5] = D5,
    .data[6] = D6,
    .data[7] = D7,
    .data[8] = D7,
    .data[9] = D7,
    .data[10] = D7,
    .data[11] = D7,
    .data[12] = D7,
    .data[13] = D7,
    .data[14] = D7,
    .data[15] = D7,
};

static bool s_ioexp_ok = false;
static StaticSemaphore_t s_shared_reset_storage;
static SemaphoreHandle_t s_shared_reset_mu;
static portMUX_TYPE s_shared_reset_init = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t shared_reset_mutex(void) {
    portENTER_CRITICAL(&s_shared_reset_init);
    if (s_shared_reset_mu == NULL) {
        s_shared_reset_mu = xSemaphoreCreateMutexStatic(&s_shared_reset_storage);
    }
    portEXIT_CRITICAL(&s_shared_reset_init);
    return s_shared_reset_mu;
}

esp_err_t epd_board_metalio_eink4_plus_camera_acquire(uint32_t timeout_ms) {
    SemaphoreHandle_t mu = shared_reset_mutex();
    if (xSemaphoreTake(mu, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE("DvpCameraPower", "camera power: shared P12 reset lock timeout (%u ms)", (unsigned)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    uint8_t p0 = 0, p1 = 0;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_ioexp_ok) {
        err = pca9555_read_input_ex(I2C_NUM_1, 0, &p0);
        if (err != ESP_OK) {
            ESP_LOGE("DvpCameraPower", "camera power: TCA @0x%02x Port0 read failed: %s (0x%x)",
                     pca9555_get_addr(), esp_err_to_name(err), (unsigned)err);
        } else {
            ESP_LOGI("DvpCameraPower", "camera power: TCA @0x%02x P0=0x%02x P5(CAM/SCR_EN)=%u",
                     pca9555_get_addr(), p0, (unsigned)((p0 & CFG_PIN_CAM_SCR) != 0));
            err = pca9555_read_input_ex(I2C_NUM_1, 1, &p1);
            if (err != ESP_OK) {
                ESP_LOGE("DvpCameraPower", "camera reset: TCA Port1 read failed: %s (0x%x)",
                         esp_err_to_name(err), (unsigned)err);
            } else {
                ESP_LOGI("DvpCameraPower", "camera reset: P1=0x%02x P12(shared RST)=%u; no reset pulse",
                         p1, (unsigned)((p1 & CFG_PIN_TP_RST) != 0));
            }
        }
        if (err == ESP_OK && (!(p0 & CFG_PIN_CAM_SCR) || !(p1 & CFG_PIN_TP_RST))) {
            ESP_LOGE("DvpCameraPower", "camera power/reset: P5 or P12 is LOW; shared outputs left unchanged");
            err = ESP_ERR_INVALID_STATE;
        }
    } else {
        ESP_LOGE("DvpCameraPower", "camera power: TCA expander is not ready");
    }
    if (err != ESP_OK) xSemaphoreGive(mu);
    return err;
}

void epd_board_metalio_eink4_plus_camera_release(void) {
    xSemaphoreGive(shared_reset_mutex());
}
static bool s_tps_ok = false;
static bool s_lcd_bus_held = false;       // 入睡已拆 LCD 并置低总线脚
// Port1 锁存：P12/P14 默认高（上电），关机脉冲默认低
static uint8_t s_p1_out = (uint8_t)(CFG_PIN_TP_RST | CFG_PIN_BT_PA_PWR);
static uint8_t s_motor_on = 0;             // Port0 马达位锁存，0 或 CFG_PIN_MOTOR
static uint8_t s_pa_on = 0;                // Port0 PA_EN 锁存，0 或 CFG_PIN_PA_EN
static bool s_bt_reset_pa_off = false;     // P14 复位期间屏蔽所有 PA 开启请求
static uint8_t s_cam_scr_on = CFG_PIN_CAM_SCR; // Port0 P05 摄像/屏电，默认高

static uint8_t p0_output_value(void) {
    uint8_t value = 0x00;
    if (config_reg.pwrup) {
        value |= CFG_PIN_PWRUP;
    }
    if (config_reg.vcom_ctrl) {
        value |= CFG_PIN_VCOM_CTRL;
    }
    if (config_reg.wakeup) {
        value |= CFG_PIN_WAKEUP;
    }
    value |= s_motor_on;
    if (!s_bt_reset_pa_off) value |= s_pa_on;
    value |= s_cam_scr_on;
    return value;
}

static void p0_write(void) {
    esp_err_t err = pca9555_set_value(config_reg.port, p0_output_value(), CFG_PORT_CTRL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "p0_write: %s", esp_err_to_name(err));
    }
}

static void p1_write(void) {
    esp_err_t err = pca9555_set_value(config_reg.port, s_p1_out, CFG_PORT_INT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "p1_write: %s", esp_err_to_name(err));
    }
}

bool epd_board_metalio_eink4_plus_tps_ok(void) {
    return s_tps_ok;
}

bool epd_board_metalio_eink4_plus_ioexp_ok(void) {
    return s_ioexp_ok;
}

void epd_board_metalio_eink4_plus_motor_set(bool on) {
    if (!s_ioexp_ok) {
        return;
    }
    s_motor_on = on ? CFG_PIN_MOTOR : 0;
    p0_write();
}

void epd_board_metalio_eink4_plus_pa_set(bool on) {
    if (!s_ioexp_ok) {
        return;
    }
    s_pa_on = on ? CFG_PIN_PA_EN : 0;
    p0_write();
}

esp_err_t epd_board_metalio_eink4_plus_bt_power_reset(void) {
    if (!s_ioexp_ok) return ESP_ERR_INVALID_STATE;
    s_bt_reset_pa_off = true;
    s_pa_on = 0;
    esp_err_t pa_err = pca9555_set_value(config_reg.port, p0_output_value(), CFG_PORT_CTRL);
    if (pa_err != ESP_OK) {
        s_bt_reset_pa_off = false;
        return pa_err;
    }
    s_p1_out &= (uint8_t)~CFG_PIN_BT_PA_PWR;
    esp_err_t low_err = pca9555_set_value(config_reg.port, s_p1_out, CFG_PORT_INT);
    if (low_err == ESP_OK) {
        ESP_LOGI(TAG, "BT reset: P14 low for 3000 ms");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    // 即使拉低失败也尝试恢复电源，并保持共享端口其他位。
    s_p1_out |= CFG_PIN_BT_PA_PWR;
    esp_err_t high_err = pca9555_set_value(config_reg.port, s_p1_out, CFG_PORT_INT);
    // 不恢复复位期间到达的 PA 开启请求，完成后仍保持低。
    s_pa_on = 0;
    s_bt_reset_pa_off = false;
    if (high_err != ESP_OK) return high_err;
    ESP_LOGI(TAG, "BT reset: P14 high");
    return low_err;
}

void epd_board_metalio_eink4_plus_ana_sw_set(bool high) {
    static bool s_gpio_ready = false;
    if (!s_gpio_ready) {
        gpio_reset_pin(ANA_SW_GPIO);
        gpio_set_direction(ANA_SW_GPIO, GPIO_MODE_OUTPUT);
        s_gpio_ready = true;
    }
    gpio_set_level(ANA_SW_GPIO, high ? 1 : 0);
}

void epd_board_metalio_eink4_plus_sleep_cut_rails(void) {
    if (!s_ioexp_ok) {
        return;
    }
    s_cam_scr_on = 0;
    p0_write();
    s_p1_out &= (uint8_t)~CFG_PIN_BT_PA_PWR;
    p1_write();
    ESP_LOGI(TAG, "sleep cut P5 then P14");
}

void epd_board_metalio_eink4_plus_sleep_restore_rails(void) {
    if (!s_ioexp_ok) {
        return;
    }
    s_p1_out |= CFG_PIN_BT_PA_PWR;
    p1_write();
    s_cam_scr_on = CFG_PIN_CAM_SCR;
    p0_write();
    ESP_LOGI(TAG, "sleep restore P14 then P5");
}

bool epd_board_metalio_eink4_plus_ioexp_get_level(int io) {
    static uint8_t s_last[2] = {0xFF, 0xFF}; // 读失败保持；默认当高
    if (!s_ioexp_ok || io < 0 || io > 15) {
        return true;
    }
    const int port = CFG_IO_PORT(io);
    uint8_t v = s_last[port];
    if (pca9555_read_input_ex(config_reg.port, port, &v) == ESP_OK) {
        s_last[port] = v;
    }
    return (s_last[port] & CFG_IO_BIT(io)) != 0;
}

bool epd_board_metalio_eink4_plus_ioexp_read_port(int port, uint8_t* out) {
    if (!s_ioexp_ok || out == NULL || port < 0 || port > 1) {
        return false;
    }
    return pca9555_read_input_ex(config_reg.port, port, out) == ESP_OK;
}

bool epd_board_metalio_eink4_plus_ioexp_irq_wait(uint32_t timeout_ms) {
    if (s_ioexp_irq_sem == NULL) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms == 0 ? 1 : timeout_ms));
        return false;
    }
    return xSemaphoreTake(s_ioexp_irq_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void epd_board_metalio_eink4_plus_gpio2_irq_subscribe(TaskHandle_t task) {
    s_gpio2_irq_task = task;
}

void epd_board_metalio_eink4_plus_tps_enter_sleep(void) {
    if (!s_ioexp_ok) {
        return;
    }
    // tip 的 poweroff 往往已把三脚拉低；SLEEP 过渡期勿再打 I2C，只补等待
    const bool already =
        !config_reg.vcom_ctrl && !config_reg.pwrup && !config_reg.wakeup;
    if (!already) {
        // 手册 8.4：PWRUP/VCOM↓ → WAKEUP↓ → 等 ≥100ms
        config_reg.vcom_ctrl = false;
        config_reg.pwrup = false;
        p0_write();
        vTaskDelay(pdMS_TO_TICKS(50));
        config_reg.wakeup = false;
        p0_write();
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_LOGI(TAG, "TPS SLEEP ready (already=%d)", already ? 1 : 0);
}

static const gpio_num_t k_epd_bus_pins[] = {
    D0, D1, D2, D3, D4, D5, D6, D7, CKH, LEH, EPD_OE, STH, STV, CKV, EPD_MODE, EPD_BORDER,
};

static void epd_ctrl_gpios_out_low(void) {
    gpio_reset_pin(EPD_OE);
    gpio_set_direction(EPD_OE, GPIO_MODE_OUTPUT);
    gpio_set_level(EPD_OE, 0);
    gpio_reset_pin(EPD_MODE);
    gpio_set_direction(EPD_MODE, GPIO_MODE_OUTPUT);
    gpio_set_level(EPD_MODE, 0);
    gpio_reset_pin(EPD_BORDER);
    gpio_set_direction(EPD_BORDER, GPIO_MODE_OUTPUT);
    gpio_set_level(EPD_BORDER, 0);
}

static void epd_lcd_reinit(void) {
    const EpdDisplay_t* display = epd_get_display();
    LcdEpdConfig_t config = {
        .pixel_clock = display->bus_speed * 1000 * 1000,
        .ckv_high_time = 60,
        .line_front_porch = 4,
        .le_high_time = 9,
        .bus_width = display->bus_width,
        .bus = lcd_config,
        .dummy_clocks_after_data = 10, // E0470A14-AF section 6.3.1.
        .white_border_pixels = DISPLAY_WHITE_BORDER_PIXELS,
    };
    epd_lcd_init(&config, display->width, display->height);
}

void epd_board_metalio_eink4_plus_bus_hold_low(void) {
    for (size_t i = 0; i < sizeof(k_epd_bus_pins) / sizeof(k_epd_bus_pins[0]); ++i) {
        gpio_hold_dis(k_epd_bus_pins[i]);
    }
    if (!s_lcd_bus_held) {
        epd_lcd_deinit();
        s_lcd_bus_held = true;
    }
    for (size_t i = 0; i < sizeof(k_epd_bus_pins) / sizeof(k_epd_bus_pins[0]); ++i) {
        const gpio_num_t pin = k_epd_bus_pins[i];
        gpio_reset_pin(pin);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        gpio_set_pull_mode(pin, GPIO_FLOATING);
        gpio_set_level(pin, 0);
    }
    ESP_LOGI(TAG, "EPD bus pins held low D0..D7/XCL/XLE/XOE/XSTL/SPV/CKV/MODE/BORDER");
}

void epd_board_metalio_eink4_plus_bus_latch_for_sleep(void) {
    for (size_t i = 0; i < sizeof(k_epd_bus_pins) / sizeof(k_epd_bus_pins[0]); ++i) {
        esp_err_t err = gpio_hold_en(k_epd_bus_pins[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "gpio_hold_en(%d): %s", (int)k_epd_bus_pins[i], esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "EPD bus pins latched for sleep");
}

void epd_board_metalio_eink4_plus_bus_restore(void) {
    if (!s_lcd_bus_held) {
        return;
    }
    for (size_t i = 0; i < sizeof(k_epd_bus_pins) / sizeof(k_epd_bus_pins[0]); ++i) {
        gpio_hold_dis(k_epd_bus_pins[i]);
    }
    epd_ctrl_gpios_out_low();
    epd_lcd_reinit();
    s_lcd_bus_held = false;
    ESP_LOGI(TAG, "EPD bus restored after sleep");
}

void epd_board_metalio_eink4_plus_boot_release_holds(void) {
    static bool peripherals_cleared = false; // app_main 与显示初始化均可调用，外设残留只清理一次
    if (!peripherals_cleared) {
        // S31 软件复位不复位外设；注册 GPIO 服务前先清除上次运行的中断与握手 DMA。
        const gpio_num_t irq_pins[] = {NT26_SRDY_PIN, CFG_INTR, TOUCH_INT_PIN, BOOT_BUTTON_GPIO};
        uint64_t retained = 0;
        for (size_t i = 0; i < sizeof(irq_pins) / sizeof(irq_pins[0]); ++i) {
            const gpio_num_t pin = irq_pins[i];
            if (GPIO.pin[pin].int_ena != 0) retained |= 1ULL << pin;
            ESP_ERROR_CHECK(gpio_intr_disable(pin));
            ESP_ERROR_CHECK(gpio_wakeup_disable(pin));
            ESP_ERROR_CHECK(gpio_set_intr_type(pin, GPIO_INTR_DISABLE));
            if (pin < 32) gpio_ll_clear_intr_status(&GPIO, 1U << pin);
            else gpio_ll_clear_intr_status_high(&GPIO, 1U << (pin - 32));
        }
        PERIPH_RCC_ATOMIC() {
            for (int i = 0; i < UHCI_LL_NUM; ++i) {
                uhci_ll_enable_bus_clock(i, true);
                uhci_ll_reset_register(i);
                uhci_ll_enable_bus_clock(i, false);
            }
        }
        if (retained != 0) {
            ESP_LOGW(TAG, "boot: cleared retained GPIO interrupt mask=0x%llx", (unsigned long long)retained);
        }
        peripherals_cleared = true;
    }
    // 深睡 gpio_hold 跨 CPU 复位仍有效；不解则并行脚锁死为低，TPS 升压易失败
    for (size_t i = 0; i < sizeof(k_epd_bus_pins) / sizeof(k_epd_bus_pins[0]); ++i) {
        gpio_hold_dis(k_epd_bus_pins[i]);
    }
    MetalioSd_ReleaseHold();
    gpio_hold_dis(CFG_SDA);
    gpio_hold_dis(CFG_SCL);
    gpio_hold_dis(FL_COOL_PIN);
    gpio_hold_dis(FL_WARM_PIN);
    s_lcd_bus_held = false;
    ESP_LOGI(TAG, "boot: released EPD/SD/I2C/FL gpio holds");
}

void epd_board_metalio_eink4_plus_ioexp_mask_for_sleep(void) {
    if (!s_ioexp_ok) {
        return;
    }
    // Port0：TPS 三位与 PWR_GOOD 输出低；Motor/PA/P5 保持当前锁存
    config_reg.pwrup = false;
    config_reg.vcom_ctrl = false;
    config_reg.wakeup = false;
    const uint8_t p0_out = p0_output_value();
    pca9555_set_value(config_reg.port, p0_out, CFG_PORT_CTRL);
    pca9555_set_config(config_reg.port, 0x00, CFG_PORT_CTRL);

    // Port1：P12/P14 锁存保留；音量/电源脚驱空闲高（勿驱低，醒后改输入会假出按下→松开）；P13 低
    const uint8_t rail = (uint8_t)(s_p1_out & CFG_P1_RAIL_MASK);
    const uint8_t keys_idle = (uint8_t)(CFG_IO_BIT(EPD_IO_VOL_UP) | CFG_IO_BIT(EPD_IO_VOL_DOWN) |
                                        CFG_IO_BIT(EPD_IO_POWER));
    s_p1_out = (uint8_t)((rail | keys_idle) & (uint8_t)~CFG_PIN_PWR_PULSE);
    pca9555_set_value(config_reg.port, s_p1_out, CFG_PORT_INT);
    pca9555_set_config(config_reg.port, 0x00, CFG_PORT_INT);

    uint8_t p0 = 0;
    uint8_t p1 = 0;
    pca9555_read_input_ex(config_reg.port, CFG_PORT_CTRL, &p0);
    pca9555_read_input_ex(config_reg.port, CFG_PORT_INT, &p1);
    ESP_LOGI(TAG, "sleep mask all-out TPS=low keys=idle-high INT_GPIO=%d P0=0x%02X P1=0x%02X",
             gpio_get_level(CFG_INTR), p0, p1);
}

bool epd_board_metalio_eink4_plus_ioexp_enable_power_wake_pin(void) {
    if (!s_ioexp_ok) {
        return false;
    }
    // P17 锁存已是高；改输入时同电平，尽量无假边沿
    if (pca9555_set_config(config_reg.port, CFG_IO_BIT(EPD_IO_POWER), CFG_PORT_INT) != ESP_OK) {
        ESP_LOGW(TAG, "enable P17 input fail");
        return false;
    }
    return true;
}

void epd_board_metalio_eink4_plus_ioexp_restore_keys(void) {
    if (!s_ioexp_ok) {
        return;
    }
    pca9555_set_value(config_reg.port, p0_output_value(), CFG_PORT_CTRL);
    pca9555_set_config(config_reg.port, CFG_P0_INPUT_MASK, CFG_PORT_CTRL);

    // 保留 P12/P14 锁存；清关机脉冲；按键脚改回输入（P16 仍为 RTC_INT 输入）
    s_p1_out = (uint8_t)((s_p1_out & CFG_P1_RAIL_MASK) & (uint8_t)~CFG_PIN_PWR_PULSE);
    pca9555_set_value(config_reg.port, s_p1_out, CFG_PORT_INT);
    pca9555_set_config(config_reg.port, CFG_P1_INPUT_MASK, CFG_PORT_INT);

    uint8_t p0 = 0;
    uint8_t p1 = 0;
    pca9555_read_input_ex(config_reg.port, CFG_PORT_CTRL, &p0);
    pca9555_read_input_ex(config_reg.port, CFG_PORT_INT, &p1);
    ESP_LOGI(TAG, "keys restore P0=0x%02X P1=0x%02X INT_GPIO=%d", p0, p1, gpio_get_level(CFG_INTR));
}

static void p1_pwr_pulse_set(bool high) {
    if (high) {
        s_p1_out |= CFG_PIN_PWR_PULSE;
    } else {
        s_p1_out &= (uint8_t)~CFG_PIN_PWR_PULSE;
    }
    p1_write();
}

void epd_board_metalio_eink4_plus_pwr_key_pulse_train(void) {
    if (!s_ioexp_ok) {
        ESP_LOGW(TAG, "pwr pulse skipped (no TCA)");
        return;
    }
    ESP_LOGI(TAG, "pwr pulse train P1%d(io=%d) x%d interval=%dms", EPD_IO_PWR_PULSE & 7,
             EPD_IO_PWR_PULSE, PWR_PULSE_COUNT, PWR_PULSE_INTERVAL_MS);
    for (int i = 0; i < PWR_PULSE_COUNT; ++i) {
        p1_pwr_pulse_set(true);
        vTaskDelay(pdMS_TO_TICKS(PWR_PULSE_INTERVAL_MS));
        p1_pwr_pulse_set(false);
        vTaskDelay(pdMS_TO_TICKS(PWR_PULSE_INTERVAL_MS));
    }
}

void epd_board_metalio_eink4_plus_tp_reset(void) {
    if (!s_ioexp_ok) {
        ESP_LOGW(TAG, "TP RST skipped (no TCA)");
        vTaskDelay(pdMS_TO_TICKS(300));
        return;
    }
    SemaphoreHandle_t mu = shared_reset_mutex();
    xSemaphoreTake(mu, portMAX_DELAY);
    s_p1_out &= (uint8_t)~CFG_PIN_TP_RST;
    pca9555_set_value(config_reg.port, s_p1_out, CFG_PORT_INT);
    vTaskDelay(pdMS_TO_TICKS(10)); // Trst ≥ 5ms
    s_p1_out |= CFG_PIN_TP_RST;
    pca9555_set_value(config_reg.port, s_p1_out, CFG_PORT_INT);
    vTaskDelay(pdMS_TO_TICKS(300)); // Trsi ≥ 300ms
    xSemaphoreGive(mu);
    ESP_LOGI(TAG, "TP RST pulse on TCA P1%d (io=%d)", EPD_IO_TP_RST & 7, EPD_IO_TP_RST);
}

static esp_err_t i2c_probe_addr(i2c_port_t port, uint8_t addr_7bit) {
    if (!metalio_i2c1_lock(50)) return ESP_ERR_TIMEOUT;
    const esp_err_t ret = metalio_i2c1_probe_locked(port, addr_7bit, 50);
    metalio_i2c1_unlock();
    return ret;
}

static void i2c_scan_log(i2c_port_t port) {
    // 只 probe：对每个 ACK 地址盲读 0x08/0x0F 会拖死 BQ27220 并触发 clear bus error
    ESP_LOGI(TAG, "I2C scan SDA=%d SCL=%d (probe only):", (int)CFG_SDA, (int)CFG_SCL);
    for (uint8_t addr = 0x08; addr < 0x78; ++addr) {
        if (i2c_probe_addr(port, addr) != ESP_OK) {
            continue;
        }
        ESP_LOGI(TAG, "  found 0x%02X", addr);
    }
}

/** @return true 找到并配置好扩展器 */
static bool probe_and_init_ioexp(i2c_port_t port) {
    static const uint8_t k_candidates[] = {0x20, 0x24, 0x21, 0x22, 0x23, 0x25, 0x26, 0x27};
    for (size_t i = 0; i < sizeof(k_candidates); ++i) {
        uint8_t addr = k_candidates[i];
        if (i2c_probe_addr(port, addr) != ESP_OK) {
            continue;
        }
        pca9555_set_addr(addr);
        ESP_LOGI(TAG, "TCA/PCA9555 ACK at 0x%02X", addr);

        /* Port0：VCOM/PWRUP/WAKEUP/Motor/PA/P05 输出，其余输入（含 PWR_GOOD） */
        if (pca9555_set_config(port, CFG_P0_INPUT_MASK, CFG_PORT_CTRL) != ESP_OK) {
            ESP_LOGW(TAG, "set_config port0 fail at 0x%02X", addr);
            continue;
        }
        s_cam_scr_on = CFG_PIN_CAM_SCR;
        s_motor_on = 0;
        s_pa_on = 0;
        if (pca9555_set_value(port, p0_output_value(), CFG_PORT_CTRL) != ESP_OK) {
            continue;
        }
        /* Port1：P12/P14 输出拉高、关机脉冲默认低；P16=RTC_INT 输入 */
        s_p1_out = (uint8_t)(CFG_PIN_TP_RST | CFG_PIN_BT_PA_PWR);
        if (pca9555_set_value(port, s_p1_out, CFG_PORT_INT) != ESP_OK) {
            continue;
        }
        if (pca9555_set_config(port, CFG_P1_INPUT_MASK, CFG_PORT_INT) != ESP_OK) {
            continue;
        }
        ESP_LOGI(TAG,
                 "TPS↔TCA: VCOM=P%d PWRUP=P%d WAKE=P%d PG=P%d | P5/P12/P14 out P16=RTC_IN | P0=0x%02X",
                 EPD_TPS_IO_VCOM_CTRL, EPD_TPS_IO_PWRUP, EPD_TPS_IO_WAKEUP, EPD_TPS_IO_PWR_GOOD,
                 CFG_P0_OUTPUT_MASK);
        return true;
    }
    return false;
}

static void epd_board_init(uint32_t epd_row_width) {
    (void)epd_row_width;
    epd_board_metalio_eink4_plus_boot_release_holds();

    esp_err_t err = metalio_i2c1_init(CFG_SDA, CFG_SCL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new I2C1 init: %s", esp_err_to_name(err));
        return;
    }

    config_reg.port = EPDIY_I2C_PORT;
    config_reg.pwrup = false;
    config_reg.vcom_ctrl = false;
    config_reg.wakeup = false;

    s_ioexp_ok = probe_and_init_ioexp(config_reg.port);
    // 深睡曾关 P14(VOUT)：TCA 锁存仍低，上面已拉高；等轨稳定再启 TPS
    if (s_ioexp_ok) {
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    epd_board_metalio_eink4_plus_ana_sw_set(true); // GPIO36 默认高（非虚拟 U 盘）
    i2c_scan_log(config_reg.port); // 启动即扫；此时 TPS 多半仍 SLEEP，可能只有 TCA
    if (!s_ioexp_ok) {
        ESP_LOGE(TAG, "TCA9555 not found; continue (panel power unavailable)");
    }

    gpio_reset_pin(CFG_INTR);
    gpio_set_direction(CFG_INTR, GPIO_MODE_INPUT);
    gpio_pullup_en(CFG_INTR);
    gpio_pulldown_dis(CFG_INTR);
    gpio_set_intr_type(CFG_INTR, GPIO_INTR_NEGEDGE);
    if (s_ioexp_irq_sem == NULL) {
        s_ioexp_irq_sem = xSemaphoreCreateBinary();
    }
    esp_err_t isr = gpio_install_isr_service(ESP_INTR_FLAG_EDGE);
    if (isr != ESP_OK && isr != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "gpio_install_isr_service: %s", esp_err_to_name(isr));
    } else {
        err = gpio_isr_handler_add(CFG_INTR, interrupt_handler, (void*)CFG_INTR);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "gpio_isr_handler_add: %s", esp_err_to_name(err));
        }
    }
    // 读口清 TCA INT，避免上电残留一直拉低
    if (s_ioexp_ok) {
        pca9555_read_input(config_reg.port, CFG_PORT_CTRL);
        pca9555_read_input(config_reg.port, CFG_PORT_INT);
    }

    epd_ctrl_gpios_out_low();
    epd_lcd_reinit();
    s_lcd_bus_held = false;
    ESP_LOGI(TAG, "init ok %dx%d bus=%d pclk=%dMHz I2C SDA=%d SCL=%d ioexp=%s addr=0x%02X",
             epd_get_display()->width, epd_get_display()->height, epd_get_display()->bus_width,
             epd_get_display()->bus_speed, CFG_SDA, CFG_SCL, s_ioexp_ok ? "ok" : "FAIL",
             pca9555_get_addr());
}

static void epd_board_deinit(void) {
    gpio_set_level(EPD_OE, 0);
    gpio_set_level(EPD_MODE, 0);
    gpio_set_direction(EPD_OE, GPIO_MODE_INPUT);
    gpio_set_direction(EPD_MODE, GPIO_MODE_INPUT);

    epd_lcd_deinit();

    if (s_ioexp_ok) {
        s_motor_on = 0;
        pca9555_set_config(config_reg.port, 0xFF, CFG_PORT_CTRL);
        pca9555_set_config(config_reg.port, 0xFF, CFG_PORT_INT);

        int tries = 0;
        while ((pca9555_read_input(config_reg.port, CFG_PORT_CTRL) & CFG_PIN_PWRGOOD) != 0) {
            if (tries >= 50) {
                ESP_LOGE(TAG, "TPS65185 shutdown timeout");
                break;
            }
            tries++;
            vTaskDelay(1);
        }

        vTaskDelay(50);
        pca9555_read_input(config_reg.port, CFG_PORT_CTRL);
        pca9555_read_input(config_reg.port, CFG_PORT_INT);
    }
    const esp_err_t i2c_err = metalio_i2c1_deinit();
    if (i2c_err != ESP_OK) ESP_LOGW(TAG, "new I2C1 deinit: %s", esp_err_to_name(i2c_err));
    gpio_isr_handler_remove(CFG_INTR);
}

static void epd_board_set_ctrl(epd_ctrl_state_t* state, const epd_ctrl_state_t* const mask) {
    if (mask->ep_output_enable) {
        gpio_set_level(EPD_OE, state->ep_output_enable ? 1 : 0);
    }
    if (mask->ep_mode) {
        gpio_set_level(EPD_MODE, state->ep_mode ? 1 : 0);
    }

    if (!s_ioexp_ok) {
        return;
    }
    if (mask->ep_output_enable || mask->ep_mode || mask->ep_stv) {
        esp_err_t err = pca9555_set_value(config_reg.port, p0_output_value(), CFG_PORT_CTRL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "pca9555_set_value: %s", esp_err_to_name(err));
        }
    }
}

/**
 * @brief 上电：PWRUP 保持低、WAKEUP 高进 STANDBY；读 REVID → 写 UPSEQ0 → 写 UPSEQ1；
 *        配置成功后拉高 PWRUP（上升沿启动升压）。GPIO PG 后再写 ENABLE/VCOM。
 */
static void epd_board_poweron_once(epd_ctrl_state_t* state) {
    s_tps_ok = false;
    if (!s_ioexp_ok) {
        ESP_LOGE(TAG, "poweron skipped: no IO expander");
        return;
    }
    epd_board_metalio_eink4_plus_bus_restore();
    // Restore the schematic's original VBORDER bias. GPIO37 HIGH in 2.0.67
    // did not resolve the reported flashing of addressable edge pixels.
    gpio_set_level(EPD_BORDER,0);
    epd_ctrl_state_t mask = {
        .ep_output_enable = true,
        .ep_mode = true,
        .ep_stv = true,
    };

    state->ep_output_enable = false;
    state->ep_stv = true;
    state->ep_mode = false;

    // 1) PWRUP 低 + WAKEUP 高 → STANDBY，I2C 可用
    config_reg.wakeup = true;
    config_reg.pwrup = false;
    config_reg.vcom_ctrl = false;
    epd_board_set_ctrl(state, &mask);
    // 深睡后 TPS 从 SLEEP 醒：多等一会再碰寄存器
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_err_t probe = ESP_FAIL;
    probe = tps_probe(config_reg.port);
    if (probe != ESP_OK) {
        ESP_LOGE(TAG, "TPS probe failed: %s", esp_err_to_name(probe));
        return;
    }

    // 2) 先读版本号
    uint8_t revid = 0;
    esp_err_t revid_err = tps_read_register_ex(config_reg.port, TPS_REG_REVID, &revid);
    ESP_LOGI(TAG, "STANDBY REVID(%s)=0x%02X probe=%s", esp_err_to_name(revid_err), revid,
             esp_err_to_name(probe));

    // 3) 配置升压时序；写失败交给完整上电重试。
    esp_err_t up0 = tps_write_register(config_reg.port, TPS_REG_UPSEQ0, 0xE4);
    esp_err_t up1 = tps_write_register(config_reg.port, TPS_REG_UPSEQ1, 0x55);
    ESP_LOGI(TAG, "UPSEQ0 write %s read=0x%02X; UPSEQ1 write %s read=0x%02X", esp_err_to_name(up0),
             tps_read_register(config_reg.port, TPS_REG_UPSEQ0), esp_err_to_name(up1),
             tps_read_register(config_reg.port, TPS_REG_UPSEQ1));

    if (revid_err != ESP_OK || up0 != ESP_OK || up1 != ESP_OK) {
        ESP_LOGE(TAG, "TPS standby configuration failed");
        return;
    }

    // PWRUP 拉高前：读 INT2(0x08) / PG(0x0F)
    uint8_t int2_pre = 0;
    uint8_t pg_pre = 0;
    esp_err_t e_int2_pre = tps_read_register_ex(config_reg.port, TPS_REG_INT2, &int2_pre);
    esp_err_t e_pg_pre = tps_read_register_ex(config_reg.port, TPS_REG_PG, &pg_pre);
    ESP_LOGI(TAG, "before PWRUP: INT2(%s)=0x%02X PG(%s)=0x%02X", esp_err_to_name(e_int2_pre),
             int2_pre, esp_err_to_name(e_pg_pre), pg_pre);

    // 4) 配置成功后拉高 PWRUP（上升沿）
    config_reg.pwrup = true;
    config_reg.vcom_ctrl = true;
    epd_board_set_ctrl(state, &mask);
    ESP_LOGI(TAG, "PWRUP high (after UPSEQ)");
    vTaskDelay(pdMS_TO_TICKS(50)); // 等升压启动片刻再采一次

    // PWRUP 拉高后：再读 INT2 / PG
    uint8_t int2_post = 0;
    uint8_t pg_post = 0;
    esp_err_t e_int2_post = tps_read_register_ex(config_reg.port, TPS_REG_INT2, &int2_post);
    esp_err_t e_pg_post = tps_read_register_ex(config_reg.port, TPS_REG_PG, &pg_post);
    ESP_LOGI(TAG, "after PWRUP+50ms: INT2(%s)=0x%02X PG(%s)=0x%02X", esp_err_to_name(e_int2_post),
             int2_post, esp_err_to_name(e_pg_post), pg_post);

    bool success = false;
    config_reg.pwrup = true;
    config_reg.vcom_ctrl = true;
    epd_board_set_ctrl(state, &mask);

    // demo：只等扩展器 GPIO PWR_GOOD，成功前不写 ENABLE
    for (int wait_ms = 0; wait_ms < 200; ++wait_ms) {
        uint8_t p0 = pca9555_read_input(config_reg.port, CFG_PORT_CTRL);
        if (p0 & CFG_PIN_PWRGOOD) {
            ESP_LOGI(TAG, "PWR_GOOD ok p0=0x%02X @%dms", p0, wait_ms);
            success = true;
            break;
        }
        if (wait_ms == 0 || (wait_ms % 50) == 49) {
            uint8_t en = (probe == ESP_OK) ? tps_read_register(config_reg.port, TPS_REG_ENABLE) : 0;
            uint8_t pg = (probe == ESP_OK) ? tps_read_register(config_reg.port, TPS_REG_PG) : 0;
            uint8_t int1 = (probe == ESP_OK) ? tps_read_register(config_reg.port, TPS_REG_INT1) : 0;
            uint8_t int2 = (probe == ESP_OK) ? tps_read_register(config_reg.port, TPS_REG_INT2) : 0;
            ESP_LOGI(TAG, "wait GPIO PG... p0=0x%02X ENABLE=0x%02X PG=0x%02X INT1=0x%02X INT2=0x%02X",
                     p0, en, pg, int1, int2);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (!success) {
        uint8_t p0 = pca9555_read_input(config_reg.port, CFG_PORT_CTRL);
        uint8_t en = (probe == ESP_OK) ? tps_read_register(config_reg.port, TPS_REG_ENABLE) : 0;
        uint8_t pg = (probe == ESP_OK) ? tps_read_register(config_reg.port, TPS_REG_PG) : 0;
        uint8_t int1 = (probe == ESP_OK) ? tps_read_register(config_reg.port, TPS_REG_INT1) : 0;
        uint8_t int2 = (probe == ESP_OK) ? tps_read_register(config_reg.port, TPS_REG_INT2) : 0;
        ESP_LOGE(TAG,
                 "TPS PowerOn failed; p0=0x%02X REVID=0x%02X ENABLE=0x%02X INT1=0x%02X INT2=0x%02X "
                 "PG=0x%02X (check VDD4.2V / L6 L7)",
                 p0, revid, en, int1, int2, pg);
        s_tps_ok = false;
        return;
    }

    // demo：GPIO PG 成功后再写寄存器
    if (probe == ESP_OK) {
        esp_err_t err = tps_write_register(config_reg.port, TPS_REG_ENABLE, 0x3F);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "tps ENABLE write: %s", esp_err_to_name(err));
            s_tps_ok = false;
            return;
        }
        const unsigned value = vcom / 10;
        err = tps_write_register(config_reg.port, 4, (value & 0x100) >> 8);
        if (err == ESP_OK) err = tps_write_register(config_reg.port, 3, value & 0xff);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "TPS VCOM write: %s", esp_err_to_name(err));
            return;
        }
    }

    state->ep_sth = true;
    epd_ctrl_state_t sth_mask = {.ep_sth = true};
    epd_board_set_ctrl(state, &sth_mask);

    int tries = 0;
    while (probe == ESP_OK && !((tps_read_register(config_reg.port, TPS_REG_PG) & 0xFA) == 0xFA)) {
        if (tries >= 200) {
            ESP_LOGE(TAG, "TPS PG register timeout");
            s_tps_ok = false;
            return;
        }
        tries++;
        vTaskDelay(1);
    }

    state->ep_output_enable = true;
    epd_ctrl_state_t out_mask = {.ep_output_enable = true};
    epd_board_set_ctrl(state, &out_mask);
    s_tps_ok = true;
    ESP_LOGI(TAG, "TPS PowerOn ok vcom=%d mV", vcom);
}

static void epd_board_poweroff(epd_ctrl_state_t* state);

static void epd_board_poweron(epd_ctrl_state_t* state) {
    if (!s_ioexp_ok) {
        s_tps_ok = false;
        ESP_LOGE(TAG, "poweron skipped: no IO expander");
        return;
    }
    // Initial attempt plus at most three complete power-cycle retries.
    for (int attempt = 0; attempt <= 3; ++attempt) {
        epd_board_poweron_once(state);
        if (s_tps_ok) return;
        epd_board_poweroff(state);
        if (attempt < 3) {
            ESP_LOGW(TAG, "TPS power-on retry %d/3", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    ESP_LOGE(TAG, "TPS power-on failed after 3 retries; restarting");
    esp_restart();
}

static void epd_board_measure_vcom(epd_ctrl_state_t* state) {
    (void)state;
    ESP_LOGW(TAG, "measure_vcom not implemented");
}

static void epd_board_poweroff(epd_ctrl_state_t* state) {
    epd_ctrl_state_t mask = {
        .ep_stv = true,
        .ep_output_enable = true,
        .ep_mode = true,
    };
    state->ep_stv = false;
    state->ep_output_enable = false;
    state->ep_mode = false;
    epd_board_set_ctrl(state, &mask);
    // E0470A14-AF 6.1: XOE LOW must precede VPOS/VNEG OFF by >=12us.
    esp_rom_delay_us(12);
    config_reg.vcom_ctrl = false;
    config_reg.pwrup = false;
    epd_board_set_ctrl(state, &mask);
    vTaskDelay(1);
    config_reg.wakeup = false;
    epd_board_set_ctrl(state, &mask);
}

static float epd_board_ambient_temperature(void) {
    return 25.0f;
}

static void set_vcom(int value) {
    vcom = value;
}

const EpdBoardDefinition epd_board_metalio_eink4_plus = {
    .init = epd_board_init,
    .deinit = epd_board_deinit,
    .set_ctrl = epd_board_set_ctrl,
    .poweron = epd_board_poweron,
    .poweroff = epd_board_poweroff,
    .measure_vcom = epd_board_measure_vcom,
    .get_temperature = epd_board_ambient_temperature,
    .set_vcom = set_vcom,
    .gpio_set_direction = NULL,
    .gpio_read = NULL,
    .gpio_write = NULL,
};
