/**
 * @file epd_panel.c
 * @brief 并行墨水屏初始化门面（EPDiy）
 */

#include "epdiy.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "boards/common/display/epdiy/waveforms/my_waveform.h"
#include "epd_panel.h"
#include "config.h"
#include "boards/common/display/epdiy/panel/epd_board_adapter.h"

#define TAG "EpdPanel"

static EpdiyHighlevelState s_hl;
static bool s_ready = false;

bool EpdPanel_Init(void) {
#if !EPD_VERBOSE_LOG
    // 刷屏路径默认静默；改 config.h 里 EPD_VERBOSE_LOG=1 再开
    esp_log_level_set("epdiy", ESP_LOG_WARN);
    esp_log_level_set("epd_path", ESP_LOG_WARN);
    esp_log_level_set("epd_lcd", ESP_LOG_WARN);
    esp_log_level_set("epd_eink4p", ESP_LOG_WARN);
    esp_log_level_set("epd", ESP_LOG_WARN);
    esp_log_level_set("epd_font24", ESP_LOG_WARN);
#endif
    ESP_LOGI(TAG, "EPDiy init E0470A14-AF 34Pin VCOM=%d mV", EPD_VCOM_MV);

    const size_t free_int_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t free_dma_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    epd_init(&epd_board_adapter, &E0470A14_AF, EPD_LUT_1K);
    epd_set_vcom(EPD_VCOM_MV);
    // 与 ebook 一致：原生横屏 + 业务侧 logical_to_physical，勿用 INVERTED_PORTRAIT
    epd_set_rotation(EPD_ROT_LANDSCAPE);
    // 默认均衡完整 GC/GL；设置→屏幕可切换快速裁剪波形，DU 保持完整。
    my_waveform_init();
    s_hl = epd_hl_init(&MY_WAVEFORM);

    const size_t free_int_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t free_dma_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    const size_t used_int = free_int_before > free_int_after ? free_int_before - free_int_after : 0;
    const size_t used_dma = free_dma_before > free_dma_after ? free_dma_before - free_dma_after : 0;
    ESP_LOGI(TAG,
             "EPDiy DRAM base: INTERNAL used=%u B (%u KB) DMA-cap used=%u B (%u KB) | "
             "free INTERNAL %u→%u KB",
             (unsigned)used_int, (unsigned)(used_int / 1024), (unsigned)used_dma, (unsigned)(used_dma / 1024),
             (unsigned)(free_int_before / 1024), (unsigned)(free_int_after / 1024));

    ESP_LOGI(TAG, "path: rotation=LANDSCAPE waveform=MY_WAVEFORM modes=%d hl.front=%p VE_pack=REV",
             MY_WAVEFORM.num_modes, (void*)s_hl.front_fb);
    // TPS 就绪标志在 poweron 序列里置位；勿在上电前读 tps_ok
    epd_poweron();
    s_ready = epd_board_adapter_tps_ok();
    if (!s_ready) {
        ESP_LOGE(TAG, "TPS not ready");
        return false;
    }
    epd_poweroff();
    ESP_LOGI(TAG, "ready native=%dx%d (UI logical 684x1216)", epd_width(), epd_height());
    return true;
}

EpdiyHighlevelState* EpdPanel_GetHighlevelState(void) {
    return &s_hl;
}

bool EpdPanel_IsReady(void) {
    return s_ready;
}
