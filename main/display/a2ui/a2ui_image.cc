#include "a2ui_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <mutex>

#include "a2ui_img_cache.h"
#include "api_http.h"
#include "board.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "image_util.h"
#include "reader_types.h"

static const char *TAG = "a2ui_image";

/** Full-panel 800×480 I1 ≈ 48KB+hdr; default chat images are smaller. Cap peak. */
#define A2UI_IMG_MAX_DOWNLOAD (64 * 1024)
/** mem:// local blobs may be A2L8 (~440×280 L8 ≈ 123KB). */
#define A2UI_IMG_MAX_LOCAL    (400 * 1024)
#define A2UI_IMG_TASK_STACK   (12 * 1024)
#define A2UI_IMG_TASK_PRIO    4

#pragma pack(push, 1)
typedef struct {
    uint32_t magic; /* A2UI_I1_MAGIC */
    uint16_t width;
    uint16_t height;
    uint16_t stride;
    uint16_t reserved;
} a2ui_i1_hdr_t;

typedef struct {
    uint32_t magic; /* A2UI_L8_MAGIC */
    uint16_t width;
    uint16_t height;
} a2ui_l8_hdr_t;
#pragma pack(pop)

typedef struct a2ui_img_job {
    lv_obj_t *img_obj;
    char *url;
    struct a2ui_img_job *next;
} a2ui_img_job_t;

/** One displayed bitmap, owned for the lifetime of the matching lv_image. */
typedef struct {
    lv_obj_t *img_obj; /* NULL = free slot */
    uint8_t *buf;
    lv_image_dsc_t dsc;
} a2ui_img_surface_t;

/*
 * Architecture (multi-surface, single-flight FIFO):
 *
 *   LVGL thread                         worker task (at most one)
 *   -----------                         -------------------------
 *   load_async → alloc surface
 *             → enqueue FIFO
 *             → idle? xTaskCreate
 *   abort/clear → gen++, drain queue
 *   clear       → free all surfaces
 *
 *   worker: dequeue → cache/HTTP → apply to that surface → next
 *
 * Rules:
 * - Only LVGL thread calls load_async / clear (via a2ui_render under DisplayLock).
 * - s_worker_alive is the busy flag (not TaskHandle — create can return after
 *   the task already finished on the other core).
 * - gen++ only on abort/clear — never on enqueue (siblings must not cancel each other).
 * - clear(): destroy lv_image objs FIRST, then clear() — never free surface buf
 *   while an object still references its dsc.
 */
static a2ui_img_surface_t s_surfaces[A2UI_IMG_MAX_SURFACES];
static volatile uint32_t s_job_gen;
static lv_display_t *s_disp;
static char s_http_origin[96];

static a2ui_img_job_t *s_queue_head;
static a2ui_img_job_t *s_queue_tail;
static bool s_worker_alive;
static portMUX_TYPE s_job_mux = portMUX_INITIALIZER_UNLOCKED;

/** In-memory A2I1 blobs (mem:// / file:// preload). Survive abort/clear for page flip. */
#define A2UI_IMG_LOCAL_SLOTS 8
typedef struct {
    char url[128];
    uint8_t *data;
    size_t len;
} a2ui_img_local_t;
static a2ui_img_local_t s_local[A2UI_IMG_LOCAL_SLOTS];
static std::mutex s_local_mu;
static int s_local_rr = 0;

static void free_job(a2ui_img_job_t *job)
{
    if (!job) {
        return;
    }
    free(job->url);
    free(job);
}

static void drain_queue(void)
{
    portENTER_CRITICAL(&s_job_mux);
    a2ui_img_job_t *head = s_queue_head;
    s_queue_head = NULL;
    s_queue_tail = NULL;
    portEXIT_CRITICAL(&s_job_mux);

    while (head) {
        a2ui_img_job_t *next = head->next;
        free_job(head);
        head = next;
    }
}

/** Take head; if empty, clear s_worker_alive. */
static a2ui_img_job_t *dequeue_job_or_idle(void)
{
    a2ui_img_job_t *job = NULL;
    portENTER_CRITICAL(&s_job_mux);
    job = s_queue_head;
    if (job) {
        s_queue_head = job->next;
        if (s_queue_head == NULL) {
            s_queue_tail = NULL;
        }
        job->next = NULL;
    } else {
        s_worker_alive = false;
    }
    portEXIT_CRITICAL(&s_job_mux);
    return job;
}

static bool job_cancelled(uint32_t gen)
{
    return gen != s_job_gen;
}

static a2ui_img_surface_t *surface_find(lv_obj_t *img_obj)
{
    if (!img_obj) {
        return NULL;
    }
    for (int i = 0; i < A2UI_IMG_MAX_SURFACES; i++) {
        if (s_surfaces[i].img_obj == img_obj) {
            return &s_surfaces[i];
        }
    }
    return NULL;
}

/** Bind a free slot to img_obj (or return existing). LVGL thread only. */
static a2ui_img_surface_t *surface_get_or_alloc(lv_obj_t *img_obj)
{
    a2ui_img_surface_t *existing = surface_find(img_obj);
    if (existing) {
        return existing;
    }
    for (int i = 0; i < A2UI_IMG_MAX_SURFACES; i++) {
        if (s_surfaces[i].img_obj == NULL) {
            s_surfaces[i].img_obj = img_obj;
            return &s_surfaces[i];
        }
    }
    return NULL;
}

static void surface_free_buf(a2ui_img_surface_t *surf)
{
    if (!surf) {
        return;
    }
    if (surf->buf) {
        heap_caps_free(surf->buf);
        surf->buf = NULL;
    }
    memset(&surf->dsc, 0, sizeof(surf->dsc));
}

static void surface_release_all(void)
{
    for (int i = 0; i < A2UI_IMG_MAX_SURFACES; i++) {
        surface_free_buf(&s_surfaces[i]);
        s_surfaces[i].img_obj = NULL;
    }
}

void a2ui_image_set_disp(lv_display_t *disp)
{
    s_disp = disp;
}

void a2ui_image_set_http_origin(const char *origin)
{
    s_http_origin[0] = '\0';
    if (!origin || !origin[0]) {
        return;
    }
    size_t n = 0;
    while (origin[n] && n + 1 < sizeof(s_http_origin)) {
        s_http_origin[n] = origin[n];
        n++;
    }
    while (n > 0 && s_http_origin[n - 1] == '/') {
        n--;
    }
    s_http_origin[n] = '\0';
    ESP_LOGD(TAG, "http_origin=%s", s_http_origin);
}

static bool host_is_loopback(const char *host, size_t host_len)
{
    if (host_len == 9 && strncasecmp(host, "localhost", 9) == 0) {
        return true;
    }
    if (host_len == 9 && strncmp(host, "127.0.0.1", 9) == 0) {
        return true;
    }
    if (host_len == 3 && strncmp(host, "::1", 3) == 0) {
        return true;
    }
    return false;
}

/** Rewrite relative / localhost URLs to s_http_origin. Caller frees. */
static char *resolve_image_url(const char *url)
{
    if (!url || !url[0]) {
        return NULL;
    }

    /* Local schemes / absolute SD paths: never rewrite to http_origin. */
    if (strncmp(url, "file://", 7) == 0 || strncmp(url, "mem://", 6) == 0) {
        return strdup(url);
    }

    if (url[0] == '/' && s_http_origin[0]) {
        size_t need = strlen(s_http_origin) + strlen(url) + 1;
        char *out = static_cast<char *>(malloc(need));
        if (!out) {
            return NULL;
        }
        snprintf(out, need, "%s%s", s_http_origin, url);
        return out;
    }

    const char *scheme_end = strstr(url, "://");
    if (!scheme_end || !s_http_origin[0]) {
        return strdup(url);
    }

    const char *host = scheme_end + 3;
    const char *path = strchr(host, '/');
    size_t host_len = path ? static_cast<size_t>(path - host) : strlen(host);
    const char *colon = static_cast<const char *>(memchr(host, ':', host_len));
    size_t name_len = colon ? static_cast<size_t>(colon - host) : host_len;

    if (!host_is_loopback(host, name_len)) {
        return strdup(url);
    }

    if (!path) {
        return strdup(s_http_origin);
    }

    size_t need = strlen(s_http_origin) + strlen(path) + 1;
    char *out = static_cast<char *>(malloc(need));
    if (!out) {
        return NULL;
    }
    snprintf(out, need, "%s%s", s_http_origin, path);
    return out;
}

static esp_err_t local_blob_load(const char *url, uint8_t **out_buf, int *out_len)
{
    *out_buf = NULL;
    *out_len = 0;
    if (!url || !url[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(s_local_mu);
    for (int i = 0; i < A2UI_IMG_LOCAL_SLOTS; ++i) {
        if (s_local[i].data && s_local[i].url[0] && strcmp(s_local[i].url, url) == 0) {
            uint8_t *copy = static_cast<uint8_t *>(
                heap_caps_malloc(s_local[i].len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!copy) {
                copy = static_cast<uint8_t *>(malloc(s_local[i].len));
            }
            if (!copy) {
                return ESP_ERR_NO_MEM;
            }
            memcpy(copy, s_local[i].data, s_local[i].len);
            *out_buf = copy;
            *out_len = static_cast<int>(s_local[i].len);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/** file:///abs/path or absolute /sdcard/... → fread into SPIRAM. */
static esp_err_t file_load(const char *url, uint8_t **out_buf, int *out_len)
{
    *out_buf = NULL;
    *out_len = 0;
    if (!url || !url[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *path = url;
    if (strncmp(url, "file://", 7) == 0) {
        path = url + 7;
        if (path[0] == '/' && path[1] == '/') {
            path += 1;
        } else if (path[0] != '/') {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (url[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "file open fail %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long sz = ftell(f);
    if (sz <= 0 || sz > A2UI_IMG_MAX_LOCAL) {
        fclose(f);
        ESP_LOGW(TAG, "file bad size %ld path=%s", sz, path);
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    uint8_t *buf = static_cast<uint8_t *>(
        heap_caps_malloc(static_cast<size_t>(sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        buf = static_cast<uint8_t *>(malloc(static_cast<size_t>(sz)));
    }
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    const size_t n = fread(buf, 1, static_cast<size_t>(sz), f);
    fclose(f);
    if (n != static_cast<size_t>(sz)) {
        heap_caps_free(buf);
        return ESP_FAIL;
    }
    *out_buf = buf;
    *out_len = static_cast<int>(sz);
    ESP_LOGI(TAG, "file load %s (%d bytes)", path, *out_len);
    return ESP_OK;
}

static bool url_is_local_path(const char *url)
{
    if (!url || !url[0]) {
        return false;
    }
    if (strncmp(url, "file://", 7) == 0) {
        return true;
    }
    if (url[0] == '/' && strstr(url, "://") == NULL) {
        return true;
    }
    return false;
}

esp_err_t a2ui_image_put_local(const char *url, const uint8_t *a2i1, size_t len)
{
    if (!url || !url[0] || !a2i1 || len == 0 || len > A2UI_IMG_MAX_LOCAL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t *copy = static_cast<uint8_t *>(
        heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!copy) {
        copy = static_cast<uint8_t *>(malloc(len));
    }
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, a2i1, len);

    uint8_t *old = NULL;
    int slot = -1;
    {
        std::lock_guard<std::mutex> lock(s_local_mu);
        for (int i = 0; i < A2UI_IMG_LOCAL_SLOTS; ++i) {
            if (s_local[i].url[0] && strcmp(s_local[i].url, url) == 0) {
                slot = i;
                break;
            }
            if (slot < 0 && s_local[i].data == NULL) {
                slot = i;
            }
        }
        if (slot < 0) {
            slot = s_local_rr % A2UI_IMG_LOCAL_SLOTS;
            s_local_rr = (s_local_rr + 1) % A2UI_IMG_LOCAL_SLOTS;
        }
        old = s_local[slot].data;
        s_local[slot].data = copy;
        s_local[slot].len = len;
        strncpy(s_local[slot].url, url, sizeof(s_local[slot].url) - 1);
        s_local[slot].url[sizeof(s_local[slot].url) - 1] = '\0';
    }
    if (old) {
        heap_caps_free(old);
    }
    ESP_LOGI(TAG, "put_local %s (%u bytes) slot=%d", url, (unsigned)len, slot);
    return ESP_OK;
}

esp_err_t a2ui_image_put_local_l8(const char *url, uint16_t w, uint16_t h, const uint8_t *pixels,
                                  size_t pixels_len)
{
    if (!url || !url[0] || !pixels || w == 0 || h == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t need_px = static_cast<size_t>(w) * static_cast<size_t>(h);
    if (pixels_len < need_px) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t total = sizeof(a2ui_l8_hdr_t) + need_px;
    if (total > A2UI_IMG_MAX_LOCAL) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *blob = static_cast<uint8_t *>(
        heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!blob) {
        blob = static_cast<uint8_t *>(malloc(total));
    }
    if (!blob) {
        return ESP_ERR_NO_MEM;
    }

    a2ui_l8_hdr_t *hdr = reinterpret_cast<a2ui_l8_hdr_t *>(blob);
    hdr->magic = A2UI_L8_MAGIC;
    hdr->width = w;
    hdr->height = h;
    memcpy(blob + sizeof(a2ui_l8_hdr_t), pixels, need_px);

    /* Reuse put_local slot logic via temporary steal — store blob directly. */
    uint8_t *old = NULL;
    int slot = -1;
    {
        std::lock_guard<std::mutex> lock(s_local_mu);
        for (int i = 0; i < A2UI_IMG_LOCAL_SLOTS; ++i) {
            if (s_local[i].url[0] && strcmp(s_local[i].url, url) == 0) {
                slot = i;
                break;
            }
            if (slot < 0 && s_local[i].data == NULL) {
                slot = i;
            }
        }
        if (slot < 0) {
            slot = s_local_rr % A2UI_IMG_LOCAL_SLOTS;
            s_local_rr = (s_local_rr + 1) % A2UI_IMG_LOCAL_SLOTS;
        }
        old = s_local[slot].data;
        s_local[slot].data = blob;
        s_local[slot].len = total;
        strncpy(s_local[slot].url, url, sizeof(s_local[slot].url) - 1);
        s_local[slot].url[sizeof(s_local[slot].url) - 1] = '\0';
    }
    if (old) {
        heap_caps_free(old);
    }
    ESP_LOGI(TAG, "put_local_l8 %s %ux%u (%u bytes) slot=%d", url, (unsigned)w, (unsigned)h,
             (unsigned)total, slot);
    return ESP_OK;
}

void a2ui_image_abort_loads(void)
{
    s_job_gen++;
    drain_queue();
}

void a2ui_image_clear(void)
{
    a2ui_image_abort_loads();
    surface_release_all();
}

/** Poll gen each read chunk so page-flip can abort without waiting for full body. */
static esp_err_t http_download(const char *url, uint8_t **out_buf, int *out_len, uint32_t gen)
{
    *out_buf = NULL;
    *out_len = 0;

    ESP_LOGD(TAG, "GET %s (Board network Http)", url);

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGE(TAG, "no network");
        return ESP_ERR_INVALID_STATE;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        ESP_LOGE(TAG, "CreateHttp failed");
        return ESP_ERR_NO_MEM;
    }

    http->SetTimeout(15000);
    http->SetKeepAlive(false);
    api::ApplyCommonHeaders(http.get());

    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "http open failed err=0x%x", http->GetLastError());
        return ESP_FAIL;
    }

    const int status = http->GetStatusCode();
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "http status %d", status);
        http->Close();
        return ESP_FAIL;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length > static_cast<size_t>(A2UI_IMG_MAX_DOWNLOAD)) {
        ESP_LOGW(TAG, "skip download: Content-Length %u > max %d",
                 static_cast<unsigned>(content_length), A2UI_IMG_MAX_DOWNLOAD);
        http->Close();
        return ESP_ERR_INVALID_SIZE;
    }
    int cap = content_length > 0 ? static_cast<int>(content_length) : A2UI_IMG_MAX_DOWNLOAD;
    if (cap > A2UI_IMG_MAX_DOWNLOAD) {
        cap = A2UI_IMG_MAX_DOWNLOAD;
    }

    uint8_t *buf = static_cast<uint8_t *>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        buf = static_cast<uint8_t *>(malloc(cap));
    }
    if (!buf) {
        http->Close();
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    int total = 0;
    while (total < cap) {
        if (job_cancelled(gen)) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        int n = http->Read(reinterpret_cast<char *>(buf) + total, static_cast<size_t>(cap - total));
        if (n < 0) {
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            break;
        }
        total += n;
    }
    http->Close();

    if (err != ESP_OK || total < static_cast<int>(sizeof(a2ui_i1_hdr_t) + 8)) {
        heap_caps_free(buf);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    *out_buf = buf;
    *out_len = total;
    ESP_LOGI(TAG, "downloaded %d bytes", total);
    return ESP_OK;
}

static esp_err_t parse_a2i1(uint8_t *buf, int len, int *w, int *h, uint32_t *stride, uint8_t **payload,
                            size_t *payload_size)
{
    if (len < static_cast<int>(sizeof(a2ui_i1_hdr_t) + 8)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const a2ui_i1_hdr_t *hdr = reinterpret_cast<const a2ui_i1_hdr_t *>(buf);
    if (hdr->magic != A2UI_I1_MAGIC) {
        ESP_LOGE(TAG, "bad magic 0x%08lx (need A2I1 from server)", (unsigned long)hdr->magic);
        return ESP_ERR_INVALID_ARG;
    }
    if (hdr->width == 0 || hdr->height == 0 || hdr->stride == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t need = sizeof(a2ui_i1_hdr_t) + 8u + static_cast<size_t>(hdr->stride) * static_cast<size_t>(hdr->height);
    if (static_cast<size_t>(len) < need) {
        ESP_LOGE(TAG, "truncated a2i1 %d < %u", len, (unsigned)need);
        return ESP_ERR_INVALID_SIZE;
    }

    *w = hdr->width;
    *h = hdr->height;
    *stride = hdr->stride;
    *payload = buf + sizeof(a2ui_i1_hdr_t);
    *payload_size = 8u + static_cast<size_t>(hdr->stride) * static_cast<size_t>(hdr->height);
    return ESP_OK;
}

/**
 * 墨水屏上屏：一律 LV_COLOR_FORMAT_L8（与书封/产测同源）。
 * owned_blob 可为 A2L8（直挂）或 A2I1（解码后释放原包）；成败均接管释放。
 */
static bool apply_image(lv_obj_t *img_obj, uint8_t *owned_blob, int blob_len, uint32_t gen,
                        bool already_locked)
{
    if (!owned_blob || blob_len <= 0) {
        if (owned_blob) {
            heap_caps_free(owned_blob);
        }
        return false;
    }
    if (!img_obj || job_cancelled(gen)) {
        heap_caps_free(owned_blob);
        return false;
    }

    uint8_t *l8 = NULL;
    uint16_t out_w = 0;
    uint16_t out_h = 0;
    size_t l8_bytes = 0;
    bool l8_is_owned_blob = false; /* A2L8：surf 持有整包，dsc.data 跳过头 */

    const uint32_t magic =
        static_cast<uint32_t>(owned_blob[0]) | (static_cast<uint32_t>(owned_blob[1]) << 8) |
        (static_cast<uint32_t>(owned_blob[2]) << 16) | (static_cast<uint32_t>(owned_blob[3]) << 24);

    if (magic == A2UI_L8_MAGIC) {
        if (static_cast<size_t>(blob_len) < sizeof(a2ui_l8_hdr_t)) {
            ESP_LOGW(TAG, "apply: truncated A2L8 len=%d", blob_len);
            heap_caps_free(owned_blob);
            return false;
        }
        const a2ui_l8_hdr_t *hdr = reinterpret_cast<const a2ui_l8_hdr_t *>(owned_blob);
        out_w = hdr->width;
        out_h = hdr->height;
        l8_bytes = static_cast<size_t>(out_w) * static_cast<size_t>(out_h);
        if (out_w == 0 || out_h == 0 ||
            static_cast<size_t>(blob_len) < sizeof(a2ui_l8_hdr_t) + l8_bytes) {
            ESP_LOGW(TAG, "apply: bad A2L8 %ux%u len=%d", (unsigned)out_w, (unsigned)out_h,
                     blob_len);
            heap_caps_free(owned_blob);
            return false;
        }
        l8 = owned_blob + sizeof(a2ui_l8_hdr_t);
        l8_is_owned_blob = true;
    } else {
        int hdr_w = 0;
        int hdr_h = 0;
        uint32_t hdr_stride = 0;
        uint8_t *payload = NULL;
        size_t payload_size = 0;
        if (parse_a2i1(owned_blob, blob_len, &hdr_w, &hdr_h, &hdr_stride, &payload, &payload_size) !=
            ESP_OK) {
            ESP_LOGW(TAG, "apply: bad blob magic=0x%08lx len=%d", (unsigned long)magic, blob_len);
            heap_caps_free(owned_blob);
            return false;
        }
        (void)payload;
        (void)payload_size;
        (void)hdr_stride;

        reader::RasterImage raster;
        if (!reader::DecodeImageToL8(owned_blob, static_cast<size_t>(blob_len), hdr_w, hdr_h,
                                     raster) ||
            raster.empty()) {
            ESP_LOGW(TAG, "apply: A2I1→L8 failed %dx%d", hdr_w, hdr_h);
            heap_caps_free(owned_blob);
            return false;
        }
        heap_caps_free(owned_blob);
        owned_blob = NULL;

        out_w = raster.width;
        out_h = raster.height;
        l8_bytes = static_cast<size_t>(out_w) * static_cast<size_t>(out_h);
        l8 = static_cast<uint8_t *>(heap_caps_malloc(l8_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!l8) {
            l8 = static_cast<uint8_t *>(malloc(l8_bytes));
        }
        if (!l8) {
            ESP_LOGE(TAG, "apply: L8 alloc %u failed", (unsigned)l8_bytes);
            return false;
        }
        memcpy(l8, raster.pixels.data(), l8_bytes);
        raster.Reset();
    }

    if (!already_locked) {
        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            if (l8_is_owned_blob) {
                heap_caps_free(owned_blob);
            } else {
                heap_caps_free(l8);
            }
            return false;
        }
    }

    if (job_cancelled(gen) || !lv_obj_is_valid(img_obj)) {
        if (l8_is_owned_blob) {
            heap_caps_free(owned_blob);
        } else {
            heap_caps_free(l8);
        }
        if (!already_locked) {
            esp_lv_adapter_unlock();
        }
        return false;
    }

    a2ui_img_surface_t *surf = surface_find(img_obj);
    if (!surf) {
        if (l8_is_owned_blob) {
            heap_caps_free(owned_blob);
        } else {
            heap_caps_free(l8);
        }
        if (!already_locked) {
            esp_lv_adapter_unlock();
        }
        return false;
    }

    surface_free_buf(surf);
    if (l8_is_owned_blob) {
        surf->buf = owned_blob;
    } else {
        surf->buf = l8;
    }

    memset(&surf->dsc, 0, sizeof(surf->dsc));
    surf->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    surf->dsc.header.cf = LV_COLOR_FORMAT_L8;
    surf->dsc.header.flags = 0;
    surf->dsc.header.w = out_w;
    surf->dsc.header.h = out_h;
    surf->dsc.header.stride = out_w;
    surf->dsc.data_size = static_cast<uint32_t>(l8_bytes);
    surf->dsc.data = l8;

    lv_image_set_src(img_obj, &surf->dsc);
    lv_obj_set_size(img_obj, static_cast<lv_coord_t>(out_w), static_cast<lv_coord_t>(out_h));
    lv_obj_set_style_bg_opa(img_obj, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(img_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(img_obj);
    lv_obj_t *parent = lv_obj_get_parent(img_obj);
    if (parent) {
        lv_obj_invalidate(parent);
    }

    if (!already_locked) {
        esp_lv_adapter_unlock();
    }
    (void)s_disp;
    ESP_LOGI(TAG, "shown L8 %ux%u (%s, sync=%d)", (unsigned)out_w, (unsigned)out_h,
             l8_is_owned_blob ? "A2L8" : "A2I1→L8", already_locked ? 1 : 0);
    return true;
}

static void image_task(void *arg)
{
    a2ui_img_job_t *job = static_cast<a2ui_img_job_t *>(arg);

    while (job) {
        const uint32_t gen = s_job_gen;
        uint8_t *buf = NULL;
        size_t len_sz = 0;
        int len = 0;
        int w = 0;
        int h = 0;
        uint32_t stride = 0;
        uint8_t *payload = NULL;
        size_t payload_size = 0;
        bool from_cache = false;
        bool parsed_ok = false;

        esp_err_t err = ESP_FAIL;
        /* 1) RAM put_local (mem:// / preloaded file://) */
        err = local_blob_load(job->url, &buf, &len);
        if (err == ESP_OK && buf && len > 0) {
            from_cache = true;
        } else {
            buf = NULL;
            len = 0;
            err = ESP_FAIL;
        }

        /* 2) SD session cache */
        if (err != ESP_OK && a2ui_img_cache_is_ready()) {
            err = a2ui_img_cache_load(job->url, &buf, &len_sz);
            if (err == ESP_OK && buf && len_sz > 0) {
                len = static_cast<int>(len_sz);
                from_cache = true;
            } else {
                buf = NULL;
                len_sz = 0;
                err = ESP_FAIL;
            }
        }

        /* 3) Local file path / HTTP */
        if (err != ESP_OK) {
            if (job_cancelled(gen)) {
                goto next_job;
            }
            if (url_is_local_path(job->url)) {
                err = file_load(job->url, &buf, &len);
            } else {
                err = http_download(job->url, &buf, &len, gen);
            }
            if (err != ESP_OK || job_cancelled(gen)) {
                goto next_job;
            }
            from_cache = false;
        } else if (job_cancelled(gen)) {
            goto next_job;
        }

        parsed_ok = false;
        if (buf && len >= 4) {
            const uint32_t magic = static_cast<uint32_t>(buf[0]) | (static_cast<uint32_t>(buf[1]) << 8) |
                                   (static_cast<uint32_t>(buf[2]) << 16) |
                                   (static_cast<uint32_t>(buf[3]) << 24);
            if (magic == A2UI_L8_MAGIC) {
                parsed_ok = true;
            }
        }
        if (!parsed_ok) {
            err = parse_a2i1(buf, len, &w, &h, &stride, &payload, &payload_size);
            if (err != ESP_OK) {
                if (from_cache) {
                    ESP_LOGW(TAG, "cache entry invalid, invalidate + reload");
                    a2ui_img_cache_invalidate(job->url);
                    heap_caps_free(buf);
                    buf = NULL;
                    if (job_cancelled(gen)) {
                        goto next_job;
                    }
                    if (url_is_local_path(job->url)) {
                        err = file_load(job->url, &buf, &len);
                    } else if (strncmp(job->url, "mem://", 6) != 0) {
                        err = http_download(job->url, &buf, &len, gen);
                    } else {
                        err = ESP_FAIL;
                    }
                    if (err != ESP_OK || job_cancelled(gen)) {
                        goto next_job;
                    }
                    from_cache = false;
                    err = parse_a2i1(buf, len, &w, &h, &stride, &payload, &payload_size);
                }
                if (err != ESP_OK || job_cancelled(gen)) {
                    goto next_job;
                }
            } else if (job_cancelled(gen)) {
                goto next_job;
            }
            parsed_ok = true;
        } else if (job_cancelled(gen)) {
            goto next_job;
        }
        (void)parsed_ok;

        {
            /* 先落缓存（仅 A2I1），再 apply（A2L8 直显 / A2I1→L8） */
            const bool is_a2i1 =
                buf && len >= 4 && buf[0] == 'A' && buf[1] == '2' && buf[2] == 'I' && buf[3] == '1';
            const bool do_store =
                (!from_cache && is_a2i1 && a2ui_img_cache_is_ready() && buf && len > 0);
            if (do_store) {
                (void)a2ui_img_cache_store(job->url, buf, static_cast<size_t>(len));
            }
            lv_obj_t *target = job->img_obj;
            const bool shown = apply_image(target, buf, len, gen, false);
            buf = NULL;
            (void)shown;
        }

    next_job:
        if (buf) {
            heap_caps_free(buf);
            buf = NULL;
        }
        free_job(job);
        job = dequeue_job_or_idle();
        if (job) {
            ESP_LOGI(TAG, "run next queued image job");
        }
    }

    vTaskDelete(NULL);
}

esp_err_t a2ui_image_load_async(lv_obj_t *img_obj, const char *url, int max_w, int max_h)
{
    (void)max_w;
    (void)max_h;
    ESP_RETURN_ON_FALSE(img_obj && url && url[0], ESP_ERR_INVALID_ARG, TAG, "bad args");

    /* Reserve surface before HTTP so we never download into a full table. */
    if (!surface_get_or_alloc(img_obj)) {
        ESP_LOGW(TAG, "Image skipped: surface table full (%d)", A2UI_IMG_MAX_SURFACES);
        return ESP_ERR_NO_MEM;
    }

    char *resolved = resolve_image_url(url);
    ESP_RETURN_ON_FALSE(resolved, ESP_ERR_NO_MEM, TAG, "resolve url");
    if (strcmp(resolved, url) != 0) {
        ESP_LOGW(TAG, "rewrite %s -> %s", url, resolved);
    }

    /*
     * mem://：put_local / put_local_l8 已就绪。须在当前 LVGL 锁内同步上屏，
     * 否则墨水屏会先局刷空占位，异步上图后不一定再刷。
     * A2L8 直挂 L8；A2I1 则解码为 L8。调用方已持 Display/adapter lock。
     */
    if (strncmp(resolved, "mem://", 6) == 0) {
        uint8_t *buf = NULL;
        int len = 0;
        size_t len_sz = 0;
        esp_err_t err = local_blob_load(resolved, &buf, &len);
        if (err != ESP_OK && a2ui_img_cache_is_ready()) {
            err = a2ui_img_cache_load(resolved, &buf, &len_sz);
            if (err == ESP_OK && buf && len_sz > 0) {
                len = static_cast<int>(len_sz);
            } else {
                buf = NULL;
                err = ESP_ERR_NOT_FOUND;
            }
        }
        if (err != ESP_OK || buf == NULL || len <= 0) {
            ESP_LOGW(TAG, "mem:// miss %s", resolved);
            free(resolved);
            return err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
        }

        const bool ok = apply_image(img_obj, buf, len, s_job_gen, true);
        free(resolved);
        return ok ? ESP_OK : ESP_FAIL;
    }

    a2ui_img_job_t *job = static_cast<a2ui_img_job_t *>(calloc(1, sizeof(*job)));
    if (!job) {
        free(resolved);
        return ESP_ERR_NO_MEM;
    }
    job->img_obj = img_obj;
    job->url = resolved;

    bool start_new = false;
    portENTER_CRITICAL(&s_job_mux);
    if (s_worker_alive) {
        /* Append to FIFO — do not bump gen (would cancel sibling downloads). */
        if (s_queue_tail) {
            s_queue_tail->next = job;
            s_queue_tail = job;
        } else {
            s_queue_head = job;
            s_queue_tail = job;
        }
    } else {
        s_worker_alive = true;
        start_new = true;
    }
    portEXIT_CRITICAL(&s_job_mux);

    if (!start_new) {
        ESP_LOGI(TAG, "image worker busy — queued FIFO");
        return ESP_OK;
    }

    // 任务内可能写 SD 缓存：栈须 INTERNAL
    if (xTaskCreate(image_task, "a2ui_img", A2UI_IMG_TASK_STACK, job, A2UI_IMG_TASK_PRIO, NULL) !=
        pdPASS) {
        free_job(job);
        portENTER_CRITICAL(&s_job_mux);
        s_worker_alive = false;
        a2ui_img_job_t *head = s_queue_head;
        s_queue_head = NULL;
        s_queue_tail = NULL;
        portEXIT_CRITICAL(&s_job_mux);
        while (head) {
            a2ui_img_job_t *next = head->next;
            free_job(head);
            head = next;
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
