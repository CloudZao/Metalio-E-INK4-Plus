#include <stdint.h>
#include <string.h>

#include "../output_common/render_method.h"

#ifdef RENDER_METHOD_LCD

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <rom/cache.h>

#include "../epd_internals.h"
#include "../output_common/line_queue.h"
#include "../output_common/lut.h"
#include "../output_common/render_context.h"
#include "epd_board.h"
#include "epdiy.h"
#include "lcd_driver.h"
#include "render_lcd.h"

// declare vector optimized line mask application.
void epd_apply_line_mask_VE(uint8_t* line, const uint8_t* mask, int mask_len);

__attribute__((optimize("O3"))) static bool IRAM_ATTR
retrieve_line_isr(RenderContext_t* ctx, uint8_t* buf) {
    if (ctx->lines_consumed >= ctx->lines_total) {
        return false;
    }
    int thread = ctx->line_threads[ctx->lines_consumed];
    BaseType_t awoken = pdFALSE;

    if (thread >= NUM_RENDER_THREADS) {
        ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
        memset(buf, 0x00, ctx->display_width / 4);
        ctx->lines_consumed += 1;
        return awoken;
    }

    LineQueue_t* lq = &ctx->line_queues[thread];

    if (lq_read(lq, buf) != 0) {
        ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
        memset(buf, 0x00, ctx->display_width / 4);
    }

    if (ctx->lines_consumed >= ctx->display_height) {
        memset(buf, 0x00, ctx->display_width / 4);
    }
    ctx->lines_consumed += 1;
    return awoken;
}

/// start the next frame in the current update cycle
static void IRAM_ATTR handle_lcd_frame_done(RenderContext_t* ctx) {
    epd_lcd_frame_done_cb(NULL, NULL);
    epd_lcd_line_source_cb(NULL, NULL);

    // 扫描先于供数结束时置错，禁止残留行进入下一相位
    if (ctx->lines_consumed < ctx->lines_total) {
        ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
    }

    BaseType_t task_awoken = pdFALSE;
    xSemaphoreGiveFromISR(ctx->frame_done, &task_awoken);

    portYIELD_FROM_ISR();
}

// 纯扫约 11ms；TB8 相太短 DU/GL 易残影。流式路径保留，仅垫相周期下限。
#ifndef EPD_MIN_PHASE_US
#define EPD_MIN_PHASE_US 12000
#endif

static void pace_phase(int64_t phase_start_us) {
    for (;;) {
        if (esp_timer_get_time() - phase_start_us >= EPD_MIN_PHASE_US) {
            break;
        }
        vTaskDelay(0);
    }
}

void lcd_do_update(RenderContext_t* ctx) {
    epd_set_mode(1);
    ESP_LOGD("epd_lcd", "update %d frames, lines=%d (stream pace=%dus)", ctx->cycle_frames,
             ctx->lines_total, EPD_MIN_PHASE_US);

    for (uint8_t k = 0; k < ctx->cycle_frames; k++) {
        epd_lcd_frame_done_cb((frame_done_func_t)handle_lcd_frame_done, ctx);
        prepare_context_for_next_frame(ctx);

        const int64_t t0 = esp_timer_get_time();

        // start both feeder tasks；开扫由 feeder 预填够行后触发
        xTaskNotifyGive(ctx->feed_tasks[!xPortGetCoreID()]);
        xTaskNotifyGive(ctx->feed_tasks[xPortGetCoreID()]);

        if (xSemaphoreTake(ctx->frame_done, pdMS_TO_TICKS(15000)) != pdTRUE) {
            ESP_LOGE("epd_lcd", "LCD frame %d/%d timeout", k + 1, ctx->cycle_frames);
            ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
            epd_lcd_line_source_cb(NULL, NULL);
            epd_lcd_abort_frame();
            xSemaphoreTake(ctx->frame_done, 0);
        }

        for (int i = 0; i < NUM_RENDER_THREADS; i++) {
            if (xSemaphoreTake(ctx->feed_done_smphr[i], pdMS_TO_TICKS(15000)) != pdTRUE) {
                ESP_LOGE("epd_lcd", "feed %d timeout @frame %d", i, k);
                ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
            }
        }

        pace_phase(t0);

        const int64_t period_us = esp_timer_get_time() - t0;
        if ((k & 3) == 0 || k + 1 == ctx->cycle_frames) {
            ESP_LOGD("epd_lcd", "phase %d/%d period=%lldus", k + 1, ctx->cycle_frames,
                     (long long)period_us);
        }

        if (ctx->error) {
            for (int i = 0; i < NUM_RENDER_THREADS; i++) {
                lq_reset(&ctx->line_queues[i]);
            }
            break;
        }

        ctx->current_frame++;
    }

    epd_lcd_line_source_cb(NULL, NULL);
    epd_lcd_frame_done_cb(NULL, NULL);

    epd_set_mode(0);
}

__attribute__((optimize("O3"))) static bool IRAM_ATTR
push_pixels_isr(RenderContext_t* ctx, uint8_t* buf) {
    // Output no-op outside of drawn area
    if (ctx->lines_consumed < ctx->area.y) {
        memset(buf, 0, ctx->display_width / 4);
    } else if (ctx->lines_consumed >= ctx->area.y + ctx->area.height) {
        memset(buf, 0, ctx->display_width / 4);
    } else {
        memcpy(buf, ctx->static_line_buffer, ctx->display_width / 4);
    }
    ctx->lines_consumed += 1;
    return pdFALSE;
}

/**
 * Populate the line mask for use in epd_push_pixels.
 */
static void push_pixels_populate_line(RenderContext_t* ctx, int color) {
    // Select fill pattern by draw color
    int fill_byte = 0;
    switch (color) {
        case 0:
            fill_byte = DARK_BYTE;
            break;
        case 1:
            fill_byte = CLEAR_BYTE;
            break;
        default:
            fill_byte = 0x00;
    }

    // Compute a line mask based on the drawn area
    uint8_t* dirtyness = malloc(ctx->display_width / 2);
    assert(dirtyness != NULL);

    memset(dirtyness, 0, ctx->display_width / 2);

    for (int i = 0; i < ctx->display_width; i++) {
        if ((i >= ctx->area.x) && (i < ctx->area.x + ctx->area.width)) {
            dirtyness[i / 2] |= i % 2 ? 0xF0 : 0x0F;
        }
    }
    epd_populate_line_mask(ctx->line_mask, dirtyness, ctx->display_width / 4);

    // mask the line pattern with the populated mask
    memset(ctx->static_line_buffer, fill_byte, ctx->display_width / 4);
    epd_apply_line_mask(ctx->static_line_buffer, ctx->line_mask, ctx->display_width / 4);

    free(dirtyness);
}

void epd_push_pixels_lcd(RenderContext_t* ctx, short time, int color) {
    ctx->current_frame = 0;
    ctx->lines_total = ctx->display_height;
    ctx->lines_consumed = 0;
    ctx->static_line_buffer = malloc(ctx->display_width / 4);
    assert(ctx->static_line_buffer != NULL);

    push_pixels_populate_line(ctx, color);
    epd_lcd_frame_done_cb((frame_done_func_t)handle_lcd_frame_done, ctx);
    epd_lcd_line_source_cb((line_cb_func_t)&push_pixels_isr, ctx);

    epd_set_mode(1);
    epd_lcd_start_frame();
    xSemaphoreTake(ctx->frame_done, portMAX_DELAY);
    epd_set_mode(0);

    free(ctx->static_line_buffer);
    ctx->static_line_buffer = NULL;
}

#define int_min(a, b) (((a) < (b)) ? (a) : (b))

// 预填够 bounce 再开扫；太少易欠载，太多则短队列在开扫前会堵死
static int s_prefill_lines = 127;

__attribute__((optimize("O3"))) void IRAM_ATTR
lcd_calculate_frame(RenderContext_t* ctx, int thread_id) {
    assert(ctx->lut_lookup_func != NULL);
    uint8_t* input_line = ctx->feed_line_buffers[thread_id];

    LineQueue_t* lq = &ctx->line_queues[thread_id];
    int l = 0;

    // 另一生产者可能已启动并报告欠载；不得再次启动同一帧
    if (ctx->error) {
        return;
    }

    // line must be able to hold 2-pixel-per-byte or 1-pixel-per-byte data
    memset(input_line, 0x00, ctx->display_width);

    EpdRect area = ctx->area;
    int min_y, max_y, bytes_per_line, _ppB;
    const uint8_t* ptr_start;
    get_buffer_params(ctx, &bytes_per_line, &ptr_start, &min_y, &max_y, &_ppB);

    assert(area.width == ctx->display_width && area.x == 0 && !ctx->error);

    const int trigger_line = int_min(s_prefill_lines, max_y - min_y);

    while (l = atomic_fetch_add(&ctx->lines_prepared, 1), l < ctx->lines_total) {
        ctx->line_threads[l] = (uint8_t)thread_id;

        // 预填够行后开扫，随后边扫边喂
        if (l - min_y == trigger_line) {
            epd_lcd_line_source_cb((line_cb_func_t)&retrieve_line_isr, ctx);
            epd_lcd_start_frame();
        }

        if (l < min_y || l >= max_y
            || (ctx->drawn_lines != NULL && !ctx->drawn_lines[l - area.y])) {
            uint8_t* buf = NULL;
            int wait_iters = 0;
            while (buf == NULL) {
                if (ctx->error & EPD_DRAW_EMPTY_LINE_QUEUE) {
                    lq_reset(lq);
                    return;
                }
                buf = lq_current(lq);
                if (buf != NULL) {
                    break;
                }
                vTaskDelay(0);
                if (++wait_iters > 30000) {
                    ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
                    lq_reset(lq);
                    return;
                }
            }
            memset(buf, 0x00, lq->element_size);
            lq_commit(lq);
            continue;
        }

        const uint8_t* ptr = ptr_start + bytes_per_line * (l - min_y);
#if !CONFIG_IDF_TARGET_ESP32S31
        Cache_Start_DCache_Preload((uint32_t)ptr, ctx->display_width, 0);
#endif
        uint32_t* lp = (uint32_t*)ptr;

        uint8_t* buf = NULL;
        int wait_iters = 0;
        while (buf == NULL) {
            if (ctx->error & EPD_DRAW_EMPTY_LINE_QUEUE) {
                lq_reset(lq);
                return;
            }
            buf = lq_current(lq);
            if (buf != NULL) {
                break;
            }
            vTaskDelay(0);
            if (++wait_iters > 30000) {
                ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
                lq_reset(lq);
                return;
            }
        }

        ctx->lut_lookup_func(lp, buf, ctx->conversion_lut, ctx->display_width);
        epd_apply_line_mask_VE(buf, ctx->line_mask, ctx->display_width / 4);
        lq_commit(lq);
    }
}

#endif
