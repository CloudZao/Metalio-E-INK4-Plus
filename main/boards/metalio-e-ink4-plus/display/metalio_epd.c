/**
 * @file metalio_epd.c
 * @brief Metalio-E-Ink4-Plus 并行墨水屏门面（EPDiy）
 */

#include "metalio_epd.h"

#include "config.h"
#include "epd_board_metalio_eink4_plus.h"
#include "epdiy.h"
#include "my_waveform.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#define TAG "MetalioEpd"

static EpdiyHighlevelState s_hl;
static bool s_ready = false;

bool MetalioEpd_Init(void) {
#if !EPD_VERBOSE_LOG
    // 刷屏路径默认静默；改 config.h 里 EPD_VERBOSE_LOG=1 再开
    esp_log_level_set("epdiy", ESP_LOG_WARN);
    esp_log_level_set("epd_path", ESP_LOG_WARN);
    esp_log_level_set("epd_lcd", ESP_LOG_WARN);
    esp_log_level_set("epd_eink4p", ESP_LOG_WARN);
    esp_log_level_set("epd", ESP_LOG_WARN);
    esp_log_level_set("epd_font24", ESP_LOG_WARN);
#endif
    ESP_LOGI(TAG, "EPDiy init ED047TC2_1216 VCOM=%d mV", EPD_VCOM_MV);

    const size_t free_int_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t free_dma_before =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    epd_init(&epd_board_metalio_eink4_plus, &ED047TC2_1216, EPD_LUT_1K);
    epd_set_vcom(EPD_VCOM_MV);
    // 与 ebook 一致：原生横屏 + 业务侧 logical_to_physical，勿用 INVERTED_PORTRAIT
    epd_set_rotation(EPD_ROT_LANDSCAPE);
    // MY_WAVEFORM：DU 完备；GC/GL 默认完整 48 相（MY_WAVEFORM_TRIM=1 才裁剪）
    my_waveform_init();
    s_hl = epd_hl_init(&MY_WAVEFORM);

    const size_t free_int_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t free_dma_after =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    const size_t used_int =
        free_int_before > free_int_after ? free_int_before - free_int_after : 0;
    const size_t used_dma =
        free_dma_before > free_dma_after ? free_dma_before - free_dma_after : 0;
    ESP_LOGI(TAG,
             "EPDiy DRAM base: INTERNAL used=%u B (%u KB) DMA-cap used=%u B (%u KB) | "
             "free INTERNAL %u→%u KB",
             (unsigned)used_int, (unsigned)(used_int / 1024), (unsigned)used_dma,
             (unsigned)(used_dma / 1024), (unsigned)(free_int_before / 1024),
             (unsigned)(free_int_after / 1024));

    ESP_LOGI(TAG,
             "path: rotation=LANDSCAPE waveform=MY_WAVEFORM modes=%d hl.front=%p VE_pack=REV",
             MY_WAVEFORM.num_modes, (void*)s_hl.front_fb);
    // TPS 就绪标志在 poweron 序列里置位；勿在上电前读 tps_ok
    epd_poweron();
    s_ready = epd_board_metalio_eink4_plus_tps_ok();
    if (!s_ready) {
        ESP_LOGE(TAG, "TPS not ready");
        return false;
    }
    epd_poweroff();
    ESP_LOGI(TAG, "ready native=%dx%d (UI logical 684x1216)", epd_width(), epd_height());
    return true;
}

EpdiyHighlevelState* MetalioEpd_Hl(void) {
    return &s_hl;
}

bool MetalioEpd_Ready(void) {
    return s_ready;
}
