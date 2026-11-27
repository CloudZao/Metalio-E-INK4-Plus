/**
 * @file expander_button_input.cc
 * @brief BOOT=GPIO 中断；音量/电源=TCA INT(GPIO2) 触发后读口
 */

#include <atomic>
#include <mutex>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "expander_button_input.h"
#include "boards/common/display/epdiy/panel/epd_board_adapter.h"
#include "boards/common/input/input.h"
#include "config.h"
#include "infra/tasks/task_create.h"

#define TAG "ExpanderButtonInput"

static constexpr int64_t kDebounceUs = 30000;
static constexpr int64_t kPowerLongUs = static_cast<int64_t>(kPowerLongPressMs) * 1000LL;
static constexpr uint32_t kWaitMs = 50;  // 兼作 BOOT/长按轮询节拍

enum ExpanderKeyId : int {
    kBoot = 0,
    kVolUp = 1,
    kVolDown = 2,
    kPower = 3,
    kCount = 4,
};

struct ExpanderKeySlot {
    InputKey key;             // 公共逻辑按键
    const char* label;        // 诊断角色
    int io;                   // <0：GPIO BOOT；≥0：TCA 脚号
    bool idle_high;           // 空闲电平
    bool prev_down;           // 去抖后的按下状态
    int64_t last_change_us;   // 最近有效边沿
    int64_t down_us = 0;      // 本次按下起点，零表示启动或恢复时屏蔽
    int64_t click_us = 0;     // 等待合并单击或双击的松手时刻
    uint8_t clicks = 0;       // 等待分发的连续点击次数
    bool long_fired = false;  // 本次长按已交付或已屏蔽
};

static ExpanderKeySlot g_keys[kCount] = {
    {InputKeyBoot, "BOOT", -1, true, false, 0},
    {InputKeyVolumeUp, "音量+", EPD_IO_VOL_UP, true, false, 0},
    {InputKeyVolumeDown, "音量-", EPD_IO_VOL_DOWN, true, false, 0},
    {InputKeyPower, "电源", EPD_IO_POWER, true, false, 0},
};

static std::atomic<bool> g_ready{false};
static std::atomic<bool> g_power_click{false};
static int64_t g_power_down_us = 0;
static bool g_power_long_fired = false;
static std::atomic<bool> g_shutdown_pulse_req{false};
static std::mutex g_sample_mu;
static TaskHandle_t g_keys_task = nullptr;
static uint8_t g_p1 = 0xFF;
static bool g_sample_failed = false;  // 采样失败与恢复仅记录一次

static bool level_high(const ExpanderKeySlot& k) {
    if (k.io < 0) {
        return gpio_get_level(BOOT_BUTTON_GPIO) != 0;
    }
    return (g_p1 & (1u << (k.io & 7))) != 0;
}

static bool is_down(const ExpanderKeySlot& k) {
    return level_high(k) != k.idle_high;
}

static bool refresh_p1() {
    uint8_t p1 = g_p1;
    const bool p1_ok = epd_board_adapter_ioexp_read_port(1, &p1);
    if (p1_ok) {
        g_p1 = p1;
    }
    uint8_t p0 = 0;
    const bool p0_ok = epd_board_adapter_ioexp_read_port(0, &p0);  // 清 TCA INT
    return p1_ok && p0_ok;
}

static void latch_power_long_press(int64_t now) {
    if (g_power_down_us != 0 && !g_power_long_fired && now - g_power_down_us >= kPowerLongUs) {
        g_power_long_fired = true;
        ESP_LOGI(TAG, "POWER long press %dms", kPowerLongPressMs);
        g_shutdown_pulse_req = true;
    }
}

static void apply_edge(ExpanderKeySlot& k, bool down, int64_t now) {
    if (down == k.prev_down) {
        return;
    }
    if (now - k.last_change_us < kDebounceUs) {
        return;
    }
    k.last_change_us = now;
    k.prev_down = down;

    ESP_LOGD(TAG, "%s %s raw=%d", k.label, down ? "DOWN" : "UP", level_high(k) ? 1 : 0);

    if (&k == &g_keys[kPower]) {
        if (down) {
            g_power_down_us = now;
            InputPostEvent(InputKeyPower, InputPressDown);
            g_power_long_fired = false;
        } else {
            InputPostEvent(InputKeyPower, InputPressUp);
            latch_power_long_press(now);
            if (g_power_down_us != 0 && !g_power_long_fired) {
                g_power_click.store(true, std::memory_order_release);
            }
            g_power_down_us = 0;
            g_power_long_fired = false;
        }
    } else if (down) {
        k.down_us = now;
        k.long_fired = false;
        InputPostEvent(k.key, InputPressDown);
    } else {
        InputPostEvent(k.key, InputPressUp);
        if (!k.long_fired && k.down_us != 0) {
            if (k.clicks < 3) {
                ++k.clicks;
            }
            k.click_us = now;
        }
        k.down_us = 0;
    }
}

static void sample_all() {
    const int64_t now = esp_timer_get_time();
    const bool fresh = refresh_p1();
    if (!fresh && !g_sample_failed) {
        ESP_LOGW(TAG, "operation=sample stage=i2c result=unavailable; expander gestures cancelled");
        for (auto& key : g_keys) {
            if (key.io < 0) {
                continue;
            }
            key.down_us = 0;
            key.clicks = 0;
            key.long_fired = true;
            InputPostEvent(key.key, InputCancel);
        }
        g_power_down_us = 0;
        g_power_long_fired = true;
    }
    if (fresh && g_sample_failed) {
        ESP_LOGI(TAG, "operation=sample stage=i2c result=recovered");
    }
    g_sample_failed = !fresh;
    for (auto& k : g_keys) {
        if (k.io >= 0 && !fresh) {
            continue;
        }
        apply_edge(k, is_down(k), now);
        if (k.key == InputKeyPower) {
            continue;
        }
        if (k.prev_down && k.down_us != 0 && !k.long_fired && now - k.down_us >= int64_t(kBootLongPressMs) * 1000) {
            k.long_fired = true;
            k.clicks = 0;
            InputPostEvent(k.key, InputLongPress);
        }
        if (!k.prev_down && k.clicks && now - k.click_us >= int64_t(CONFIG_BUTTON_SHORT_PRESS_TIME_MS) * 1000) {
            if (k.clicks <= 2) {
                InputPostEvent(k.key, k.clicks == 1 ? InputClick : InputDoubleClick);
            }
            k.clicks = 0;
        }
    }
    if (fresh && is_down(g_keys[kPower])) {
        latch_power_long_press(now);
    }
}

static void IRAM_ATTR boot_isr(void* /*arg*/) {
    BaseType_t hp = pdFALSE;
    if (g_keys_task != nullptr) {
        vTaskNotifyGiveFromISR(g_keys_task, &hp);
    }
    if (hp == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void setup_boot_gpio() {
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

static void keys_task(void* /*arg*/) {
    g_keys_task = xTaskGetCurrentTaskHandle();
    while (true) {
        const bool irq = epd_board_adapter_ioexp_irq_wait(kWaitMs);
        if (ulTaskNotifyTake(pdTRUE, 0) > 0 || irq) {
            while (epd_board_adapter_ioexp_irq_wait(0)) {
            }
            while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
            }
        }
        std::lock_guard<std::mutex> lock(g_sample_mu);
        if (g_ready.load()) {
            sample_all();
        }
    }
}

extern "C" void ExpanderButtonInput_Init(void) {
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
        // 全部按键为低有效，启动时按住不能改变空闲极性。
        k.idle_high = true;
        k.prev_down = is_down(k);
        k.down_us = 0;
        k.click_us = 0;
        k.clicks = 0;
        k.long_fired = k.prev_down;
        k.last_change_us = esp_timer_get_time();
    }
    g_power_down_us = 0;
    g_power_long_fired = false;
    g_shutdown_pulse_req = false;

    ESP_LOGI(TAG,
             "init IRQ: BOOT=GPIO%d TCA_INT=GPIO%d POWER=P17 VOL=P10/P11 pulse=P13 "
             "ioexp=%s idle B/P/V+/V-=%d/%d/%d/%d P1=0x%02X",
             BOOT_BUTTON_GPIO, IO_EXPANDER_INT_GPIO, epd_board_adapter_ioexp_ok() ? "ok" : "FAIL",
             g_keys[kBoot].idle_high ? 1 : 0, g_keys[kPower].idle_high ? 1 : 0, g_keys[kVolUp].idle_high ? 1 : 0,
             g_keys[kVolDown].idle_high ? 1 : 0, g_p1);

    g_ready = true;
    if (FirmwareCreateTask(keys_task, "expander_keys", 3072, nullptr, 5, &g_keys_task, tskNO_AFFINITY, 0) != pdPASS) {
        g_ready = false;
        g_keys_task = nullptr;
        ESP_LOGE(TAG, "operation=init stage=task result=no_memory");
    }
}

extern "C" esp_err_t ExpanderButtonInput_MaskForSleep(void) {
    std::lock_guard<std::mutex> lock(g_sample_mu);
    // 先停扫描再驱脚，避免输出切换产生虚假的按键边沿。
    g_ready = false;
    const esp_err_t err = epd_board_adapter_ioexp_mask_for_sleep();
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "ioexp ports masked for sleep");
    return ESP_OK;
}

extern "C" esp_err_t ExpanderButtonInput_ResumeAfterSleep(void) {
    std::lock_guard<std::mutex> lock(g_sample_mu);
    g_ready = false;
    const esp_err_t err = epd_board_adapter_ioexp_restore_keys();
    if (err != ESP_OK) {
        return err;
    }
    g_shutdown_pulse_req = false;
    g_power_down_us = 0;
    g_power_long_fired = false;

    // 输出→输入后上拉拉高需短暂稳定；稳定前保持 !ready，避免侧键轮询采到假沿
    vTaskDelay(pdMS_TO_TICKS(20));
    if (!refresh_p1()) {
        return ESP_FAIL;
    }
    for (auto& k : g_keys) {
        k.prev_down = is_down(k);
        k.down_us = 0;
        k.click_us = 0;
        k.clicks = 0;
        k.long_fired = k.prev_down;
        k.last_change_us = esp_timer_get_time();
    }
    // 将后续 SD、串口和显示总线的恢复耗时计入本次电源键长按。
    if (g_keys[kPower].prev_down) {
        g_power_down_us = esp_timer_get_time();
    }
    g_ready = true;
    return ESP_OK;
}

extern "C" bool InputTakePowerClick(void) {
    return g_power_click.exchange(false, std::memory_order_acq_rel);
}

extern "C" bool ExpanderButtonInput_HasPowerClick(void) {
    return g_power_click.load(std::memory_order_acquire);
}

extern "C" bool ExpanderButtonInput_IsDown(int index) {
    std::lock_guard<std::mutex> lock(g_sample_mu);
    if (index < 0 || index >= kCount || !g_ready) {
        return false;
    }
    // 待机量时长等路径要现读，勿只信边沿缓存
    if (g_keys[index].io >= 0 && !refresh_p1()) {
        return false;
    }
    return is_down(g_keys[index]);
}

extern "C" bool InputTakeShutdownRequest(void) {
    return g_shutdown_pulse_req.exchange(false);
}

bool InputKeyIsDown(InputKey key) {
    switch (key) {
        case InputKeyBoot:
            return ExpanderButtonInput_IsDown(kBoot);
        case InputKeyPower:
            return ExpanderButtonInput_IsDown(kPower);
        case InputKeyVolumeUp:
            return ExpanderButtonInput_IsDown(kVolUp);
        case InputKeyVolumeDown:
            return ExpanderButtonInput_IsDown(kVolDown);
        default:
            return false;
    }
}
