#pragma once
#include "app_api.h"
#include <stddef.h>
#include <stdint.h>

static const xz_app_api_t* app;
static int zh;
static char text[4096];
static unsigned text_len;

static int AppInit(int argc, char** argv) {
    if (argc != 1 || !argv || !argv[0]) return 0;
    app = (const xz_app_api_t*)argv[0];
    if (app->abi_version != XZ_APP_ABI_VERSION || app->struct_size < offsetof(xz_app_api_t, asset_context)) return 0;
    if (!app->label || !app->button || !app->set_text) return 0;
    zh = app->language && app->language[0] == 'z';
    return app->width >= 600 && app->height >= 1000;
}
static void ResetText(void) { text_len = 0; text[0] = 0; }
static void Add(const char* value) {
    if (!value) return;
    while (*value && text_len + 1 < sizeof(text)) text[text_len++] = *value++;
    text[text_len] = 0;
}
static void Number(uint32_t value) {
    char digits[11]; unsigned n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n) { char part[2] = {digits[--n], 0}; Add(part); }
}
static void Signed(int value) {
    if (value < 0) { Add("-"); Number(0u - (uint32_t)value); }
    else Number((uint32_t)value);
}
static void FloatText(float value) {
    if (!(value >= -2000000000.0f && value < 2000000000.0f)) { Add(zh ? "超出显示范围" : "Out of display range"); return; }
    if (value < 0) { Add("-"); value = -value; }
    unsigned whole = (unsigned)value;
    unsigned fraction = (unsigned)((value - whole) * 1000.0f + 0.5f);
    if (fraction >= 1000) { ++whole; fraction = 0; }
    Number(whole);
    if (fraction) {
        char digits[4] = {(char)('0' + fraction / 100), (char)('0' + fraction / 10 % 10), (char)('0' + fraction % 10), 0};
        unsigned n = 3; while (n && digits[n - 1] == '0') digits[--n] = 0;
        Add("."); Add(digits);
    }
}
static void* Label(int x, int y, int w, int h, const char* value) { return app->label(app->root, x, y, w, h, value); }
static void* Button(int x, int y, int w, int h, const char* value, xz_app_click_t click, uintptr_t key) {
    return app->button(app->root, x, y, w, h, value, click, (void*)key);
}

static void AppStyle(void* widget, unsigned size, unsigned align, unsigned border, unsigned primary) {
    if (!widget || app->struct_size < offsetof(xz_app_api_t,set_style)+sizeof(app->set_style) || !app->set_style) return;
    const xz_app_style_t style={(uint16_t)size,(uint8_t)align,12,(uint8_t)border,(uint8_t)primary};
    app->set_style(widget,&style);
}
static void AppEnabled(void* widget, int enabled) {
    if (widget && app->struct_size >= offsetof(xz_app_api_t,set_enabled)+sizeof(app->set_enabled) && app->set_enabled) app->set_enabled(widget,enabled);
}
