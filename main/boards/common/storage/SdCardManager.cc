#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <new>
#include <driver/gpio.h>
#include <driver/sdmmc_defs.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <esp_private/esp_cache_private.h>
#include <esp_vfs_fat.h>
#include <sdkconfig.h>
#include <soc/soc_caps.h>
#include "SdCardManager.hpp"
#include "config.h"
#include "sd_access.h"
#include "infra/storage/sd_data_layout.h"
#include "sd_log_writer.h"
#include "sysmgmt/tasks/task_executor.h"

#define TAG "SdCardManager"

static constexpr uint32_t kSdInitFallbackKhz = 100;  // 探测时钟失败后的识别频率

static void ReportMountFailure(esp_err_t error) {
    const auto code = error == ESP_ERR_TIMEOUT ? StorageErrorCode::Timeout :
                      error == ESP_ERR_NO_MEM ? StorageErrorCode::NoMemory : StorageErrorCode::Io;
    SdAccessSetMounted(false, {code, StorageStage::Mount, 0, error});
}

SdCardManager& SdCardManager::GetInstance() {
    static SdCardManager instance;
    return instance;
}

std::shared_ptr<const SdCapacityResult> SdCardManager::QueryCapacityAsync() try {
    auto request = std::make_shared<SdCapacityResult>();
    request->mount_generation = SdAccessMountGeneration();
    if (!TaskExecutor::Schedule([request] {
        SdAccessTransaction transaction;
        if (transaction && request->mount_generation == SdAccessMountGeneration()) {
            const esp_err_t result = esp_vfs_fat_info(SD_MOUNT_POINT, &request->total_bytes, &request->free_bytes);
            request->ok = result == ESP_OK;
            if (!request->ok) {
                request->error = {StorageErrorCode::Io, StorageStage::Capacity, 0, result};
                LogStorageError(TAG, request->error);
            }
        }
        if (transaction && request->mount_generation != SdAccessMountGeneration()) {
            request->error = {StorageErrorCode::Cancelled, StorageStage::Capacity};
        }
        if (!transaction) {
            request->error = transaction.Error();
            LogStorageError(TAG, request->error);
        }
        request->ready.store(true, std::memory_order_release);
    }, TaskDomain::Storage)) {
        return {};
    }
    return request;
} catch (const std::bad_alloc&) {
    ESP_LOGE(TAG, "operation=capacity stage=allocate result=no_memory");
    return {};
}

bool SdCardManager::CheckDmaBuffer(int slot, const void* buffer, size_t size) {
    return esp_ptr_internal(buffer) && sdmmc_host_check_buffer_alignment(slot, buffer, size);
}

esp_err_t SdCardManager::DoTransaction(int slot, sdmmc_command_t* command) {
    // 每次识别使用本次尝试的时钟，正常读写频率仍由 SDK 协商。
    if (command->opcode == MMC_GO_IDLE_STATE) {
        const esp_err_t clock_result = sdmmc_host_set_card_clk(slot, GetInstance().init_freq_khz_);
        if (clock_result != ESP_OK) {
            ESP_LOGE(TAG, "SD initialization clock configuration failed code=0x%x", static_cast<unsigned>(clock_result));
            return clock_result;
        }
    }
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
    const esp_err_t alignment_error = esp_cache_get_alignment(caps, &alignment);
    if (alignment_error != ESP_OK) {
        ReportMountFailure(alignment_error);
        ESP_LOGE(TAG, "SD transfer buffer alignment failed code=0x%x", static_cast<unsigned>(alignment_error));
        return false;
    }
    if (alignment < 4) {
        alignment = 4;
    }
    dma_buffer_ = heap_caps_aligned_alloc(alignment, bytes, caps);
    if (dma_buffer_ == nullptr) {
        ReportMountFailure(ESP_ERR_NO_MEM);
        ESP_LOGE(TAG, "SD transfer buffer allocation failed: bytes=%u caps=0x%lx", static_cast<unsigned>(bytes),
                 static_cast<unsigned long>(caps));
        return false;
    }
    host.dma_aligned_buffer = dma_buffer_;
    host.unaligned_multi_block_rw_max_chunk_size = bytes / 512;
    ESP_LOGI(TAG, "SD transfer buffer ready: bytes=%u external=%d", static_cast<unsigned>(bytes),
             (caps & MALLOC_CAP_SPIRAM) != 0);
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
#else
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    slot.width = 1;
#endif
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    return slot;
}

void SdCardManager::PreparePinsForMount() {
    const gpio_num_t pins[] = {
        SDMMC_CLK_PIN, SDMMC_CMD_PIN, SDMMC_D0_PIN,
#if defined(SDMMC_D1_PIN)
        SDMMC_D1_PIN,  SDMMC_D2_PIN,  SDMMC_D3_PIN,
#endif
    };
    for (gpio_num_t pin : pins) {
        gpio_hold_dis(pin);
    }
}

bool SdCardManager::Mount() {
    SdAccessTransition transition;
    if (!transition) {
        return false;
    }
    if (mounted_) {
        return true;
    }
    ReleaseRawCard();
    PreparePinsForMount();

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    if (!PrepareDmaBuffer(host)) {
        return false;
    }
    sdmmc_slot_config_t slot = MakeSlotConfig();
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 12,
        .allocation_unit_size = 16 * 1024,
    };

    ESP_LOGI(TAG, "mount SDMMC width=%d CLK=%d CMD=%d D0=%d", (int)slot.width, (int)SDMMC_CLK_PIN, (int)SDMMC_CMD_PIN,
             (int)SDMMC_D0_PIN);
    sdmmc_card_t* mounted_card = nullptr;
    init_freq_khz_ = SDMMC_FREQ_PROBING;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot, &mount_cfg, &mounted_card);
    if (err != ESP_OK && err != ESP_FAIL && err != ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "SD initialization failed code=0x%x freq_khz=%lu; retry freq_khz=%lu",
                 static_cast<unsigned>(err), static_cast<unsigned long>(init_freq_khz_),
                 static_cast<unsigned long>(kSdInitFallbackKhz));
        init_freq_khz_ = kSdInitFallbackKhz;
        err = esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot, &mount_cfg, &mounted_card);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount fail: %s", esp_err_to_name(err));
        card_ = nullptr;
        mounted_ = false;
        ReportMountFailure(err);
        ReleaseDmaBuffer();
        return false;
    }

    card_ = mounted_card;
    raw_owned_ = false;
    mounted_ = true;
    sdmmc_card_print_info(stdout, card_);
    static bool prepared = false;
    if (!prepared) {
        SdDataPrepare();
        prepared = true;
    } else if (!SdEnsureAppLayout()) {
        ESP_LOGW(TAG, "app layout incomplete");
    }
    SdLogWriter_OnSdMounted();
    ESP_LOGI(TAG, "SD mounted init_khz=%lu", static_cast<unsigned long>(init_freq_khz_));
    return true;
}

bool SdCardManager::Unmount() {
    SdAccessTransition transition;
    if (!transition) {
        return false;
    }
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
    SdAccessTransition transition;
    if (!transition) {
        return nullptr;
    }
    if (mounted_) {
        if (!Unmount()) {
            return nullptr;
        }
    } else {
        ReleaseRawCard();
    }
    PreparePinsForMount();

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    if (!PrepareDmaBuffer(host)) {
        return nullptr;
    }
    sdmmc_slot_config_t slot = MakeSlotConfig();

    sdmmc_card_t* card = static_cast<sdmmc_card_t*>(calloc(1, sizeof(sdmmc_card_t)));
    if (card == nullptr) {
        ReportMountFailure(ESP_ERR_NO_MEM);
        ESP_LOGE(TAG, "calloc sdmmc_card_t failed");
        ReleaseDmaBuffer();
        return nullptr;
    }

    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < 2; ++attempt) {
        init_freq_khz_ = attempt == 0 ? SDMMC_FREQ_PROBING : kSdInitFallbackKhz;
        ret = (*host.init)();
        if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "host already inited, force deinit then retry");
            CallHostDeinit(&host);
            ret = (*host.init)();
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SDMMC host init failed code=0x%x", static_cast<unsigned>(ret));
            break;
        }

        ret = sdmmc_host_init_slot(host.slot, &slot);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SDMMC slot init failed code=0x%x", static_cast<unsigned>(ret));
            CallHostDeinit(&host);
            break;
        }

        ret = sdmmc_card_init(&host, card);
        if (ret == ESP_OK) {
            break;
        }
        CallHostDeinit(&host);
        if (attempt == 0 && ret != ESP_ERR_NO_MEM) {
            ESP_LOGW(TAG, "SD initialization failed code=0x%x freq_khz=%lu; retry freq_khz=%lu",
                     static_cast<unsigned>(ret), static_cast<unsigned long>(init_freq_khz_),
                     static_cast<unsigned long>(kSdInitFallbackKhz));
        } else {
            ESP_LOGE(TAG, "sdmmc_card_init failed code=0x%x", static_cast<unsigned>(ret));
            break;
        }
    }
    if (ret != ESP_OK) {
        ReportMountFailure(ret);
        free(card);
        ReleaseDmaBuffer();
        return nullptr;
    }

    ret = sdmmc_get_status(card);
    if (ret != ESP_OK) {
        ReportMountFailure(ret);
        ESP_LOGE(TAG, "sdmmc_get_status failed: %s", esp_err_to_name(ret));
        CallHostDeinit(&card->host);
        free(card);
        ReleaseDmaBuffer();
        return nullptr;
    }

    card_ = card;
    raw_owned_ = true;
    mounted_ = false;
    SdAccessSetMounted(false);
    ESP_LOGI(TAG, "raw SDMMC card ready for MSC init_khz=%lu", static_cast<unsigned long>(init_freq_khz_));
    return card_;
}

void SdCardManager::ReleaseRawCard() {
    SdAccessTransition transition;
    if (!transition) {
        return;
    }
    if (!raw_owned_ || card_ == nullptr) {
        if (!mounted_) {
            card_ = nullptr;
            ReleaseDmaBuffer();
        }
        raw_owned_ = false;
        return;
    }
    SdAccessSetMounted(false);
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
    if (!mounted_) {
        SdAccessSetMounted(false);
    }
    if (mounted_) {
        SdLogWriter_OnSdMounted();
    }
    // TinyUSB only borrows our raw card; its APP mount does not transfer ownership.
}

void SdCardManager::NotifyExportedToHost() {
    SdAccessSetMounted(false, {StorageErrorCode::UsbOwned, StorageStage::Access, EBUSY});
    SdLogWriter_OnSdUnmounting();
    mounted_ = false;
}

bool SdCardManager::RemountVfsFromCard() {
    SdAccessTransition transition;
    if (!transition) {
        return false;
    }
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
