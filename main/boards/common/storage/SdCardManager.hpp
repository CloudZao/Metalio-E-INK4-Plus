#pragma once

#include <cstdint>
#include <atomic>

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
    bool Unmount();

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

    /** @brief 查询挂载状态快照，实际文件访问仍须经过统一访问许可 */
    bool IsMounted() const;
    /** @brief 查询是否持有卡对象的状态快照 */
    bool HasCard() const;
    /**
     * @brief 查询最近一次初始化或挂载错误快照，成功或主动卸载后为 ESP_OK
     * @param mount_failed 可选输出，底层明确返回分区挂载失败时为 true
     */
    esp_err_t GetLastInitError(bool* mount_failed = nullptr) const {
        if (mount_failed) *mount_failed = last_mount_failed_.load(std::memory_order_relaxed);
        return last_init_error_.load(std::memory_order_relaxed);
    }
    /** @brief 本次启动是否发生过 SD 数据读写错误 */
    bool HasIoError() const { return io_error_.load(std::memory_order_relaxed); }
    /** @brief 获取借用的卡对象，仅在持有 sd_access::Transition 时使用 */
    sdmmc_card_t* GetCard() const;
    const char* GetMountPoint() const;

private:
    SdCardManager() = default;

    static void CallHostDeinit(const sdmmc_host_t* host);
    static sdmmc_slot_config_t MakeSlotConfig();
    static void PreparePinsForMount();
    static bool CheckDmaBuffer(int slot, const void* buffer, size_t size);
    static esp_err_t DoTransaction(int slot, sdmmc_command_t* command);
    bool PrepareDmaBuffer(sdmmc_host_t& host);
    void ReleaseDmaBuffer();

    std::atomic<sdmmc_card_t*> card_{nullptr};
    std::atomic<bool> mounted_{false};
    std::atomic<bool> last_mount_failed_{false}; // 区别文件系统挂载失败与原始卡初始化失败
    std::atomic<esp_err_t> last_init_error_{ESP_OK}; // 保留底层初始化错误供界面诊断
    std::atomic<bool> io_error_{false}; // 数据读写故障保持到重启，避免重复提示
    uint32_t init_freq_khz_ = SDMMC_FREQ_PROBING; // 当前识别尝试的时钟，失败后降至 100 kHz
    bool raw_owned_ = false;
    void* dma_buffer_ = nullptr; // 随卡生命周期复用的对齐传输缓冲
};
