#include "SdCardManager.hpp"

#include "config.h"

#include <cstdlib>
#include <cstring>

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <sdkconfig.h>

#define TAG "SdCardManager"

SdCardManager& SdCardManager::GetInstance() {
    static SdCardManager instance;
    return instance;
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
    if (mounted_) {
        return true;
    }
    ReleaseRawCard();
    PreparePinsForMount();

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = MakeSlotConfig();
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 12,
        .allocation_unit_size = 16 * 1024,
    };

    ESP_LOGI(TAG, "mount SDMMC width=%d CLK=%d CMD=%d D0=%d", (int)slot.width, (int)SDMMC_CLK_PIN,
             (int)SDMMC_CMD_PIN, (int)SDMMC_D0_PIN);
    esp_err_t err = esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot, &mount_cfg, &card_);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount fail: %s", esp_err_to_name(err));
        card_ = nullptr;
        mounted_ = false;
        return false;
    }

    raw_owned_ = false;
    mounted_ = true;
    sdmmc_card_print_info(stdout, card_);
    if (!SdEnsureAppLayout()) {
        ESP_LOGW(TAG, "app layout under %s incomplete", SD_APP_ROOT);
    }
    ESP_LOGI(TAG, "SD ok at %s", kMountPoint);
    return true;
}

void SdCardManager::Unmount() {
    if (!mounted_ || card_ == nullptr) {
        ReleaseRawCard();
        return;
    }
    ESP_LOGI(TAG, "unmount SD");
    esp_err_t err = esp_vfs_fat_sdcard_unmount(kMountPoint, card_);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sdcard_unmount: %s", esp_err_to_name(err));
    }
    card_ = nullptr;
    mounted_ = false;
    raw_owned_ = false;
}

sdmmc_card_t* SdCardManager::InitRawCardForMsc() {
    if (mounted_) {
        Unmount();
    } else {
        ReleaseRawCard();
    }
    PreparePinsForMount();

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = MakeSlotConfig();

    sdmmc_card_t* card = static_cast<sdmmc_card_t*>(calloc(1, sizeof(sdmmc_card_t)));
    if (card == nullptr) {
        ESP_LOGE(TAG, "calloc sdmmc_card_t failed");
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
        return nullptr;
    }

    ret = sdmmc_host_init_slot(host.slot, &slot);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SDMMC slot init failed: %s", esp_err_to_name(ret));
        CallHostDeinit(&host);
        free(card);
        return nullptr;
    }

    ret = sdmmc_card_init(&host, card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_card_init failed: %s", esp_err_to_name(ret));
        CallHostDeinit(&host);
        free(card);
        return nullptr;
    }

    ret = sdmmc_get_status(card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_get_status failed: %s", esp_err_to_name(ret));
        CallHostDeinit(&card->host);
        free(card);
        return nullptr;
    }

    card_ = card;
    raw_owned_ = true;
    mounted_ = false;
    ESP_LOGI(TAG, "raw SDMMC card ready for MSC");
    return card_;
}

void SdCardManager::ReleaseRawCard() {
    if (!raw_owned_ || card_ == nullptr) {
        if (!mounted_) {
            card_ = nullptr;
        }
        raw_owned_ = false;
        return;
    }
    ESP_LOGI(TAG, "release raw SDMMC card");
    CallHostDeinit(&card_->host);
    free(card_);
    card_ = nullptr;
    raw_owned_ = false;
    mounted_ = false;
}

void SdCardManager::NotifyExternalAppMount(sdmmc_card_t* card) {
    card_ = card;
    mounted_ = (card != nullptr);
    raw_owned_ = false;
}

void SdCardManager::NotifyExportedToHost() {
    mounted_ = false;
}

bool SdCardManager::RemountVfsFromCard() {
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
