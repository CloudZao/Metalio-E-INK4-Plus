/**
 * Flash mmap 零拷贝示例（推荐长文）。
 *
 * 1. menuconfig / sdkconfig 使用分区表：
 *      CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/v2/16m_fontpack.csv"
 *    或 32m_fontpack.csv
 * 2. 烧录字体：
 *      parttool.py --port PORT write_partition --partition-name font_data \
 *          --input tools/epdfont/fonts.fontpack
 * 3. 将本文件与 font_loader.c 加入组件编译。
 */

#include "font_loader.h"

#include "esp_log.h"

static const char* TAG = "fontpack_flash";

void fontpack_flash_demo(void) {
    int rc = font_loader_init_flash(NULL); /* 默认分区 label = "font_data" */
    if (rc != FONT_LOADER_OK) {
        ESP_LOGE(TAG, "init_flash failed: %d", rc);
        return;
    }

    ESP_LOGI(TAG, "mmap base=%p size=%u (expect ~0x3Cxxxxxx on ESP32-S3)",
             (void*)font_loader_mmap_base(), (unsigned)font_loader_mmap_size());

    GlyphInfo g;
    if (get_glyph(U'你', 25, 2, &g)) {
        ESP_LOGI(TAG, "你: %ux%u adv=%u bmp=%u ptr=%p", g.width, g.height, g.advance, g.bmp_len,
                 (void*)g.bitmap);
        /* g.bitmap 指向 Flash mmap，直接 blit，勿 free / 勿在 deinit 后使用 */
    } else {
        ESP_LOGW(TAG, "miss, err=%d", font_loader_last_error());
    }

    /* 一段话：逐字零拷贝 */
    const uint32_t text[] = {U'我', U'爱', U'你'};
    int x = 10;
    for (unsigned i = 0; i < sizeof(text) / sizeof(text[0]); i++) {
        if (!get_glyph(text[i], 25, 2, &g)) {
            x += 25;
            continue;
        }
        /* your_blit_2bpp(x + g.x_offset, ..., g.width, g.height, g.bitmap); */
        x += g.advance;
    }

    font_loader_deinit();
}
