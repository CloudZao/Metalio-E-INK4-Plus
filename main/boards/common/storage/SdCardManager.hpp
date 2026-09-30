#pragma once

#include <cstdint>

#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>

#include "sd_paths.h"

/**
 * @brief SDMMC 单例：VFS 挂载 + MSC raw card
 * @note 挂载点 SD_MOUNT_POINT；板级引脚来自 config.h
 */
class SdCardManager {
public:
    static constexpr const char* kMountPoint = SD_MOUNT_POINT;

    static SdCardManager& GetInstance();

    SdCardManager(const SdCardManager&) = delete;
    SdCardManager& operator=(const SdCardManager&) = delete;

    /** @brief 挂载 FAT 到 SD_MOUNT_POINT（可重复调用） */
    bool Mount();
    /** @brief 卸载 VFS；若持有 raw card 一并释放 */
    void Unmount();

    /**
     * @brief 卸 VFS 后重新 sdmmc_card_init，供 TinyUSB MSC 使用
     * @return raw card，失败为 nullptr
     */
    sdmmc_card_t* InitRawCardForMsc();
    /** @brief 释放 InitRawCardForMsc 持有的 card */
    void ReleaseRawCard();

    /** @brief MSC 把卡交回本机 APP 挂载时同步标志 */
    void NotifyExternalAppMount(sdmmc_card_t* card);
    /** @brief 卡已导出给 USB 主机（本机 VFS 不可用） */
    void NotifyExportedToHost();
    /** @brief USB 停用后尽量恢复本机 VFS */
    bool RemountVfsFromCard();

    bool IsMounted() const;
    bool HasCard() const;
    sdmmc_card_t* GetCard() const;
    const char* GetMountPoint() const;

private:
    SdCardManager() = default;

    static void CallHostDeinit(const sdmmc_host_t* host);
    static sdmmc_slot_config_t MakeSlotConfig();
    static void PreparePinsForMount();

    sdmmc_card_t* card_ = nullptr;
    bool mounted_ = false;
    bool raw_owned_ = false;
};
