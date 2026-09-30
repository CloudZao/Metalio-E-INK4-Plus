/**
 * @file metalio_keys.cc
 * @brief BOOT=GPIO 中断；音量/电源=TCA INT(GPIO2) 触发后读口
 */

#include "metalio_keys.h"

#include "config.h"
#include "epd_board_metalio_eink4_plus.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#define TAG "MetalioKeys"

namespace {

constexpr int64_t kDebounceUs = 30000;
constexpr int64_t kPowerLongUs = static_cast<int64_t>(POWER_LONG_PRESS_MS) * 1000LL;
constexpr int kQueueLen = 8;
constexpr uint32_t kWaitMs = 50; // 兼作 BOOT/长按轮询节拍

enum KeyId : int {
    kBoot = 0,
    kVolUp = 1,
    kVolDown = 2,
    kPower = 3,
    kCount = 4,
};

struct KeySlot {
    const char* label;
    int io; // <0：GPIO BOOT；≥0：TCA 脚号
    bool idle_high;
    bool prev_down;
    int64_t last_change_us;
};

KeySlot g_keys[kCount] = {
    {"BOOT", -1, true, false, 0},
    {"音量+", EPD_IO_VOL_UP, true, false, 0},
    {"音量-", EPD_IO_VOL_DOWN, true, false, 0},
    {"电源", EPD_IO_POWER, true, false, 0},
};

bool g_ready = false;
int64_t g_power_down_us = 0;
bool g_power_long_fired = false;
volatile bool g_shutdown_pulse_req = false;
QueueHandle_t g_evt_q = nullptr;
TaskHandle_t g_keys_task = nullptr;
uint8_t g_p1 = 0xFF;

bool level_high(const KeySlot& k) {
    if (k.io < 0) {
        return gpio_get_level(BOOT_BUTTON_GPIO) != 0;
    }
    return (g_p1 & (1u << (k.io & 7))) != 0;
}

bool is_down(const KeySlot& k) {
    return level_high(k) != k.idle_high;
}

void push_edge(void) {
    if (g_evt_q == nullptr) {
        return;
    }
    uint8_t one = 1;
    xQueueSend(g_evt_q, &one, 0);
}

void refresh_p1() {
    uint8_t p1 = g_p1;
    if (epd_board_metalio_eink4_plus_ioexp_read_port(1, &p1)) {
        g_p1 = p1;
    }
    uint8_t p0 = 0;
    epd_board_metalio_eink4_plus_ioexp_read_port(0, &p0); // 清 TCA INT
}

void apply_edge(KeySlot& k, bool down, int64_t now) {
    if (down == k.prev_down) {
        return;
    }
    if (now - k.last_change_us < kDebounceUs) {
        return;
    }
    k.last_change_us = now;
    k.prev_down = down;

    ESP_LOGI(TAG, "%s %s raw=%d", k.label, down ? "DOWN" : "UP", level_high(k) ? 1 : 0);

    if (&k == &g_keys[kPower]) {
        if (down) {
            g_power_down_us = now;
            g_power_long_fired = false;
        } else {
            g_power_down_us = 0;
            g_power_long_fired = false;
        }
    }
    push_edge();
}

void sample_all() {
    const int64_t now = esp_timer_get_time();
    refresh_p1();
    for (auto& k : g_keys) {
        apply_edge(k, is_down(k), now);
    }
    if (g_power_down_us != 0 && !g_power_long_fired && is_down(g_keys[kPower]) &&
        (now - g_power_down_us) >= kPowerLongUs) {
        g_power_long_fired = true;
        ESP_LOGI(TAG, "POWER long press %dms -> shutdown pulse", POWER_LONG_PRESS_MS);
        g_shutdown_pulse_req = true;
    }
}

void IRAM_ATTR boot_isr(void* /*arg*/) {
    BaseType_t hp = pdFALSE;
    if (g_keys_task != nullptr) {
        vTaskNotifyGiveFromISR(g_keys_task, &hp);
    }
    if (hp == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

void setup_boot_gpio() {
    gpio_reset_pin(BOOT_BUTTON_GPIO);
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << static_cast<unsigned>(BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io);
    gpio_isr_handler_add(BOOT_BUTTON_GPIO, boot_isr, nullptr);
}

void keys_task(void* /*arg*/) {
    g_keys_task = xTaskGetCurrentTaskHandle();
    while (true) {
        const bool irq = epd_board_metalio_eink4_plus_ioexp_irq_wait(kWaitMs);
        if (ulTaskNotifyTake(pdTRUE, 0) > 0 || irq) {
            while (epd_board_metalio_eink4_plus_ioexp_irq_wait(0)) {
            }
            while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
            }
        }
        sample_all();
    }
}

} // namespace

extern "C" void MetalioKeys_Init(void) {
    if (g_keys_task != nullptr) {
        return;
    }
    static bool s_boot_gpio_done = false;
    if (!s_boot_gpio_done) {
        setup_boot_gpio();
        s_boot_gpio_done = true;
    }

    refresh_p1();
    vTaskDelay(pdMS_TO_TICKS(20));
    refresh_p1();
    for (auto& k : g_keys) {
        // TCA 键硬件空闲为高；勿用「当前按下」当 idle
        k.idle_high = (k.io < 0) ? level_high(k) : true;
        k.prev_down = is_down(k);
        k.last_change_us = esp_timer_get_time();
    }
    g_power_down_us = 0;
    g_power_long_fired = false;
    g_shutdown_pulse_req = false;

    if (g_evt_q == nullptr) {
        g_evt_q = xQueueCreate(kQueueLen, sizeof(uint8_t));
    }
    g_ready = true;

    ESP_LOGI(TAG,
             "init IRQ: BOOT=GPIO%d TCA_INT=GPIO%d POWER=P17 VOL=P10/P11 pulse=P13 "
             "ioexp=%s idle B/P/V+/V-=%d/%d/%d/%d P1=0x%02X",
             BOOT_BUTTON_GPIO, IO_EXPANDER_INT_GPIO,
             epd_board_metalio_eink4_plus_ioexp_ok() ? "ok" : "FAIL", g_keys[kBoot].idle_high ? 1 : 0,
             g_keys[kPower].idle_high ? 1 : 0, g_keys[kVolUp].idle_high ? 1 : 0,
             g_keys[kVolDown].idle_high ? 1 : 0, g_p1);

    xTaskCreate(keys_task, "metalio_keys", 3072, nullptr, 5, &g_keys_task);
}

extern "C" void MetalioKeys_MaskForSleep(void) {
    // 先停扫描再驱脚：否则输出低会被 PollSideKeys 当成音量键按下，g_ready 变 false 后再「松开」→ 假 +10
    g_ready = false;
    epd_board_metalio_eink4_plus_ioexp_mask_for_sleep();
    ESP_LOGI(TAG, "ioexp ports masked for sleep");
}

extern "C" void MetalioKeys_ResumeAfterSleep(void) {
    epd_board_metalio_eink4_plus_ioexp_restore_keys();
    g_shutdown_pulse_req = false;
    g_power_down_us = 0;
    g_power_long_fired = false;

    // 输出→输入后上拉拉高需短暂稳定；稳定前保持 !ready，避免侧键轮询采到假沿
    vTaskDelay(pdMS_TO_TICKS(20));
    refresh_p1();
    for (auto& k : g_keys) {
        k.prev_down = is_down(k);
        k.last_change_us = esp_timer_get_time();
    }
    g_ready = true;
}

extern "C" bool MetalioKeys_IsReady(void) {
    return g_ready;
}

extern "C" bool MetalioKeys_TakeEdge(void) {
    if (!g_ready || g_evt_q == nullptr) {
        return false;
    }
    uint8_t one = 0;
    return xQueueReceive(g_evt_q, &one, 0) == pdTRUE;
}

extern "C" bool MetalioKeys_IsDown(int index) {
    if (index < 0 || index >= kCount || !g_ready) {
        return false;
    }
    // 待机量时长等路径要现读，勿只信边沿缓存
    if (g_keys[index].io >= 0) {
        refresh_p1();
    }
    return is_down(g_keys[index]);
}

extern "C" bool MetalioKeys_TakeShutdownPulse(void) {
    if (!g_shutdown_pulse_req) {
        return false;
    }
    g_shutdown_pulse_req = false;
    return true;
}
