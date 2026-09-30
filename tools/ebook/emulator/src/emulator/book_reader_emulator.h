#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** GDEM0397T81P 竖屏逻辑分辨率（与固件 ROTATE_270 后 LV_HOR×LV_VER 一致） */
#define EPD_HOR_RES 480
#define EPD_VER_RES 800

void book_reader_init(void);
void book_reader_set_fontpack_path(const char* path);
void book_reader_set_epdfont_path(const char* path);
int book_reader_open_ebook_path(const char* path);
int book_reader_open_ebook_mem(const uint8_t* data, size_t len);
void book_reader_next_page(void);
void book_reader_prev_page(void);
void book_reader_go_toc(int index);
int book_reader_toc_count(void);
const char* book_reader_toc_title(int index);
int book_reader_current_toc_index(void);
const char* book_reader_last_error(void);

#ifdef __cplusplus
}
#endif
