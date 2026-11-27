#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/stat.h>
#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include "firmware_image.h"
#include "boards/common/board/board.h"
#include "boards/common/bus/flash_access.h"

static constexpr const char* TAG = "FirmwareImage";

template <typename Operation>
static auto WithOtaFlashAccess(Operation operation) {
    FlashAccessGuard guard;
    if (guard.status() != ESP_OK) {
        return guard.status();
    }
    return operation();
}

struct FirmwareImageWriteGuard {
    esp_ota_handle_t& handle;  // 非零句柄在作用域退出时中止
    ~FirmwareImageWriteGuard() {
        if (handle != 0) {
            const auto err = WithOtaFlashAccess([&] { return esp_ota_abort(handle); });
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "operation=abort_image code=%d", int(err));
            }
        }
    }
};

static esp_err_t FinishFirmwareWrite(esp_ota_handle_t& handle) {
    return WithOtaFlashAccess([&] {
        const auto err = esp_ota_end(handle);
        handle = 0;  // SDK 在校验成功或失败时均消费句柄；总线未取得时仍需中止。
        return err;
    });
}

static bool ValidFirmwareHeader(const char* data, size_t size) {
    constexpr size_t required =
        sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    if (size < required) {
        return false;
    }
    esp_image_header_t header{};
    esp_app_desc_t description{};
    memcpy(&header, data, sizeof(header));
    memcpy(&description, data + sizeof(header) + sizeof(esp_image_segment_header_t), sizeof(description));
    return header.magic == ESP_IMAGE_HEADER_MAGIC && description.magic_word == ESP_APP_DESC_MAGIC_WORD;
}

void FirmwareImage::MarkCurrentVersionValid() {
    auto partition = esp_ota_get_running_partition();
    if (partition == nullptr) {
        ESP_LOGE(TAG, "operation=confirm_image result=no_running_partition");
        return;
    }
    if (strcmp(partition->label, "factory") == 0) {
        ESP_LOGI(TAG, "Running from factory partition, skipping");
        return;
    }

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get state of partition");
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking firmware as valid");
        const auto err = WithOtaFlashAccess([] { return esp_ota_mark_app_valid_cancel_rollback(); });
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "operation=confirm_image code=%d", int(err));
        }
    }
}

bool FirmwareImage::Upgrade(const std::string& firmware_url) {
    esp_ota_handle_t update_handle = 0;

    FirmwareImageWriteGuard abort_incomplete{update_handle};

    auto update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get update partition");
        return false;
    }

    // 下载前擦除整个非运行分区，避免预擦除与 UART 网络接收并发。
    const int64_t erase_started_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Preparing OTA partition %s before HTTP connection", update_partition->label);
    const esp_err_t begin_err =
        WithOtaFlashAccess([&]() { return esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle); });
    if (begin_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to prepare OTA partition: %s", esp_err_to_name(begin_err));
        return false;
    }
    ESP_LOGI(TAG, "OTA partition ready, erase elapsed_ms=%lld",
             static_cast<long long>((esp_timer_get_time() - erase_started_us) / 1000));
    bool image_header_checked = false;

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGE(TAG, "operation=download_image result=no_network");
        return false;
    }
    auto http = network->CreateHttp(0);
    if (!http) {
        ESP_LOGE(TAG, "operation=download_image result=no_memory");
        return false;
    }
    http->SetTimeout(30000);
    if (!http->Open("GET", firmware_url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    const int status = http->GetStatusCode();
    if (status != 200) {
        ESP_LOGE(TAG, "Failed to get firmware, status code: %d", status);
        return false;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0 || content_length > update_partition->size) {
        ESP_LOGE(TAG, "Invalid firmware size: %u (partition %lu)", static_cast<unsigned>(content_length),
                 update_partition->size);
        return false;
    }

    constexpr size_t kBufferSize = 8192;
    std::unique_ptr<char, decltype(&heap_caps_free)> buffer(
        static_cast<char*>(heap_caps_malloc(kBufferSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)), &heap_caps_free);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate OTA buffer");
        return false;
    }
    size_t buffered = 0;
    size_t total_read = 0, recent_read = 0;
    auto last_calc_time = esp_timer_get_time();
    while (true) {
        int ret = http->Read(buffer.get() + buffered, kBufferSize - buffered);
        if (ret < 0) {
            ESP_LOGE(TAG, "Failed to read HTTP data: %s", esp_err_to_name(ret));
            return false;
        }

        buffered += static_cast<size_t>(ret);
        if (ret == 0 && image_header_checked && total_read == content_length && buffered > 0) {
            const auto tail_err =
                WithOtaFlashAccess([&]() { return esp_ota_write(update_handle, buffer.get(), buffered); });
            if (tail_err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to write OTA tail: %s", esp_err_to_name(tail_err));
                return false;
            }
            buffered = 0;
        }

        // Calculate speed and progress every second
        recent_read += ret;
        total_read += ret;
        if (esp_timer_get_time() - last_calc_time >= 1000000 || ret == 0) {
            size_t progress = total_read * 100 / content_length;
            ESP_LOGI(TAG, "Progress: %u%% (%u/%u), Speed: %uB/s", progress, total_read, content_length, recent_read);
            if (upgrade_callback_) {
                upgrade_callback_(progress, recent_read);
            }
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }

        if (ret == 0) {
            break;
        }

        if (total_read > content_length) {
            ESP_LOGE(TAG, "Firmware exceeds advertised length");
            return false;
        }
        if (!image_header_checked) {
            constexpr size_t kHeaderNeed =
                sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
            if (buffered < kHeaderNeed) {
                continue;  // A TCP read may return less than the complete image header.
            }
            if (!ValidFirmwareHeader(buffer.get(), buffered)) {
                ESP_LOGE(TAG, "Invalid firmware image header");
                return false;
            }
            image_header_checked = true;
        }
        // 复用读取缓冲，攒满后写入；尾部在正常 EOF 时写入。
        if (buffered < kBufferSize) {
            continue;
        }
        auto err = WithOtaFlashAccess([&]() { return esp_ota_write(update_handle, buffer.get(), buffered); });
        buffered = 0;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write OTA data: %s", esp_err_to_name(err));
            return false;
        }
    }
    http->Close();

    if (!image_header_checked || total_read != content_length) {
        ESP_LOGE(TAG, "Incomplete firmware: %u/%u", static_cast<unsigned>(total_read),
                 static_cast<unsigned>(content_length));
        return false;
    }
    esp_err_t err = FinishFirmwareWrite(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        } else {
            ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(err));
        }
        return false;
    }

    err = WithOtaFlashAccess([&]() { return esp_ota_set_boot_partition(update_partition); });
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Firmware upgrade successful");
    return true;
}

bool FirmwareImage::StartUpgradeFromUrl(const std::string& url,
                                        std::function<void(int progress, size_t speed)> callback) {
    upgrade_callback_ = callback;
    return Upgrade(url);
}

bool FirmwareImage::StartUpgradeFromFile(const std::string& path,
                                         std::function<void(int progress, size_t speed)> callback) {
    upgrade_callback_ = callback;
    return UpgradeFromFile(path);
}

bool FirmwareImage::UpgradeFromFile(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        ESP_LOGE(TAG, "operation=read_image stage=stat result=missing_or_empty errno=%d", errno);
        return false;
    }
    const size_t content_length = static_cast<size_t>(st.st_size);
    auto partition = esp_ota_get_next_update_partition(nullptr);
    if (partition == nullptr || content_length > partition->size) {
        ESP_LOGE(TAG, "operation=read_image result=no_partition_or_oversized bytes=%u", unsigned(content_length));
        return false;
    }
    std::unique_ptr<FILE, decltype(&fclose)> file(fopen(path.c_str(), "rb"), fclose);
    if (!file) {
        ESP_LOGE(TAG, "operation=read_image stage=open errno=%d", errno);
        return false;
    }
    esp_ota_handle_t handle = 0;
    FirmwareImageWriteGuard abort_incomplete{handle};
    constexpr size_t required =
        sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    char buffer[512];
    static_assert(sizeof(buffer) >= required, "Image header must fit the first read");
    size_t total = 0;
    size_t recent = 0;
    auto last_progress = esp_timer_get_time();
    while (total < content_length) {
        const size_t count = fread(buffer, 1, std::min(sizeof(buffer), content_length - total), file.get());
        if (count == 0) {
            ESP_LOGE(TAG, "operation=read_image result=incomplete bytes=%u expected=%u errno=%d", unsigned(total),
                     unsigned(content_length), errno);
            return false;
        }
        if (total == 0) {
            if (!ValidFirmwareHeader(buffer, count)) {
                ESP_LOGE(TAG, "operation=read_image result=invalid_header");
                return false;
            }
            const auto err =
                WithOtaFlashAccess([&] { return esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle); });
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "operation=write_image stage=begin code=%d", int(err));
                return false;
            }
        }
        const auto err = WithOtaFlashAccess([&] { return esp_ota_write(handle, buffer, count); });
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "operation=write_image stage=data code=%d", int(err));
            return false;
        }
        total += count;
        recent += count;
        if (esp_timer_get_time() - last_progress >= 1000000 || total == content_length) {
            if (upgrade_callback_) {
                upgrade_callback_(static_cast<int>(total * 100 / content_length), recent);
            }
            last_progress = esp_timer_get_time();
            recent = 0;
        }
    }
    auto err = FinishFirmwareWrite(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "operation=write_image stage=validate code=%d", int(err));
        return false;
    }
    err = WithOtaFlashAccess([&] { return esp_ota_set_boot_partition(partition); });
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "operation=write_image stage=select_boot code=%d", int(err));
        return false;
    }
    ESP_LOGI(TAG, "operation=write_image result=success bytes=%u", unsigned(total));
    return true;
}
