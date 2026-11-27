// ESP32-S31 SDK compatibility backport and RTC sleep diagnostics.
// Keep IRAM/DRAM/RTC placement and low-level register ordering intact.
#include <esp_attr.h>
#include <esp_log.h>
#include <esp_private/regi2c_ctrl.h>
#include <esp_private/sleep_event.h>
#include <esp_rom_sys.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <hal/regi2c_ctrl_ll.h>
#include <hal/regi2c_impl.h>
#include <soc/hp_sys_clkrst_reg.h>
#include <soc/lp_clkrst_reg.h>
#include <soc/regi2c_bias.h>
#include "s31_sleep.h"

// Preserve existing log tags for serial tooling and previous diagnostic records.
#define TAG "PowerHw"

// Survives CPU/watchdog resets when RTC memory is retained; no SD writes while asleep.
struct SleepTrace {
    uint32_t magic;
    uint32_t cycle;
    uint32_t phase;
    uint32_t applied;
    uint32_t pll_div;
    uint32_t pll_ctrl;
    uint32_t analog;
    uint32_t bias;
};

static RTC_NOINIT_ATTR volatile SleepTrace s_sleep_trace;
static constexpr uint32_t kSleepTraceMagic = 0x534C5032;
static DRAM_ATTR bool s_sdk_sleep_active = false;
static DRAM_ATTR bool s_pmu_sleep_active = false;
extern "C" uint32_t __real_pmu_sleep_start(uint32_t, uint32_t, uint32_t, bool);

extern "C" uint32_t IRAM_ATTR __wrap_pmu_sleep_start(uint32_t wake, uint32_t reject, uint32_t mem, bool deep) {
    if (s_sleep_trace.cycle != 0) {
        s_sleep_trace.phase = 200;
    }
    s_pmu_sleep_active = !deep;
    const uint32_t result = __real_pmu_sleep_start(wake, reject, mem, deep);
    s_pmu_sleep_active = false;
    if (s_sleep_trace.cycle != 0) {
        s_sleep_trace.phase = 201;
    }
    return result;
}

extern "C" void __real_rtc_clk_mpll_configure(uint32_t, uint32_t, bool);

extern "C" void IRAM_ATTR __wrap_rtc_clk_mpll_configure(uint32_t xtal, uint32_t frequency, bool clock_owned) {
    if (!s_pmu_sleep_active || !s_sdk_sleep_active) {
        __real_rtc_clk_mpll_configure(xtal, frequency, clock_owned);
        return;
    }
    // Backport S31's upstream MPLL analog initialization before calibration:
    // https://github.com/espressif/esp-idf/commit/9726078b55430ef8d2ce4a460ebf21d889bb5c12
    // PMU runs with interrupts masked and the other CPU stalled, so use the
    // cache-safe low-level register path without acquiring a cross-CPU lock.
    const bool gate_was_enabled = regi2c_ctrl_ll_master_is_clock_enabled();
    s_sleep_trace.phase = 500;
    _regi2c_ctrl_ll_master_enable_clock(true);
    s_sleep_trace.bias = _regi2c_impl_read_mask(I2C_BIAS, 0, I2C_BIAS_DREG_1P1, 7, 4) |
                         (static_cast<uint32_t>(_regi2c_impl_read_mask(I2C_BIAS, 0, I2C_BIAS_DREG_1P1_PVT, 3, 0)) << 8);
    // PMU wake has overwritten the startup bias (observed 7/7). Reapply the
    // upstream 12/12 calibration prerequisite on every resume, not only boot.
    _regi2c_impl_write_mask(I2C_BIAS, 0, I2C_BIAS_DREG_1P1, 7, 4, 12);
    _regi2c_impl_write_mask(I2C_BIAS, 0, I2C_BIAS_DREG_1P1_PVT, 3, 0, 12);
    esp_rom_delay_us(10);
    constexpr uint8_t kMpllBlock = 0x0b;
    _regi2c_impl_write_mask(kMpllBlock, 0, 1, 1, 0, 3);  // IR_CAL_EXT_CAP
    _regi2c_impl_write_mask(kMpllBlock, 0, 1, 2, 2, 1);  // IR_CAL_ENX_CAP
    REG_SET_FIELD(LP_AONCLKRST_MSPI_DIV_REG, LP_AONCLKRST_MSPI_FB_DIV, 9);
    _regi2c_impl_write_mask(kMpllBlock, 0, 3, 5, 4, 3);  // DHREF
    _regi2c_impl_write_mask(kMpllBlock, 0, 1, 2, 2, 0);  // IR_CAL_ENX_CAP
    s_sleep_trace.pll_div = REG_READ(LP_AONCLKRST_MSPI_DIV_REG);
    s_sleep_trace.pll_ctrl = REG_READ(HP_SYS_CLKRST_ANA_PLL_CTRL0_REG);
    s_sleep_trace.analog =
        _regi2c_impl_read(kMpllBlock, 0, 1) | (static_cast<uint32_t>(_regi2c_impl_read(kMpllBlock, 0, 3)) << 8);
    s_sleep_trace.bias =
        s_sleep_trace.bias |
        (static_cast<uint32_t>(_regi2c_impl_read_mask(I2C_BIAS, 0, I2C_BIAS_DREG_1P1, 7, 4)) << 16) |
        (static_cast<uint32_t>(_regi2c_impl_read_mask(I2C_BIAS, 0, I2C_BIAS_DREG_1P1_PVT, 3, 0)) << 24);
    s_sleep_trace.phase = 501;
    __real_rtc_clk_mpll_configure(xtal, frequency, true);
    s_sleep_trace.phase = 502;
    _regi2c_ctrl_ll_master_enable_clock(gate_was_enabled);
}

extern "C" void __real_esp_sleep_execute_event_callbacks(esp_sleep_event_cb_index_t, void*);

extern "C" void IRAM_ATTR __wrap_esp_sleep_execute_event_callbacks(esp_sleep_event_cb_index_t event, void* arg) {
    // Cache may be disabled here: only touch RTC RAM, never log or allocate.
    if (s_sdk_sleep_active) {
        s_sleep_trace.phase = 100u + static_cast<uint32_t>(event);
    }
    __real_esp_sleep_execute_event_callbacks(event, arg);
}

static SleepTrace s_previous_sleep{};
static bool s_previous_sleep_valid = false;
static int s_reset_raw0 = 0, s_reset_raw1 = 0;

esp_err_t S31SleepInit() {
    // Match the startup bias prerequisite in Espressif commit
    // 9726078b55430ef8d2ce4a460ebf21d889bb5c12 together with its MPLL flow.
    REGI2C_WRITE_MASK(I2C_BIAS, I2C_BIAS_DREG_1P1, 12);
    REGI2C_WRITE_MASK(I2C_BIAS, I2C_BIAS_DREG_1P1_PVT, 12);
    ESP_LOGI(TAG, "S31 calibration startup bias: dreg=%u pvt=%u", REGI2C_READ_MASK(I2C_BIAS, I2C_BIAS_DREG_1P1),
             REGI2C_READ_MASK(I2C_BIAS, I2C_BIAS_DREG_1P1_PVT));
    ESP_LOGI(TAG, "S31 sleep MPLL restore: analog calibration init; watchdogs retained");
    return ESP_OK;
}

void S31SleepCaptureReset() {
    s_reset_raw0 = static_cast<int>(esp_rom_get_reset_reason(0));
    s_reset_raw1 = static_cast<int>(esp_rom_get_reset_reason(1));
    const bool valid = s_sleep_trace.magic == kSleepTraceMagic;
    s_previous_sleep_valid = valid;
    if (valid) {
        s_previous_sleep = {s_sleep_trace.magic,   s_sleep_trace.cycle,    s_sleep_trace.phase,  s_sleep_trace.applied,
                            s_sleep_trace.pll_div, s_sleep_trace.pll_ctrl, s_sleep_trace.analog, s_sleep_trace.bias};
    }
    ESP_LOGW(TAG, "boot reset_reason=%d previous_sleep: valid=%d cycle=%lu phase=%lu applied=0x%lx",
             static_cast<int>(esp_reset_reason()), valid ? 1 : 0,
             valid ? static_cast<unsigned long>(s_sleep_trace.cycle) : 0ul,
             valid ? static_cast<unsigned long>(s_sleep_trace.phase) : 0ul,
             valid ? static_cast<unsigned long>(s_sleep_trace.applied) : 0ul);
    s_sleep_trace.magic = kSleepTraceMagic;
    s_sleep_trace.cycle = 0;
    s_sleep_trace.phase = 0;
    s_sleep_trace.applied = 0;
    s_sleep_trace.pll_div = s_sleep_trace.pll_ctrl = s_sleep_trace.analog = s_sleep_trace.bias = 0;
}

void S31SleepReportReset() {
    // USB Serial/JTAG may miss app_main's first message during enumeration.
    // Replay the immutable previous-boot snapshot after 10, 30 and 60 seconds.
    static unsigned report = 0;
    constexpr int64_t at[] = {10000000, 30000000, 60000000};
    if (report >= 3 || esp_timer_get_time() < at[report]) {
        return;
    }
    ++report;
    ESP_LOGW(
        TAG,
        "boot reset replay[%u]: reset_reason=%d raw_cpu0=%d raw_cpu1=%d previous_sleep valid=%d cycle=%lu phase=%lu applied=0x%lx",
        report, static_cast<int>(esp_reset_reason()), s_reset_raw0, s_reset_raw1, s_previous_sleep_valid ? 1 : 0,
        static_cast<unsigned long>(s_previous_sleep.cycle), static_cast<unsigned long>(s_previous_sleep.phase),
        static_cast<unsigned long>(s_previous_sleep.applied));
    ESP_LOGW(TAG, "sleep regs replay: div=0x%lx ctrl=0x%lx analog=0x%lx bias=0x%lx",
             static_cast<unsigned long>(s_previous_sleep.pll_div),
             static_cast<unsigned long>(s_previous_sleep.pll_ctrl), static_cast<unsigned long>(s_previous_sleep.analog),
             static_cast<unsigned long>(s_previous_sleep.bias));
}

void S31SleepBeginCycle() {
    s_sleep_trace.cycle = s_sleep_trace.cycle + 1;
}

void S31SleepCheckpoint(S31SleepPhase phase, unsigned applied) {
    s_sleep_trace.applied = applied;
    s_sleep_trace.phase = static_cast<uint32_t>(phase);
    ESP_LOGI(TAG, "sleep trace cycle=%lu phase=%lu applied=0x%x", static_cast<unsigned long>(s_sleep_trace.cycle),
             static_cast<unsigned long>(phase), applied);
}

esp_err_t S31SleepLightSleep(unsigned applied) {
    s_sdk_sleep_active = true;
    const esp_err_t err = esp_light_sleep_start();
    // CPU retention may restore its continuation outside the PMU wrapper.
    // Never let the wake-only register workaround leak into normal clock use.
    s_pmu_sleep_active = false;
    s_sdk_sleep_active = false;
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    S31SleepCheckpoint(S31SleepPhase::AfterSleep, applied);  // CPU returned, before any I2C access
    esp_rom_printf("PowerHw: sleep returned cycle=%lu err=%d cause=%d before I2C\n",
                   static_cast<unsigned long>(s_sleep_trace.cycle), static_cast<int>(err), static_cast<int>(cause));
    return err;
}
