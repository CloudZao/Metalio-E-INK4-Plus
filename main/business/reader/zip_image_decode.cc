#include <algorithm>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "zip_image_decode.h"
#include "infra/image/image_stream.h"

static constexpr const char* TAG = "ZipImage";
static constexpr size_t kChunkIo = 8 * 1024;  // ZIP 条目的分块读取大小

static bool StreamReadExact(ZipReader& zip, uint8_t* dst, size_t n) {
    size_t got = 0;
    while (got < n) {
        const size_t r = zip.EntryStreamRead(dst + got, n - got);
        if (r == 0) {
            return false;
        }
        got += r;
    }
    return true;
}

static bool AbortRequested(const std::atomic<bool>* abort) {
    return abort != nullptr && abort->load(std::memory_order_relaxed);
}

static size_t ReadZipImage(void* context, uint8_t* bytes, size_t count) {
    auto& zip = *static_cast<ZipReader*>(context);
    if (bytes) {
        return zip.EntryStreamRead(bytes, count);
    }
    uint8_t discard[64];
    size_t consumed = 0;
    while (consumed < count) {
        const size_t got = zip.EntryStreamRead(discard, std::min(count - consumed, sizeof(discard)));
        if (!got) {
            break;
        }
        consumed += got;
    }
    return consumed;
}

static bool ZipImageCancelled(void* context) {
    return AbortRequested(static_cast<const std::atomic<bool>*>(context));
}

static bool DecodeImageBuffered(ZipReader& zip, int max_w, int max_h, RasterImage& out,
                                const std::atomic<bool>* abort) {
    out.Reset();
    const size_t size = zip.EntryStreamUncompSize();
    if (!size || size > 4 * 1024 * 1024 || AbortRequested(abort)) {
        return false;
    }
    auto* compressed = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!compressed) {
        return false;
    }
    size_t read = 0;
    while (read < size && !AbortRequested(abort)) {
        const size_t n = zip.EntryStreamRead(compressed + read, std::min(kChunkIo, size - read));
        if (!n) {
            break;
        }
        read += n;
    }
    const bool ok = read == size && !AbortRequested(abort) && !zip.EntryStreamError() &&
                    DecodeImageToL8(
                        compressed, size, max_w, max_h, out,
                        [](void* p) { return AbortRequested(static_cast<const std::atomic<bool>*>(p)); },
                        const_cast<std::atomic<bool>*>(abort));
    heap_caps_free(compressed);
    ESP_LOGI(TAG, "decode=image software fit bytes=%u ok=%d", static_cast<unsigned>(size), ok);
    return ok;
}

bool DecodeZipEntryImageToL8(ZipReader& zip, const char* entry_name, int max_w, int max_h, RasterImage& out,
                             const std::atomic<bool>* abort) {
    out.Reset();
    if (entry_name == nullptr || max_w <= 0 || max_h <= 0) {
        return false;
    }
    if (AbortRequested(abort)) {
        return false;
    }
    if (!zip.OpenEntryStream(entry_name)) {
        return false;
    }

    uint8_t head[8];
    if (!StreamReadExact(zip, head, 8)) {
        zip.CloseEntryStream();
        return false;
    }
    zip.CloseEntryStream();
    if (AbortRequested(abort) || !zip.OpenEntryStream(entry_name)) {
        return false;
    }

    bool ok = false;
    if (head[0] == 0xFF && head[1] == 0xD8) {
        ok = DecodeJpegStreamToL8({&zip, ReadZipImage}, max_w, max_h, out, ZipImageCancelled,
                                  const_cast<std::atomic<bool>*>(abort));
        if (!ok && !AbortRequested(abort)) {
            zip.CloseEntryStream();
            if (zip.OpenEntryStream(entry_name)) {
                ok = DecodeImageBuffered(zip, max_w, max_h, out, abort);
            }
        }
    } else if (std::memcmp(head, "\x89PNG\r\n\x1a\n", 8) == 0) {
        ok = DecodePngStreamToL8({&zip, ReadZipImage}, max_w, max_h, out, ZipImageCancelled,
                                 const_cast<std::atomic<bool>*>(abort));
    } else {
        ESP_LOGW(TAG, "operation=decode_zip result=unsupported");
    }

    if (AbortRequested(abort)) {
        ESP_LOGI(TAG, "operation=decode_zip result=cancelled");
        ok = false;
        out.Reset();
        std::vector<uint8_t>().swap(out.pixels);
    } else if (zip.EntryStreamError()) {
        ESP_LOGW(TAG, "operation=decode_zip result=read_error");
        ok = false;
        out.Reset();
        std::vector<uint8_t>().swap(out.pixels);
    }
    zip.CloseEntryStream();
    if (ok) {
        ESP_LOGI(TAG, "operation=decode_zip result=ok out=%ux%u", out.width, out.height);
    }
    return ok;
}
