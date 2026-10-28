#include "SdCardManager.hpp"

#include "config.h"
#include "sd_log_writer.h"
#include "sd_access.h"

#include <cstdlib>
#include <cstring>

#include <driver/gpio.h>
#include <driver/sdmmc_defs.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_private/esp_cache_private.h>
#include <soc/soc_caps.h>
#include <sdkconfig.h>

#define TAG "SdCardManager"

SdCardManager& SdCardManager::GetInstance() {
    static SdCardManager instance;
    return instance;
}

bool SdCardManager::CheckDmaBuffer(int slot, const void* buffer, size_t size) {
    return esp_ptr_internal(buffer) && sdmmc_host_check_buffer_alignment(slot, buffer, size);
}

esp_err_t SdCardManager::DoTransaction(int slot, sdmmc_command_t* command) {
    const esp_err_t result = sdmmc_host_do_transaction(slot, command);
    // 只记录数据读写错误，初始化探测中允许失败的命令不触发提示。
    if ((result != ESP_OK || command->error != ESP_OK) && command->datalen > 0 &&
        (command->opcode == MMC_READ_BLOCK_SINGLE || command->opcode == MMC_READ_BLOCK_MULTIPLE ||
         command->opcode == MMC_WRITE_BLOCK_SINGLE || command->opcode == MMC_WRITE_BLOCK_MULTIPLE)) {
        GetInstance().io_error_.store(true, std::memory_order_relaxed);
    }
    return result;
}

bool SdCardManager::PrepareDmaBuffer(sdmmc_host_t& host) {
    host.do_transaction = DoTransaction;
    uint32_t caps = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
    size_t bytes = 512;
    #if CONFIG_IDF_TARGET_ESP32S31
    // 隔离外部 DMA/cache 路径；PSRAM 数据由 SDK 经固定内部缓冲中转。
    bytes = 4096;
    host.check_buffer_alignment = CheckDmaBuffer;
    #endif
    size_t alignment = 0;
    if (esp_cache_get_alignment(caps, &alignment) != ESP_OK) return false;
    if (alignment < 4) alignment = 4;
    dma_buffer_ = heap_caps_aligned_alloc(alignment, bytes, caps);
    if (dma_buffer_ == nullptr) {
        ESP_LOGE(TAG, "SD transfer buffer allocation failed: bytes=%u caps=0x%lx", static_cast<unsigned>(bytes), static_cast<unsigned long>(caps));
        return false;
    }
    host.dma_aligned_buffer = dma_buffer_;
    host.unaligned_multi_block_rw_max_chunk_size = bytes / 512;
    ESP_LOGI(TAG, "SD transfer buffer ready: bytes=%u external=%d", static_cast<unsigned>(bytes), (caps & MALLOC_CAP_SPIRAM) != 0);
    return true;
}

void SdCardManager::ReleaseDmaBuffer() {
    heap_caps_free(dma_buffer_);
    dma_buffer_ = nullptr;
}

void SdCardManager::CallHostDeinit(const sdmmc_host_t* host) {
    if (host == nullptr) {
        return;
    }
    if (host->flags & SDMMC_HOST_FLAG_DEINIT_ARG) {
        if (host->deinit_p) {
            host->deinit_p(host->slot);
        }
    } else if (host->deinit) {
        host->deinit();
    }
}

sdmmc_slot_config_t SdCardManager::MakeSlotConfig() {
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = SDMMC_CLK_PIN;
    slot.cmd = SDMMC_CMD_PIN;
    slot.d0 = SDMMC_D0_PIN;
#if defined(SDMMC_D1_PIN)
    slot.d1 = SDMMC_D1_PIN;
    slot.d2 = SDMMC_D2_PIN;
    slot.d3 = SDMMC_D3_PIN;
    slot.width = 4;
    slot.flags = 0;
#else
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    slot.width = 1;
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
#endif
    return slot;
}

void SdCardManager::PreparePinsForMount() {
    const gpio_num_t pins[] = {
        SDMMC_CLK_PIN, SDMMC_CMD_PIN, SDMMC_D0_PIN,
#if defined(SDMMC_D1_PIN)
        SDMMC_D1_PIN, SDMMC_D2_PIN, SDMMC_D3_PIN,
#endif
    };
    for (gpio_num_t pin : pins) {
        gpio_hold_dis(pin);
    }
}

bool SdCardManager::Mount() {
    sd_access::Transition transition;
    if (!transition) return false;
    if (mounted_) {
        return true;
    }
    ReleaseRawCard();
    PreparePinsForMount();

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    if (!PrepareDmaBuffer(host)) return false;
    sdmmc_slot_config_t slot = MakeSlotConfig();
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 12,
        .allocation_unit_size = 16 * 1024,
    };

    ESP_LOGI(TAG, "mount SDMMC width=%d CLK=%d CMD=%d D0=%d", (int)slot.width, (int)SDMMC_CLK_PIN,
             (int)SDMMC_CMD_PIN, (int)SDMMC_D0_PIN);
    sdmmc_card_t* mounted_card = nullptr;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot, &mount_cfg, &mounted_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount fail: %s", esp_err_to_name(err));
        card_ = nullptr;
        mounted_ = false;
        ReleaseDmaBuffer();
        return false;
    }

    card_ = mounted_card;
    raw_owned_ = false;
    mounted_ = true;
    sdmmc_card_print_info(stdout, card_);
    if (!SdEnsureAppLayout()) {
        ESP_LOGW(TAG, "app layout under %s incomplete", SD_APP_ROOT);
    }
    SdLogWriter_OnSdMounted();
    ESP_LOGI(TAG, "SD ok at %s", kMountPoint);
    return true;
}

bool SdCardManager::Unmount() {
    sd_access::Transition transition;
    if (!transition) return false;
    SdLogWriter_OnSdUnmounting();
    if (!mounted_ || card_ == nullptr) {
        ReleaseRawCard();
        return true;
    }
    ESP_LOGI(TAG, "unmount SD");
    esp_err_t err = esp_vfs_fat_sdcard_unmount(kMountPoint, card_);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sdcard_unmount: %s", esp_err_to_name(err));
        return false;
    }
    card_ = nullptr;
    mounted_ = false;
    raw_owned_ = false;
    ReleaseDmaBuffer();
    return true;
}

sdmmc_card_t* SdCardManager::InitRawCardForMsc() {
    sd_access::Transition transition;
    if (!transition) return nullptr;
    if (mounted_) {
        if (!Unmount()) return nullptr;
    } else {
        ReleaseRawCard();
    }
    PreparePinsForMount();

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    if (!PrepareDmaBuffer(host)) return nullptr;
    sdmmc_slot_config_t slot = MakeSlotConfig();

    sdmmc_card_t* card = static_cast<sdmmc_card_t*>(calloc(1, sizeof(sdmmc_card_t)));
    if (card == nullptr) {
        ESP_LOGE(TAG, "calloc sdmmc_card_t failed");
        ReleaseDmaBuffer();
        return nullptr;
    }

    esp_err_t ret = (*host.init)();
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "host already inited, force deinit then retry");
        CallHostDeinit(&host);
        ret = (*host.init)();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SDMMC host init failed: %s", esp_err_to_name(ret));
        free(card);
        ReleaseDmaBuffer();
        return nullptr;
    }

    ret = sdmmc_host_init_slot(host.slot, &slot);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SDMMC slot init failed: %s", esp_err_to_name(ret));
        CallHostDeinit(&host);
        free(card);
        ReleaseDmaBuffer();
        return nullptr;
    }

    ret = sdmmc_card_init(&host, card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_card_init failed: %s", esp_err_to_name(ret));
        CallHostDeinit(&host);
        free(card);
        ReleaseDmaBuffer();
        return nullptr;
    }

    ret = sdmmc_get_status(card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_get_status failed: %s", esp_err_to_name(ret));
        CallHostDeinit(&card->host);
        free(card);
        ReleaseDmaBuffer();
        return nullptr;
    }

    card_ = card;
    raw_owned_ = true;
    mounted_ = false;
    sd_access::SetMounted(false);
    ESP_LOGI(TAG, "raw SDMMC card ready for MSC");
    return card_;
}

void SdCardManager::ReleaseRawCard() {
    sd_access::Transition transition;
    if (!transition) return;
    if (!raw_owned_ || card_ == nullptr) {
        if (!mounted_) {
            card_ = nullptr;
            ReleaseDmaBuffer();
        }
        raw_owned_ = false;
        return;
    }
    sd_access::SetMounted(false);
    ESP_LOGI(TAG, "release raw SDMMC card");
    CallHostDeinit(&card_.load()->host);
    free(card_);
    card_ = nullptr;
    raw_owned_ = false;
    mounted_ = false;
    ReleaseDmaBuffer();
}

void SdCardManager::NotifyExternalAppMount(sdmmc_card_t* card) {
    card_ = card;
    mounted_ = (card != nullptr);
    if (!mounted_) sd_access::SetMounted(false);
    if (mounted_) SdLogWriter_OnSdMounted();
    // TinyUSB only borrows our raw card; its APP mount does not transfer ownership.
}

void SdCardManager::NotifyExportedToHost() {
    sd_access::SetMounted(false);
    SdLogWriter_OnSdUnmounting();
    mounted_ = false;
}

bool SdCardManager::RemountVfsFromCard() {
    sd_access::Transition transition;
    if (!transition) return false;
    if (mounted_ && !raw_owned_) {
        return true;
    }
    ReleaseRawCard();
    return Mount();
}

bool SdCardManager::IsMounted() const {
    return mounted_;
}

bool SdCardManager::HasCard() const {
    return card_ != nullptr;
}

sdmmc_card_t* SdCardManager::GetCard() const {
    return card_;
}

const char* SdCardManager::GetMountPoint() const {
    return kMountPoint;
}
