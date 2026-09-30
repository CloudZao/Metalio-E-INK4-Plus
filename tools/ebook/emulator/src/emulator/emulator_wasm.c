#include <emscripten.h>
#include <emscripten/html5.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "book_reader_emulator.h"

static lv_display_t* s_disp = NULL;
static lv_indev_t* s_indev = NULL;
static bool s_lvgl_ready = false;

#define EPD_W EPD_HOR_RES
#define EPD_H EPD_VER_RES

/** 全屏 RGB565 缓冲 */
static uint8_t s_draw_buf[EPD_W * EPD_H * 2];

#ifdef __EMSCRIPTEN__
EM_JS(void, js_canvas_blit_rgb565, (const uint16_t* buf, int w, int h), {
    const canvas = Module['canvas'] || document.getElementById('epd-canvas');
    if (!canvas) return;
    if (canvas.width !== w) canvas.width = w;
    if (canvas.height !== h) canvas.height = h;
    if (!Module._epdCtx) {
        Module._epdCtx = canvas.getContext('2d');
        Module._epdImg = Module._epdCtx.createImageData(w, h);
    }
    const ctx = Module._epdCtx;
    const img = Module._epdImg;
    if (img.width !== w || img.height !== h) {
        Module._epdImg = ctx.createImageData(w, h);
    }
    const out = Module._epdImg;
    const n = w * h;
    const heap8 = Module.HEAPU8;
    if (!heap8) return;
    const base = buf;
    const data = out.data;
    for (let i = 0; i < n; i++) {
        const j = base + (i << 1);
        const c = heap8[j] | (heap8[j + 1] << 8);
        let r = (c >> 11) & 0x1f;
        let g = (c >> 5) & 0x3f;
        let b = c & 0x1f;
        r = (r * 255 / 31) | 0;
        g = (g * 255 / 63) | 0;
        b = (b * 255 / 31) | 0;
        const y = (r * 30 + g * 59 + b * 11) / 100;
        const v = y >= 160 ? 239 : 17;
        const o = i << 2;
        data[o] = v;
        data[o + 1] = v > 100 ? 235 : 17;
        data[o + 2] = v > 100 ? 220 : 17;
        data[o + 3] = 255;
    }
    ctx.putImageData(out, 0, 0);
});
#endif

static void flush_cb(lv_display_t* d, const lv_area_t* area, uint8_t* px_map) {
    (void)area;
#ifdef __EMSCRIPTEN__
    js_canvas_blit_rgb565((const uint16_t*)px_map, EPD_W, EPD_H);
#else
    (void)px_map;
#endif
    lv_display_flush_ready(d);
}

static void pointer_cb(lv_indev_t* i, lv_indev_data_t* data) {
    (void)i;
    data->point.x = (lv_coord_t)EM_ASM_INT({ return Module._ptrX || 0; });
    data->point.y = (lv_coord_t)EM_ASM_INT({ return Module._ptrY || 0; });
    data->state = EM_ASM_INT({ return Module._ptrDown || 0; }) ? LV_INDEV_STATE_PRESSED
                                                                : LV_INDEV_STATE_RELEASED;
}

static void style_epaper_theme(void) {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xEFEBE0), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(0x111111), 0);
}

static void init_lvgl(void) {
    lv_init();
    s_disp = lv_display_create(EPD_W, EPD_H);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_buffers(s_disp, s_draw_buf, NULL, sizeof(s_draw_buf),
                           LV_DISPLAY_RENDER_MODE_FULL);
    style_epaper_theme();

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, pointer_cb);

    book_reader_init();
}

EMSCRIPTEN_KEEPALIVE
int emulator_init(void) {
    if (!s_lvgl_ready) {
        init_lvgl();
        s_lvgl_ready = true;
    }
    if (s_disp) {
        lv_refr_now(s_disp);
    }
    return 0;
}

/** 由 JS setInterval 驱动（约 15fps），避免 emscripten 主循环占满 CPU */
EMSCRIPTEN_KEEPALIVE
void emulator_tick(void) {
    static uint32_t last_ms = 0;
    const uint32_t now = (uint32_t)emscripten_get_now();
    if (last_ms > 0) {
        uint32_t dt = now - last_ms;
        if (dt > 0 && dt < 500) {
            lv_tick_inc(dt);
        }
    }
    last_ms = now;
    lv_timer_handler();
}

EMSCRIPTEN_KEEPALIVE
void emulator_refresh(void) {
    if (s_disp) {
        lv_refr_now(s_disp);
    }
}

EMSCRIPTEN_KEEPALIVE
void emulator_set_fontpack(const char* path) {
    (void)path;
}

EMSCRIPTEN_KEEPALIVE
void emulator_set_epdfont(const char* path) {
    book_reader_set_epdfont_path(path);
}

EMSCRIPTEN_KEEPALIVE
int emulator_open_ebook_path(const char* path) {
    const int ok = book_reader_open_ebook_path(path);
    if (ok && s_disp) {
        lv_refr_now(s_disp);
    }
    return ok;
}

EMSCRIPTEN_KEEPALIVE
int emulator_open_ebook_bytes(const uint8_t* data, int len) {
    const int ok = book_reader_open_ebook_mem(data, (size_t)len);
    if (ok && s_disp) {
        lv_refr_now(s_disp);
    }
    return ok;
}

EMSCRIPTEN_KEEPALIVE
void emulator_next_page(void) {
    book_reader_next_page();
    if (s_disp) {
        lv_refr_now(s_disp);
    }
}

EMSCRIPTEN_KEEPALIVE
void emulator_prev_page(void) {
    book_reader_prev_page();
    if (s_disp) {
        lv_refr_now(s_disp);
    }
}

EMSCRIPTEN_KEEPALIVE
void emulator_go_toc(int index) {
    book_reader_go_toc(index);
    if (s_disp) {
        lv_refr_now(s_disp);
    }
}

EMSCRIPTEN_KEEPALIVE
int emulator_toc_count(void) {
    return book_reader_toc_count();
}

EMSCRIPTEN_KEEPALIVE
const char* emulator_toc_title(int index) {
    return book_reader_toc_title(index);
}

EMSCRIPTEN_KEEPALIVE
int emulator_current_toc_index(void) {
    return book_reader_current_toc_index();
}

EMSCRIPTEN_KEEPALIVE
const char* emulator_last_error(void) {
    return book_reader_last_error();
}

int main(void) {
    return 0;
}
