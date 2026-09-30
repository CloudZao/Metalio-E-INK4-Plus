#include "assistant_chat_store.h"

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

static const char *TAG = "chat_store";

/** Daily-session caps: enough for tens of turns; keep RAM replay bounded. */
#define CHAT_MAX_RECORD       (24 * 1024)
#define CHAT_MAX_FILES        40
#define CHAT_MAX_TOTAL_BYTES  (192 * 1024)
#define CHAT_PATH_MAX         80
#define CHAT_WRITE_CHUNK      2048u
#define CHAT_NAME_MAX         16

static SemaphoreHandle_t s_mu;
static bool s_ready;
static uint32_t s_next_seq = 1;

static bool join_base(char *out, size_t out_sz, const char *name)
{
    if (!out || out_sz == 0 || !name || !name[0]) {
        return false;
    }
    constexpr size_t kBaseLen = sizeof(ASSISTANT_CHAT_STORE_BASE_PATH) - 1;
    const size_t name_len = strlen(name);
    if (kBaseLen + 1 + name_len + 1 > out_sz) {
        return false;
    }
    memcpy(out, ASSISTANT_CHAT_STORE_BASE_PATH, kBaseLen);
    out[kBaseLen] = '/';
    memcpy(out + kBaseLen + 1, name, name_len + 1);
    return true;
}

static bool name_for_seq(char *name, size_t name_sz, uint32_t seq, bool tmp)
{
    if (!name || name_sz < CHAT_NAME_MAX) {
        return false;
    }
    /* Fixed width: m00001.json / m00001.tmp — cannot overflow CHAT_NAME_MAX. */
    if (tmp) {
        snprintf(name, name_sz, "m%05lu.tmp", (unsigned long)seq);
    } else {
        snprintf(name, name_sz, "m%05lu.json", (unsigned long)seq);
    }
    return true;
}

static bool path_for_seq(char *out, size_t out_sz, uint32_t seq, bool tmp)
{
    char name[CHAT_NAME_MAX];
    if (!name_for_seq(name, sizeof(name), seq, tmp)) {
        return false;
    }
    return join_base(out, out_sz, name);
}

// mNNNNN.json → seq; 0 if not a record.
static uint32_t seq_from_name(const char *name)
{
    if (!name || name[0] != 'm') {
        return 0;
    }
    size_t len = strlen(name);
    if (len != 11 || strcmp(name + 6, ".json") != 0) {
        return 0;
    }
    uint32_t seq = 0;
    for (int i = 1; i <= 5; ++i) {
        const char c = name[i];
        if (c < '0' || c > '9') {
            return 0;
        }
        seq = seq * 10u + static_cast<uint32_t>(c - '0');
    }
    return seq == 0 ? 0 : seq;
}

static bool ensure_tree(void)
{
    return SdEnsureAppLayout() && SdEnsureDir(ASSISTANT_CHAT_STORE_BASE_PATH);
}

static void wipe_all_unlocked(void)
{
    DIR *d = opendir(ASSISTANT_CHAT_STORE_BASE_PATH);
    if (!d) {
        return;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        const char *name = e->d_name;
        if (!name || name[0] == '.') {
            continue;
        }
        char path[CHAT_PATH_MAX];
        if (!join_base(path, sizeof(path), name)) {
            ESP_LOGW(TAG, "wipe skip oversized name");
            continue;
        }
        if (unlink(path) == 0) {
            n++;
        }
    }
    closedir(d);
    s_next_seq = 1;
    if (n > 0) {
        ESP_LOGI(TAG, "wiped %d file(s) under %s", n, ASSISTANT_CHAT_STORE_BASE_PATH);
    } else {
        ESP_LOGI(TAG, "wipe: dir empty %s", ASSISTANT_CHAT_STORE_BASE_PATH);
    }
}

struct ScanOut {
    int files;
    size_t bytes;
    uint32_t min_seq;
    uint32_t max_seq;
};

static void scan_unlocked(ScanOut *out)
{
    ScanOut z{};
    z.min_seq = 0;
    DIR *d = opendir(ASSISTANT_CHAT_STORE_BASE_PATH);
    if (!d) {
        if (out) {
            *out = z;
        }
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        const uint32_t seq = seq_from_name(e->d_name);
        if (seq == 0) {
            continue;
        }
        char path[CHAT_PATH_MAX];
        if (!join_base(path, sizeof(path), e->d_name)) {
            continue;
        }
        struct stat st {};
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        z.files++;
        z.bytes += static_cast<size_t>(st.st_size);
        if (z.min_seq == 0 || seq < z.min_seq) {
            z.min_seq = seq;
        }
        if (seq > z.max_seq) {
            z.max_seq = seq;
        }
    }
    closedir(d);
    if (out) {
        *out = z;
    }
}

static bool drop_oldest_unlocked(void)
{
    ScanOut sc{};
    scan_unlocked(&sc);
    if (sc.files <= 0 || sc.min_seq == 0) {
        return false;
    }
    char path[CHAT_PATH_MAX];
    if (!path_for_seq(path, sizeof(path), sc.min_seq, false)) {
        return false;
    }
    if (unlink(path) != 0) {
        ESP_LOGW(TAG, "drop oldest seq=%lu failed errno=%d", (unsigned long)sc.min_seq, errno);
        return false;
    }
    ESP_LOGI(TAG, "drop oldest seq=%lu (files=%d used=%u)", (unsigned long)sc.min_seq, sc.files - 1,
             (unsigned)(sc.bytes));
    return true;
}

static void make_room_unlocked(size_t incoming)
{
    for (int i = 0; i < CHAT_MAX_FILES; ++i) {
        ScanOut sc{};
        scan_unlocked(&sc);
        const bool too_many = sc.files >= CHAT_MAX_FILES;
        const bool too_big = (sc.bytes + incoming) > CHAT_MAX_TOTAL_BYTES;
        if (!too_many && !too_big) {
            return;
        }
        ESP_LOGI(TAG, "budget files=%d used=%u incoming=%u — drop oldest", sc.files,
                 (unsigned)sc.bytes, (unsigned)incoming);
        if (!drop_oldest_unlocked()) {
            return;
        }
        vTaskDelay(1);
    }
}

static esp_err_t write_file_unlocked(const char *final_path, const char *tmp_path, const char *data,
                                     size_t len)
{
    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "fopen tmp fail errno=%d", errno);
        return ESP_FAIL;
    }
    bool ok = true;
    size_t off = 0;
    while (ok && off < len) {
        const size_t chunk = (len - off > CHAT_WRITE_CHUNK) ? CHAT_WRITE_CHUNK : (len - off);
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
        ESP_LOGW(TAG, "rename fail errno=%d", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t assistant_chat_store_init(void)
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

    if (!SdCardManager::GetInstance().IsMounted()) {
        ESP_LOGW(TAG, "no SD — chat history disabled (RAM-only)");
        ret = ESP_ERR_NOT_FOUND;
        goto out;
    }
    if (!ensure_tree()) {
        ESP_LOGW(TAG, "cannot create %s — chat history disabled", ASSISTANT_CHAT_STORE_BASE_PATH);
        ret = ESP_FAIL;
        goto out;
    }

    wipe_all_unlocked();
    s_next_seq = 1;
    s_ready = true;
    ESP_LOGI(TAG, "ready path=%s cap: %d files / %uB/file / %uB total (boot wipe)",
             ASSISTANT_CHAT_STORE_BASE_PATH, CHAT_MAX_FILES, (unsigned)CHAT_MAX_RECORD,
             (unsigned)CHAT_MAX_TOTAL_BYTES);
    ret = ESP_OK;

out:
    xSemaphoreGive(s_mu);
    return ret;
}

bool assistant_chat_store_is_ready(void)
{
    return s_ready;
}

esp_err_t assistant_chat_store_append(const char *json, size_t len)
{
    if (!json || !json[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        len = strlen(json);
    }
    if (len == 0 || len > CHAT_MAX_RECORD) {
        ESP_LOGW(TAG, "skip append: len=%u max=%u", (unsigned)len, (unsigned)CHAT_MAX_RECORD);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    if (!SdCardManager::GetInstance().IsMounted()) {
        s_ready = false;
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }

    make_room_unlocked(len);

    {
        uint32_t seq = s_next_seq;
        if (seq == 0 || seq > 99999u) {
            seq = 1;
        }
        char final_path[CHAT_PATH_MAX];
        char tmp_path[CHAT_PATH_MAX];
        if (!path_for_seq(final_path, sizeof(final_path), seq, false) ||
            !path_for_seq(tmp_path, sizeof(tmp_path), seq, true)) {
            err = ESP_ERR_NO_MEM;
            goto out;
        }
        err = write_file_unlocked(final_path, tmp_path, json, len);
        if (err == ESP_OK) {
            s_next_seq = seq + 1;
            ScanOut sc{};
            scan_unlocked(&sc);
            ESP_LOGI(TAG, "APPEND seq=%lu len=%u files=%d used=%u", (unsigned long)seq,
                     (unsigned)len, sc.files, (unsigned)sc.bytes);
        } else {
            ESP_LOGW(TAG, "append write fail seq=%lu len=%u", (unsigned long)seq, (unsigned)len);
        }
    }

out:
    xSemaphoreGive(s_mu);
    return err;
}

esp_err_t assistant_chat_store_wipe(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    wipe_all_unlocked();
    xSemaphoreGive(s_mu);
    return ESP_OK;
}

esp_err_t assistant_chat_store_for_each(assistant_chat_store_visit_fn fn, void *ctx)
{
    if (!fn) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(8000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    if (!SdCardManager::GetInstance().IsMounted()) {
        s_ready = false;
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }

    {
        uint32_t seqs[CHAT_MAX_FILES];
        int n = 0;
        DIR *d = opendir(ASSISTANT_CHAT_STORE_BASE_PATH);
        if (!d) {
            ESP_LOGI(TAG, "for_each: no dir");
            goto out;
        }
        struct dirent *e;
        while ((e = readdir(d)) != nullptr) {
            const uint32_t seq = seq_from_name(e->d_name);
            if (seq == 0) {
                continue;
            }
            if (n < CHAT_MAX_FILES) {
                seqs[n++] = seq;
            }
        }
        closedir(d);

        for (int i = 1; i < n; ++i) {
            uint32_t key = seqs[i];
            int j = i;
            while (j > 0 && seqs[j - 1] > key) {
                seqs[j] = seqs[j - 1];
                --j;
            }
            seqs[j] = key;
        }

        ESP_LOGI(TAG, "LOAD begin records=%d", n);
        int ok_n = 0;
        int skip_n = 0;
        for (int i = 0; i < n; ++i) {
            char path[CHAT_PATH_MAX];
            if (!path_for_seq(path, sizeof(path), seqs[i], false)) {
                skip_n++;
                continue;
            }
            struct stat st {};
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
                ESP_LOGW(TAG, "LOAD skip empty seq=%lu", (unsigned long)seqs[i]);
                skip_n++;
                continue;
            }
            if (static_cast<size_t>(st.st_size) > CHAT_MAX_RECORD) {
                ESP_LOGW(TAG, "LOAD skip oversized seq=%lu size=%ld", (unsigned long)seqs[i],
                         (long)st.st_size);
                skip_n++;
                continue;
            }
            const size_t sz = static_cast<size_t>(st.st_size);
            char *buf = static_cast<char *>(heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!buf) {
                buf = static_cast<char *>(malloc(sz + 1));
            }
            if (!buf) {
                ESP_LOGE(TAG, "LOAD OOM seq=%lu size=%u", (unsigned long)seqs[i], (unsigned)sz);
                skip_n++;
                continue;
            }
            FILE *f = fopen(path, "rb");
            if (!f) {
                heap_caps_free(buf);
                skip_n++;
                continue;
            }
            const size_t got = fread(buf, 1, sz, f);
            fclose(f);
            if (got != sz) {
                heap_caps_free(buf);
                ESP_LOGW(TAG, "LOAD short read seq=%lu", (unsigned long)seqs[i]);
                skip_n++;
                continue;
            }
            buf[sz] = '\0';
            const bool keep = fn(buf, sz, ctx);
            heap_caps_free(buf);
            ok_n++;
            vTaskDelay(1);
            if (!keep) {
                ESP_LOGW(TAG, "LOAD aborted by visitor after seq=%lu", (unsigned long)seqs[i]);
                break;
            }
        }
        ESP_LOGI(TAG, "LOAD done ok=%d skip=%d", ok_n, skip_n);
        if (n > 0) {
            s_next_seq = seqs[n - 1] + 1;
        }
    }

out:
    xSemaphoreGive(s_mu);
    return err;
}
