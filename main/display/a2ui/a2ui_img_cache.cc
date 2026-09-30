#include "a2ui_img_cache.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "SdCardManager.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sd_paths.h"

static const char *TAG = "a2ui_img_cache";

/** On-disk envelope — raw A2I1 follows (already server-dithered). */
#define A2UI_IMG_CACHE_MAGIC   0x43493241u /* 'A2IC' LE */
#define A2UI_IMG_CACHE_VERSION 1u

/** Soft caps for one boot session (SD is large; keep cache bounded). */
#define A2UI_IMG_CACHE_MAX_BLOB        (128 * 1024)
#define A2UI_IMG_CACHE_MAX_FILES       24
#define A2UI_IMG_CACHE_MAX_TOTAL_BYTES (2 * 1024 * 1024)
#define A2UI_IMG_CACHE_PATH_MAX        96
#define A2UI_IMG_CACHE_FS_SLACK        4096u
#define A2UI_IMG_CACHE_WRITE_CHUNK     4096u

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t url_fnv;
    uint32_t payload_len;
} a2ui_img_cache_hdr_t;
#pragma pack(pop)

static SemaphoreHandle_t s_mu;
static bool s_ready;

static uint32_t fnv1a32(const char *s)
{
    uint32_t h = 2166136261u;
    if (!s) {
        return h;
    }
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(s); *p; ++p) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

static bool ensure_cache_tree(void)
{
    return SdEnsureAppLayout() && SdEnsureDir(A2UI_IMG_CACHE_BASE_PATH);
}

static void make_paths(uint32_t hash, char *final_path, size_t final_sz, char *tmp_path, size_t tmp_sz)
{
    snprintf(final_path, final_sz, "%s/%08lx.bin", A2UI_IMG_CACHE_BASE_PATH, (unsigned long)hash);
    if (tmp_path && tmp_sz) {
        snprintf(tmp_path, tmp_sz, "%s/%08lx.tmp", A2UI_IMG_CACHE_BASE_PATH, (unsigned long)hash);
    }
}

/** Join cache dir + filename; false if would not fit (avoids -Wformat-truncation). */
static bool join_cache_path(char *out, size_t out_sz, const char *name)
{
    if (!out || out_sz == 0 || !name || !name[0]) {
        return false;
    }
    constexpr size_t kBaseLen = sizeof(A2UI_IMG_CACHE_BASE_PATH) - 1;
    const size_t name_len = strlen(name);
    if (kBaseLen + 1 + name_len + 1 > out_sz) {
        return false;
    }
    memcpy(out, A2UI_IMG_CACHE_BASE_PATH, kBaseLen);
    out[kBaseLen] = '/';
    memcpy(out + kBaseLen + 1, name, name_len + 1);
    return true;
}

static void scan_cache_dir(int *out_files, size_t *out_bytes)
{
    int files = 0;
    size_t bytes = 0;
    DIR *d = opendir(A2UI_IMG_CACHE_BASE_PATH);
    if (!d) {
        if (out_files) {
            *out_files = 0;
        }
        if (out_bytes) {
            *out_bytes = 0;
        }
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        const char *name = e->d_name;
        if (!name || name[0] == '.') {
            continue;
        }
        size_t len = strlen(name);
        if (!(len > 4 && strcmp(name + len - 4, ".bin") == 0)) {
            continue;
        }
        char path[A2UI_IMG_CACHE_PATH_MAX];
        if (!join_cache_path(path, sizeof(path), name)) {
            continue;
        }
        struct stat st {};
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        files++;
        bytes += static_cast<size_t>(st.st_size);
    }
    closedir(d);
    if (out_files) {
        *out_files = files;
    }
    if (out_bytes) {
        *out_bytes = bytes;
    }
}

static void wipe_all_unlocked(void)
{
    DIR *d = opendir(A2UI_IMG_CACHE_BASE_PATH);
    if (!d) {
        return;
    }
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != nullptr) {
        const char *name = e->d_name;
        if (!name || name[0] == '.') {
            continue;
        }
        char path[A2UI_IMG_CACHE_PATH_MAX];
        if (!join_cache_path(path, sizeof(path), name)) {
            ESP_LOGW(TAG, "wipe skip oversized name");
            continue;
        }
        if (unlink(path) == 0) {
            n++;
        }
    }
    closedir(d);
    if (n > 0) {
        ESP_LOGW(TAG, "capacity: wiped %d file(s)", n);
    }
}

static size_t store_need_bytes(size_t payload_len)
{
    return sizeof(a2ui_img_cache_hdr_t) + payload_len + A2UI_IMG_CACHE_FS_SLACK;
}

/**
 * True if the next blob fits without wiping.
 * Soft budget = MAX_TOTAL_BYTES in cache dir + MAX_FILES
 * (newlib on ESP-IDF has no statvfs; SD ENOSPC → wipe+retry on write fail).
 */
static bool can_fit_unlocked(const char *final_path, size_t payload_len)
{
    struct stat st {};
    const bool exists = (stat(final_path, &st) == 0 && S_ISREG(st.st_mode));
    const size_t reclaim = (exists && st.st_size > 0) ? static_cast<size_t>(st.st_size) : 0;
    const size_t need = store_need_bytes(payload_len);

    int files = 0;
    size_t used = 0;
    scan_cache_dir(&files, &used);

    if (!exists && files >= A2UI_IMG_CACHE_MAX_FILES) {
        ESP_LOGI(TAG, "capacity: file count %d >= %d", files, A2UI_IMG_CACHE_MAX_FILES);
        return false;
    }

    const size_t after = (used > reclaim) ? (used - reclaim + need) : need;
    if (after > A2UI_IMG_CACHE_MAX_TOTAL_BYTES) {
        ESP_LOGI(TAG, "capacity: budget need after=%u > max=%u (used=%u)", (unsigned)after,
                 (unsigned)A2UI_IMG_CACHE_MAX_TOTAL_BYTES, (unsigned)used);
        return false;
    }
    return true;
}

esp_err_t a2ui_img_cache_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (!s_mu) {
        s_mu = xSemaphoreCreateMutex();
        if (!s_mu) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    if (s_ready) {
        goto out;
    }

    {
        auto &sd = SdCardManager::GetInstance();
        if (!sd.IsMounted()) {
            ESP_LOGW(TAG, "no SD mounted — A2UI image cache disabled");
            ret = ESP_ERR_NOT_FOUND;
            goto out;
        }
    }

    if (!ensure_cache_tree()) {
        ESP_LOGW(TAG, "cannot create %s — cache disabled", A2UI_IMG_CACHE_BASE_PATH);
        ret = ESP_FAIL;
        goto out;
    }

    wipe_all_unlocked(); /* boot: clear previous session */

    ESP_LOGI(TAG, "ready path=%s (boot wipe)", A2UI_IMG_CACHE_BASE_PATH);

    s_ready = true;
    ret = ESP_OK;

out:
    xSemaphoreGive(s_mu);
    return ret;
}

bool a2ui_img_cache_is_ready(void)
{
    return s_ready;
}

static esp_err_t load_unlocked(const char *url, uint8_t **out_buf, size_t *out_len)
{
    *out_buf = nullptr;
    *out_len = 0;

    if (!url || !url[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!SdCardManager::GetInstance().IsMounted()) {
        s_ready = false;
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t hash = fnv1a32(url);
    char path[A2UI_IMG_CACHE_PATH_MAX];
    make_paths(hash, path, sizeof(path), nullptr, 0);

    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    a2ui_img_cache_hdr_t hdr{};
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        unlink(path);
        return ESP_ERR_INVALID_SIZE;
    }
    if (hdr.magic != A2UI_IMG_CACHE_MAGIC || hdr.version != A2UI_IMG_CACHE_VERSION ||
        hdr.url_fnv != hash || hdr.payload_len == 0 || hdr.payload_len > A2UI_IMG_CACHE_MAX_BLOB) {
        fclose(f);
        unlink(path);
        ESP_LOGW(TAG, "reject corrupt %s", path);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *buf = static_cast<uint8_t *>(heap_caps_malloc(hdr.payload_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        buf = static_cast<uint8_t *>(malloc(hdr.payload_len));
    }
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    const size_t n = fread(buf, 1, hdr.payload_len, f);
    fclose(f);
    if (n != hdr.payload_len) {
        heap_caps_free(buf);
        unlink(path);
        return ESP_ERR_INVALID_SIZE;
    }

    *out_buf = buf;
    *out_len = hdr.payload_len;
    ESP_LOGI(TAG, "HIT hash=%08lx len=%u", (unsigned long)hash, (unsigned)hdr.payload_len);
    return ESP_OK;
}

esp_err_t a2ui_img_cache_load(const char *url, uint8_t **out_buf, size_t *out_len)
{
    if (!out_buf || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_buf = nullptr;
    *out_len = 0;

    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = load_unlocked(url, out_buf, out_len);
    xSemaphoreGive(s_mu);
    return err;
}

static esp_err_t write_file_unlocked(const char *final_path, const char *tmp_path, uint32_t hash,
                                     const uint8_t *data, size_t len)
{
    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        return ESP_FAIL;
    }

    a2ui_img_cache_hdr_t hdr = {
        .magic = A2UI_IMG_CACHE_MAGIC,
        .version = A2UI_IMG_CACHE_VERSION,
        .reserved = 0,
        .url_fnv = hash,
        .payload_len = static_cast<uint32_t>(len),
    };

    bool ok = (fwrite(&hdr, 1, sizeof(hdr), f) == sizeof(hdr));
    size_t off = 0;
    while (ok && off < len) {
        const size_t chunk =
            (len - off > A2UI_IMG_CACHE_WRITE_CHUNK) ? A2UI_IMG_CACHE_WRITE_CHUNK : (len - off);
        if (fwrite(data + off, 1, chunk, f) != chunk) {
            ok = false;
            break;
        }
        off += chunk;
        vTaskDelay(1);
    }
    if (ok) {
        ok = (fflush(f) == 0);
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        unlink(tmp_path);
        return ESP_FAIL;
    }

    unlink(final_path);
    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t store_unlocked(const char *url, const uint8_t *data, size_t len)
{
    if (!url || !url[0] || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > A2UI_IMG_CACHE_MAX_BLOB) {
        ESP_LOGW(TAG, "skip store: len %u > max %u", (unsigned)len, (unsigned)A2UI_IMG_CACHE_MAX_BLOB);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!SdCardManager::GetInstance().IsMounted()) {
        s_ready = false;
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t hash = fnv1a32(url);
    char final_path[A2UI_IMG_CACHE_PATH_MAX];
    char tmp_path[A2UI_IMG_CACHE_PATH_MAX];
    make_paths(hash, final_path, sizeof(final_path), tmp_path, sizeof(tmp_path));

    ESP_LOGI(TAG, "store begin hash=%08lx len=%u", (unsigned long)hash, (unsigned)len);

    if (!can_fit_unlocked(final_path, len)) {
        wipe_all_unlocked();
        vTaskDelay(1);
    }

    esp_err_t err = write_file_unlocked(final_path, tmp_path, hash, data, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "store write fail errno=%d — wipe and retry", errno);
        wipe_all_unlocked();
        vTaskDelay(1);
        err = write_file_unlocked(final_path, tmp_path, hash, data, len);
    }

    if (err == ESP_OK) {
        int files = 0;
        size_t used = 0;
        scan_cache_dir(&files, &used);
        ESP_LOGI(TAG, "STORE hash=%08lx len=%u files=%d used=%u", (unsigned long)hash, (unsigned)len,
                 files, (unsigned)used);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "store failed hash=%08lx len=%u errno=%d", (unsigned long)hash, (unsigned)len, errno);
    return ESP_ERR_NO_MEM;
}

esp_err_t a2ui_img_cache_store(const char *url, const uint8_t *data, size_t len)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = store_unlocked(url, data, len);
    xSemaphoreGive(s_mu);
    return err;
}

void a2ui_img_cache_invalidate(const char *url)
{
    if (!s_ready || !url || !url[0]) {
        return;
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return;
    }
    char path[A2UI_IMG_CACHE_PATH_MAX];
    char tmp[A2UI_IMG_CACHE_PATH_MAX];
    make_paths(fnv1a32(url), path, sizeof(path), tmp, sizeof(tmp));
    unlink(path);
    unlink(tmp);
    xSemaphoreGive(s_mu);
}
