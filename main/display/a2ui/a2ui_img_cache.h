#pragma once

/**
 * @file a2ui_img_cache.h
 * @brief Session-scoped A2I1 blob cache on SD card (optional).
 *
 * Architecture
 * ------------
 * - Ownership: storage only (dir setup / boot wipe / keyed blob IO). Display & HTTP
 *   stay in a2ui_image.cc — this module never touches LVGL or the network.
 * - Backend: FatFS under `SD_PATH_A2UI_CACHE` (`/sdcard/metalio/e-ink/a2ui_cache`).
 *   No SD → cache disabled (HTTP-only). Has SD → boot wipe then ready.
 * - Key: FNV-1a of the *resolved* image URL (same string a2ui_image downloads).
 * - Capacity: soft budget + file-count; if the next image cannot fit, wipe the whole
 *   cache directory then write (no piecemeal eviction).
 * - Failure policy: never fatal to boot or UI.
 *
 * Threading: mutex-protected. Typical caller is the single a2ui_img download task.
 */

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sd_paths.h"

#ifdef __cplusplus
extern "C" {
#endif

/** POSIX directory for A2UI image session cache (under SD app root). */
#define A2UI_IMG_CACHE_BASE_PATH SD_PATH_A2UI_CACHE

/**
 * If SD is mounted: ensure cache dirs exist, wipe previous session files, mark ready.
 * If no SD: returns ESP_ERR_NOT_FOUND and leaves cache disabled.
 * Idempotent; never fatal to boot.
 */
esp_err_t a2ui_img_cache_init(void);

bool a2ui_img_cache_is_ready(void);

/**
 * Load cached blob for url into a heap buffer (SPIRAM preferred).
 * On success *out_buf is owned by caller (heap_caps_free / free).
 * ESP_ERR_NOT_FOUND = miss.
 */
esp_err_t a2ui_img_cache_load(const char *url, uint8_t **out_buf, size_t *out_len);

/**
 * Store a validated A2I1 payload for url. Best-effort.
 * If soft budget cannot hold this blob, wipe cache dir then write.
 */
esp_err_t a2ui_img_cache_store(const char *url, const uint8_t *data, size_t len);

/** Remove one entry (e.g. after parse failure on a hit). */
void a2ui_img_cache_invalidate(const char *url);

#ifdef __cplusplus
}
#endif
