/**
 * @file metalio_touch.c
 * @brief FT6336U：GPIO INT 下降沿唤醒 feed；按下期间 10ms 续采
 */

#include "metalio_touch.h"

#include "config.h"
#include "epd_board_metalio_eink4_plus.h"
#include "epd_font24.h"
#include "epdiy.h"
#include "metalio_i2c1.h"

#include <driver/gpio.h>
#include <driver/i2c.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#define TAG "metalio_touch"

#define FT_REG_TD_STATUS 0x02
#define FT_REG_CHIP_ID   0xA3
#define FT_REG_FW_VER    0xA6
#define FT_REG_INT_MODE  0xA4
#define FT_CHIP_ID_6336  0x64

#define FT_EVENT_DOWN    0
#define FT_EVENT_CONTACT 2

#define EPDIY_I2C_PORT I2C_NUM_1
#define I2C1_LOCK_MS   50
#define TOUCH_FEED_MS  10
#define TOUCH_FEED_PRIO 5
#define TOUCH_FEED_STACK 4096

static bool s_ok = false;
static SemaphoreHandle_t s_irq_sem = NULL;
static TaskHandle_t s_feed_task = NULL;
static metalio_touch_sample_cb_t s_sample_cb = NULL;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static metalio_touch_point_t s_ui_pt = {};
static bool s_ui_pressed = false;       // 芯片侧：屏内仍按着
static bool s_ui_seen = false;          // 本次按下是否已被 indev 读到
static bool s_replay_press = false;     // 短按整段错过：下一次 read 回放 PRESSED
static bool s_replay_release = false;   // 回放 PRESSED 后还需一次 RELEASED
static bool s_ui_suppress = false;
static volatile int s_feed_pause_depth = 0; // 待机 / 落墨嵌套暂停（ISR 只读）
static bool s_finger_down = false; // feed 侧：芯片报有触点

static esp_err_t ft_read(uint8_t reg, uint8_t* buf, size_t n) {
    if (n == 0) {
        return ESP_OK;
    }
    if (!metalio_i2c1_lock(I2C1_LOCK_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        metalio_i2c1_unlock();
        return ESP_ERR_NO_MEM;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TOUCH_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TOUCH_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    if (n > 1) {
        i2c_master_read(cmd, buf, n - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, buf + n - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(EPDIY_I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    metalio_i2c1_unlock();
    return ret;
}

static esp_err_t ft_write(uint8_t reg, uint8_t val) {
    if (!metalio_i2c1_lock(I2C1_LOCK_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        metalio_i2c1_unlock();
        return ESP_ERR_NO_MEM;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TOUCH_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(EPDIY_I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    metalio_i2c1_unlock();
    return ret;
}

static void map_to_logical(int raw_x, int raw_y, int* lx, int* ly) {
    int x = raw_x;
    int y = raw_y;
    if (raw_y >= EPD_LOGICAL_W && raw_x < EPD_LOGICAL_W) {
        x = raw_x;
        y = raw_y;
    } else if (raw_x >= EPD_LOGICAL_W) {
        x = EPD_LOGICAL_W - 1 - raw_y;
        y = raw_x;
    }
    if (x < 0) {
        x = 0;
    } else if (x >= EPD_LOGICAL_W) {
        x = EPD_LOGICAL_W - 1;
    }
    if (y < 0) {
        y = 0;
    } else if (y >= EPD_LOGICAL_H) {
        y = EPD_LOGICAL_H - 1;
    }
    *lx = x;
    *ly = y;
}

static void IRAM_ATTR touch_isr(void* /*arg*/) {
    if (s_feed_pause_depth > 0) {
        return;
    }
    BaseType_t hp = pdFALSE;
    if (s_irq_sem != NULL) {
        xSemaphoreGiveFromISR(s_irq_sem, &hp);
    }
    if (hp == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void clear_ui_edges_locked(void) {
    s_ui_pressed = false;
    s_ui_seen = false;
    s_replay_press = false;
    s_replay_release = false;
    s_finger_down = false;
}

/** 仅清 LVGL 边沿；保留 s_finger_down，否则盖板键每拍 suppress 会停采、松手收不到 */
static void clear_ui_press_locked(void) {
    s_ui_pressed = false;
    s_ui_seen = false;
    s_replay_press = false;
    s_replay_release = false;
}

static void publish_ui(bool pressed, int lx, int ly, int raw_x, int raw_y) {
    portENTER_CRITICAL(&s_mux);
    if (s_ui_suppress) {
        s_ui_pressed = false;
        s_ui_seen = false;
        // 盖板键占用：丢弃未完成的屏内回放，避免键抬起后误点 UI
        s_replay_press = false;
        s_replay_release = false;
    } else if (pressed) {
        s_ui_pt.x = lx;
        s_ui_pt.y = ly;
        s_ui_pt.raw_x = raw_x;
        s_ui_pt.raw_y = raw_y;
        if (!s_ui_pressed) {
            // 新按下：清掉上一拍残留回放，等本拍被读或抬起时再排队
            s_ui_seen = false;
            s_replay_press = false;
            s_replay_release = false;
        }
        s_ui_pressed = true;
    } else {
        // 抬起时若 indev 从未读到 PRESSED，排队回放 press→release
        if (s_ui_pressed && !s_ui_seen) {
            s_replay_press = true;
            s_replay_release = true;
        }
        s_ui_pressed = false;
        s_ui_seen = false;
    }
    portEXIT_CRITICAL(&s_mux);
}

static void feed_one_sample(void) {
    uint8_t buf[6] = {0};
    if (ft_read(FT_REG_TD_STATUS, buf, sizeof(buf)) != ESP_OK) {
        return;
    }
    const int n = buf[0] & 0x0F;
    if (n == 0 || n > 2) {
        if (s_finger_down) {
            s_finger_down = false;
            ESP_LOGI(TAG, "touch up");
            publish_ui(false, 0, 0, 0, 0);
            if (s_sample_cb != NULL) {
                s_sample_cb(0, 0, 0, 0, 0);
            }
        }
        return;
    }

    const int ev = (buf[1] >> 6) & 0x03;
    if (ev != FT_EVENT_DOWN && ev != FT_EVENT_CONTACT) {
        return;
    }

    const int raw_x = ((buf[1] & 0x0F) << 8) | buf[2];
    const int raw_y = ((buf[3] & 0x0F) << 8) | buf[4];
    int lx = 0;
    int ly = 0;
    map_to_logical(raw_x, raw_y, &lx, &ly);

    static int s_last_lx = -1;
    static int s_last_ly = -1;
    const bool edge = !s_finger_down;
    if (edge || lx != s_last_lx || ly != s_last_ly) {
        ESP_LOGI(TAG, "touch %s raw=(%d,%d) logical=(%d,%d)", edge ? "down" : "move", raw_x, raw_y,
                 lx, ly);
        s_last_lx = lx;
        s_last_ly = ly;
    }
    s_finger_down = true;

    if (s_sample_cb != NULL) {
        s_sample_cb(1, raw_x, raw_y, lx, ly);
    }
    // sample_cb 可能已 set_ui_suppress（盖板键）
    publish_ui(true, lx, ly, raw_x, raw_y);
}

static void touch_feed_task(void* /*arg*/) {
    ESP_LOGI(TAG, "touch_feed start prio=%d period=%dms (INT gpio%d)", TOUCH_FEED_PRIO, TOUCH_FEED_MS,
             (int)TOUCH_INT_PIN);
    for (;;) {
        if (s_feed_pause_depth > 0) {
            while (xSemaphoreTake(s_irq_sem, 0) == pdTRUE) {
            }
            vTaskDelay(pdMS_TO_TICKS(TOUCH_FEED_MS));
            continue;
        }
        // 空闲等 INT；按下后短周期续采（CONTACT / 松手）
        const TickType_t wait = s_finger_down ? pdMS_TO_TICKS(TOUCH_FEED_MS) : portMAX_DELAY;
        xSemaphoreTake(s_irq_sem, wait);
        if (s_feed_pause_depth > 0) {
            continue;
        }
        // 合并连抖边沿
        while (xSemaphoreTake(s_irq_sem, 0) == pdTRUE) {
        }
        feed_one_sample();
        // INT 仍低（手指按着）再补采，避免只靠超时
        if (gpio_get_level(TOUCH_INT_PIN) == 0) {
            feed_one_sample();
        }
    }
}

bool metalio_touch_init(void) {
    s_ok = false;
    s_finger_down = false;
    s_ui_pressed = false;
    s_ui_seen = false;
    s_replay_press = false;
    s_replay_release = false;
    s_ui_suppress = false;
    s_feed_pause_depth = 0;

    if (s_irq_sem == NULL) {
        s_irq_sem = xSemaphoreCreateBinary();
        if (s_irq_sem == NULL) {
            ESP_LOGE(TAG, "irq sem create fail");
            return false;
        }
    }

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << TOUCH_INT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);

    epd_board_metalio_eink4_plus_tp_reset();

    uint8_t chip = 0;
    uint8_t fw = 0;
    esp_err_t id_err = ft_read(FT_REG_CHIP_ID, &chip, 1);
    ft_read(FT_REG_FW_VER, &fw, 1);
    if (id_err != ESP_OK) {
        ESP_LOGE(TAG, "no ACK at 0x%02X INT=%d (%s)", TOUCH_I2C_ADDR,
                 gpio_get_level(TOUCH_INT_PIN), esp_err_to_name(id_err));
        return false;
    }
    // 0x00：按下期间 INT 保持低 → 下降沿起采，feed 续采至抬起
    ft_write(FT_REG_INT_MODE, 0x00);

    esp_err_t isr = gpio_install_isr_service(0);
    if (isr != ESP_OK && isr != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "gpio_install_isr_service: %s", esp_err_to_name(isr));
    }
    gpio_isr_handler_remove(TOUCH_INT_PIN);
    if (gpio_isr_handler_add(TOUCH_INT_PIN, touch_isr, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "isr_handler_add GPIO%d fail", (int)TOUCH_INT_PIN);
        return false;
    }

    if (s_feed_task == NULL) {
        if (xTaskCreate(touch_feed_task, "touch_feed", TOUCH_FEED_STACK, NULL, TOUCH_FEED_PRIO,
                        &s_feed_task) != pdPASS) {
            ESP_LOGE(TAG, "touch_feed create fail");
            gpio_isr_handler_remove(TOUCH_INT_PIN);
            return false;
        }
    }

    // 上电 INT 已低：踢一脚避免漏首点
    if (gpio_get_level(TOUCH_INT_PIN) == 0) {
        xSemaphoreGive(s_irq_sem);
    }

    s_ok = true;
    ESP_LOGI(TAG, "ok chip=0x%02X%s fw=0x%02X INT=GPIO%d level=%d mode=irq+feed", chip,
             chip == FT_CHIP_ID_6336 ? " (FT6336)" : "", fw, (int)TOUCH_INT_PIN,
             gpio_get_level(TOUCH_INT_PIN));
    return true;
}

void metalio_touch_set_sample_cb(metalio_touch_sample_cb_t cb) {
    s_sample_cb = cb;
}

void metalio_touch_set_ui_suppress(bool suppress) {
    portENTER_CRITICAL(&s_mux);
    s_ui_suppress = suppress;
    if (suppress) {
        clear_ui_press_locked();
    }
    portEXIT_CRITICAL(&s_mux);
}

void metalio_touch_set_feed_paused(bool paused) {
    bool now_paused = false;
    bool edge_resume = false;
    portENTER_CRITICAL(&s_mux);
    if (paused) {
        if (s_feed_pause_depth < 100) {
            s_feed_pause_depth += 1;
        }
        clear_ui_edges_locked();
    } else if (s_feed_pause_depth > 0) {
        s_feed_pause_depth -= 1;
        edge_resume = (s_feed_pause_depth == 0);
    }
    now_paused = (s_feed_pause_depth > 0);
    portEXIT_CRITICAL(&s_mux);
    if (edge_resume && s_irq_sem != NULL && gpio_get_level(TOUCH_INT_PIN) == 0) {
        xSemaphoreGive(s_irq_sem);
    }
    if ((paused && s_feed_pause_depth == 1) || edge_resume) {
        ESP_LOGI(TAG, "feed %s depth=%d", now_paused ? "paused" : "resumed", s_feed_pause_depth);
    }
}

bool metalio_touch_has_pending_edges(void) {
    if (!s_ok) {
        return false;
    }
    bool pending = false;
    portENTER_CRITICAL(&s_mux);
    pending = s_replay_press || s_replay_release;
    portEXIT_CRITICAL(&s_mux);
    return pending;
}

bool metalio_touch_read(metalio_touch_point_t* out) {
    if (!s_ok) {
        return false;
    }
    bool pressed = false;
    metalio_touch_point_t pt = {};
    portENTER_CRITICAL(&s_mux);
    if (s_replay_press) {
        // 刷屏期间错过的短按：先交付一次 PRESSED
        s_replay_press = false;
        pressed = true;
        pt = s_ui_pt;
    } else if (s_ui_pressed) {
        s_ui_seen = true;
        pressed = true;
        pt = s_ui_pt;
    } else if (s_replay_release) {
        // 回放的 RELEASED：本拍返回 false，清标志
        s_replay_release = false;
        pressed = false;
    } else {
        pressed = false;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!pressed) {
        return false;
    }
    if (out != NULL) {
        *out = pt;
    }
    return true;
}

bool metalio_touch_ok(void) {
    return s_ok;
}
