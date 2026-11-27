#include <cstring>
#include <esp_app_format.h>
#include <esp_crc.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include "resources_upgrade.h"
#include "boards/common/bus/flash_access.h"

static const char* TAG = "res_ota";

// 接在合法 app 镜像之后，布局与 tools/assets/attach_upgrade_resources.py 一致。
struct TrailerHeader {
    char magic[4];
    uint16_t version;
    uint16_t header_size;
    uint32_t payload_size;
    uint32_t payload_crc32;
} __attribute__((packed));

static_assert(sizeof(TrailerHeader) == 16, "OTA resources trailer header");

static constexpr char kMagic[4] = {'M', 'R', 'E', 'S'};
static constexpr uint16_t kVersion = 1;

static bool ImageEnd(const esp_partition_t* part, uint32_t* end_out) {
    esp_image_header_t header{};
    if (esp_partition_read(part, 0, &header, sizeof(header)) != ESP_OK) {
        return false;
    }
    if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.segment_count == 0 ||
        header.segment_count > ESP_IMAGE_MAX_SEGMENTS) {
        return false;
    }

    uint32_t off = sizeof(header);
    for (uint8_t i = 0; i < header.segment_count; ++i) {
        esp_image_segment_header_t seg{};
        if (off > part->size || sizeof(seg) > part->size - off) {
            return false;
        }
        if (esp_partition_read(part, off, &seg, sizeof(seg)) != ESP_OK) {
            return false;
        }
        if (seg.data_len > part->size - off - sizeof(seg)) {
            return false;
        }
        off += static_cast<uint32_t>(sizeof(seg)) + seg.data_len;
    }

    uint32_t end = (off + 1u + 15u) & ~15u;
    if (header.hash_appended) {
        if (end > part->size || part->size - end < 32) {
            return false;
        }
        end += 32;
    }
    if (end > part->size) {
        return false;
    }
    *end_out = end;
    return true;
}

static bool PayloadCrc(const esp_partition_t* part, uint32_t offset, uint32_t size, uint32_t* crc_out) {
    uint8_t buf[1024];
    uint32_t crc = 0;
    uint32_t done = 0;
    while (done < size) {
        const uint32_t n = (size - done) > sizeof(buf) ? sizeof(buf) : (size - done);
        if (esp_partition_read(part, offset + done, buf, n) != ESP_OK) {
            return false;
        }
        crc = esp_crc32_le(crc, buf, n);
        done += n;
    }
    *crc_out = crc;
    return true;
}

static bool PayloadEquals(const esp_partition_t* app, uint32_t app_off, const esp_partition_t* res, uint32_t size) {
    uint8_t a[1024];
    uint8_t b[1024];
    uint32_t done = 0;
    while (done < size) {
        const uint32_t n = (size - done) > sizeof(a) ? sizeof(a) : (size - done);
        if (esp_partition_read(app, app_off + done, a, n) != ESP_OK || esp_partition_read(res, done, b, n) != ESP_OK) {
            return false;
        }
        if (std::memcmp(a, b, n) != 0) {
            return false;
        }
        done += n;
    }
    return true;
}

static bool WritePayload(const esp_partition_t* app, uint32_t app_off, const esp_partition_t* res, uint32_t size) {
    FlashAccessGuard flash;
    if (flash.status() != ESP_OK) {
        return false;
    }
    if (esp_partition_erase_range(res, 0, res->size) != ESP_OK) {
        ESP_LOGE(TAG, "erase resources failed");
        return false;
    }

    uint8_t buf[1024];
    uint32_t done = 0;
    while (done < size) {
        const uint32_t n = (size - done) > sizeof(buf) ? sizeof(buf) : (size - done);
        if (esp_partition_read(app, app_off + done, buf, n) != ESP_OK) {
            return false;
        }
        uint32_t write_n = n;
        if ((write_n & 15u) != 0) {
            const uint32_t padded = (write_n + 15u) & ~15u;
            std::memset(buf + write_n, 0xFF, padded - write_n);
            write_n = padded;
        }
        if (done + write_n > res->size || esp_partition_write(res, done, buf, write_n) != ESP_OK) {
            ESP_LOGE(TAG, "write resources failed at %u", static_cast<unsigned>(done));
            return false;
        }
        done += n;
    }
    return true;
}

void ResourcesUpgradeApply() {
    const esp_partition_t* app = esp_ota_get_running_partition();
    const esp_partition_t* res =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "resources");
    if (app == nullptr || res == nullptr) {
        return;
    }

    uint32_t image_end = 0;
    if (!ImageEnd(app, &image_end)) {
        return;
    }
    if (image_end > app->size || app->size - image_end < sizeof(TrailerHeader)) {
        return;
    }

    TrailerHeader header{};
    if (esp_partition_read(app, image_end, &header, sizeof(header)) != ESP_OK) {
        return;
    }
    if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 || header.version != kVersion ||
        header.header_size != sizeof(TrailerHeader)) {
        return;
    }
    if (header.payload_size == 0 || header.payload_size > res->size) {
        ESP_LOGE(TAG, "OTA resources trailer size %u invalid", static_cast<unsigned>(header.payload_size));
        return;
    }
    const uint32_t payload_off = image_end + static_cast<uint32_t>(sizeof(TrailerHeader));
    if (payload_off > app->size || header.payload_size > app->size - payload_off) {
        ESP_LOGE(TAG, "OTA resources trailer exceeds app partition");
        return;
    }

    uint32_t crc = 0;
    if (!PayloadCrc(app, payload_off, header.payload_size, &crc) || crc != header.payload_crc32) {
        ESP_LOGE(TAG, "OTA resources trailer crc mismatch");
        return;
    }
    if (PayloadEquals(app, payload_off, res, header.payload_size)) {
        ESP_LOGI(TAG, "resources already match firmware");
        return;
    }
    if (!WritePayload(app, payload_off, res, header.payload_size)) {
        return;
    }
    ESP_LOGI(TAG, "resources written from firmware, %u bytes", static_cast<unsigned>(header.payload_size));
}
