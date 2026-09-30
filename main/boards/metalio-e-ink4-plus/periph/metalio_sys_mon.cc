#include "metalio_sys_mon.h"

#include "bq27220_gauge.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

void MetalioPeriph_StartSystemMonitor() {
    xTaskCreate(
        [](void*) {
            constexpr int kCoreCount = portNUM_PROCESSORS;
            configRUN_TIME_COUNTER_TYPE prev_idle[kCoreCount] = {};
            for (int c = 0; c < kCoreCount; ++c) {
                prev_idle[c] = ulTaskGetIdleRunTimeCounterForCore(c);
            }
            uint64_t prev_us = static_cast<uint64_t>(esp_timer_get_time());
            uint64_t last_loop_end_us = 0;

            while (true) {
                vTaskDelay(pdMS_TO_TICKS(3000));

                const uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
                if (last_loop_end_us != 0) {
                    const uint64_t gap_us = now_us - last_loop_end_us;
                    // 期望 ~3000ms；明显偏大说明 sys_mon(pri=1) 长期得不到 CPU
                    if (gap_us > 7500000) {
                        ESP_LOGW("系统监控",
                                 "@@@调度  | sys_mon 实际间隔 %lu ms (期望 ~3000ms)",
                                 static_cast<unsigned long>(gap_us / 1000));
                    }
                }
                const uint64_t dt_us = now_us - prev_us;
                int usage[kCoreCount] = {};
                int total_usage = 0;
                if (dt_us > 0) {
                    for (int c = 0; c < kCoreCount; ++c) {
                        const configRUN_TIME_COUNTER_TYPE now_idle =
                            ulTaskGetIdleRunTimeCounterForCore(c);
                        const configRUN_TIME_COUNTER_TYPE didle = now_idle - prev_idle[c];
                        uint64_t idle_pct = static_cast<uint64_t>(didle) * 100ULL / dt_us;
                        if (idle_pct > 100) {
                            idle_pct = 100;
                        }
                        usage[c] = 100 - static_cast<int>(idle_pct);
                        total_usage += usage[c];
                        prev_idle[c] = now_idle;
                    }
                }
                prev_us = now_us;
                const int avg_usage = (kCoreCount > 0) ? (total_usage / kCoreCount) : 0;
                const int core1_usage = (kCoreCount > 1) ? usage[1] : 0;

                constexpr const char* kMonitorTag = "系统监控";
                ESP_LOGI(kMonitorTag, "@@@CPU   | 内核0: %3d%% | 内核1: %3d%% | 平均: %3d%%",
                         usage[0], core1_usage, avg_usage);

                const unsigned free_kb =
                    static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
                const unsigned min_free_kb = static_cast<unsigned>(
                    heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);
                const unsigned psram_free_kb =
                    static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
                const unsigned psram_min_kb = static_cast<unsigned>(
                    heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024);
                ESP_LOGI(kMonitorTag, "@@@RAM   | 剩余: %6u KB | 历史最小: %6u KB", free_kb,
                         min_free_kb);
                ESP_LOGI(kMonitorTag, "@@@PSRAM | 剩余: %6u KB | 历史最小: %6u KB",
                         psram_free_kb, psram_min_kb);

                auto& gauge = Bq27220Gauge::GetInstance();
                int battery_level = 0;
                bool charging = false;
                bool discharging = false;
                if (gauge.GetBatteryLevel(battery_level, charging, discharging)) {
                    uint16_t mv = 0;
                    if (gauge.GetVoltageMv(mv)) {
                        ESP_LOGI(kMonitorTag,
                                 "@@@电池  | 电量: %3d%% | 电压: %5u mV | "
                                 "充电: %s | 放电: %s",
                                 battery_level, mv, charging ? "是" : "否",
                                 discharging ? "是" : "否");
                    } else {
                        ESP_LOGI(kMonitorTag,
                                 "@@@电池  | 电量: %3d%% | 电压: 读取失败 | "
                                 "充电: %s | 放电: %s",
                                 battery_level, charging ? "是" : "否",
                                 discharging ? "是" : "否");
                    }
                }

                last_loop_end_us = static_cast<uint64_t>(esp_timer_get_time());
            }
        },
        "sys_mon", 4096, nullptr, 1, nullptr);
}
