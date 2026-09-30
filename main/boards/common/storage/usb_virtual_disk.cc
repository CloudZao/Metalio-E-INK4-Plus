#include "usb_virtual_disk.h"

#include "assets/lang_config.h"
#include "esp_log.h"

#if (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31) && \
    defined(CONFIG_TINYUSB_MSC_ENABLED)

#include <atomic>
#include <cstring>
#include <mutex>

#include "SdCardManager.hpp"
#include "book_library.h"
#include "power_policy.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
#include "hal/usb_serial_jtag_ll.h"
#endif
#if CONFIG_IDF_TARGET_ESP32P4
#include "hal/usb_wrap_ll.h"
#include "soc/usb_wrap_struct.h"
#endif
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

#if __has_include("config.h")
#include "config.h"
#endif
#if CONFIG_IDF_TARGET_ESP32S31
#include "epd_board_metalio_eink4_plus.h"
#endif
#if CONFIG_IDF_TARGET_ESP32S3
// S3 内置 USB OTG FS：D-=GPIO19、D+=GPIO20（与 USJ 共用）
#ifndef USB_OTG_DM_PIN
#define USB_OTG_DM_PIN GPIO_NUM_19
#endif
#ifndef USB_OTG_DP_PIN
#define USB_OTG_DP_PIN GPIO_NUM_20
#endif
#elif CONFIG_IDF_TARGET_ESP32P4
#ifndef USB_OTG_DM_PIN
#define USB_OTG_DM_PIN GPIO_NUM_24
#endif
#ifndef USB_OTG_DP_PIN
#define USB_OTG_DP_PIN GPIO_NUM_25
#endif
#endif
// ESP32-S31：HS UTMI 内置 PHY，无 GPIO D+/D-，TinyUSB 自行初始化

namespace {

constexpr const char* TAG = "UsbVirtualDisk";

// 启用期间挡住无操作进待机；与 s_gadget_active 对齐，避免重复 Acquire/Release
std::atomic<bool> s_standby_block_held{false};

void SetStandbyBlock(bool hold)
{
    const bool prev = s_standby_block_held.exchange(hold, std::memory_order_relaxed);
    if (prev == hold) {
        return;
    }
    if (hold) {
        PowerPolicy::GetInstance().Acquire(PowerNeed::UsbVirtualDisk);
    } else {
        PowerPolicy::GetInstance().Release(PowerNeed::UsbVirtualDisk);
    }
}

enum UsbReq : int {
    kReqEnableGadget = 0,
    kReqDisableGadget,
    kReqForceDisableGadget,  // 离开页面：重试后仍强制拆栈恢复 USJ
    kReqSdToUsb,
    kReqSdToApp,
};

tinyusb_msc_storage_handle_t s_storage_hdl = nullptr;
QueueHandle_t s_usb_req_queue = nullptr;
#if !CONFIG_IDF_TARGET_ESP32S31
usb_phy_handle_t s_phy_hdl = nullptr;
#endif
sdmmc_card_t* s_sd_card = nullptr;

std::atomic<bool> s_gadget_active{false};
std::atomic<bool> s_sd_to_usb{false};
std::atomic<bool> s_op_in_progress{false};
std::atomic<bool> s_usb_switching{false};
std::atomic<bool> s_inited{false};
std::atomic<bool> s_want_active{false};  // 期望启用态；Toggle 改它，worker 结束后再对齐
std::atomic<bool> s_reconcile_pending{false};  // 仅忙时改期望后置位，避免 DisableFailed 死循环重试
std::atomic<int> s_ui_hint{static_cast<int>(UsbVirtualDisk::UiHint::Idle)};

std::mutex s_notify_mu;
UsbVirtualDisk::UiNotifyFn s_ui_notify;

constexpr int kTusbDescTotalLen = TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN;

enum {
    kItfNumMsc = 0,
    kItfNumTotal,
};

enum {
    kEdptCtrlOut = 0x00,
    kEdptCtrlIn = 0x80,
    kEdptMscOut = 0x01,
    kEdptMscIn = 0x81,
};

const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(s_device_desc),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x4002,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

const uint8_t s_msc_fs_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, kItfNumTotal, 0, kTusbDescTotalLen,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(kItfNumMsc, 0, kEdptMscOut, kEdptMscIn, 64),
};

#if CONFIG_IDF_TARGET_ESP32S31
const uint8_t s_msc_hs_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, kItfNumTotal, 0, kTusbDescTotalLen,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(kItfNumMsc, 0, kEdptMscOut, kEdptMscIn, 512),
};
#endif

const char* const s_string_desc[] = {
    (const char[]){0x09, 0x04},
    "Metalio",
#if CONFIG_IDF_TARGET_ESP32S3
    "ESP32-S3 SD Reader",
#elif CONFIG_IDF_TARGET_ESP32S31
    "ESP32-S31 SD Reader",
#else
    "ESP32-P4 SD Reader",
#endif
    "123456",
    "MSC",
};

char s_msc_base_path[] = "/sdcard";

void SetHint(UsbVirtualDisk::UiHint hint) {
    s_ui_hint.store(static_cast<int>(hint), std::memory_order_relaxed);
}

void NotifyUi() {
    UsbVirtualDisk::UiNotifyFn fn;
    {
        std::lock_guard<std::mutex> lock(s_notify_mu);
        fn = s_ui_notify;
    }
    if (fn) {
        fn();
    }
}

// 启停请求可覆盖队列；挂载切换绝不能盖掉启停。
void PostGadgetRequest(UsbReq req) {
    if (s_usb_req_queue == nullptr) {
        return;
    }
    xQueueOverwrite(s_usb_req_queue, &req);
}

void PostMountRequest(UsbReq req) {
    if (s_usb_req_queue == nullptr) {
        return;
    }
    // 队列非空（多半是启停）时丢掉挂载请求，避免反复枚举冲掉 Disable。
    xQueueSend(s_usb_req_queue, &req, 0);
}

void PostReconcileLocked() {
    if (s_usb_req_queue == nullptr) {
        return;
    }
    const bool want = s_want_active.load(std::memory_order_relaxed);
    const bool active = s_gadget_active.load(std::memory_order_relaxed);
    if (want == active) {
        return;
    }
    if (want) {
        PostGadgetRequest(kReqEnableGadget);
    } else {
        PostGadgetRequest(kReqDisableGadget);
    }
}

void RoutePhyToOtg() {
#if CONFIG_IDF_TARGET_ESP32S31
    // S31：HS UTMI 与 USJ 分离，TinyUSB 管 PHY，此处无操作
    ESP_LOGI(TAG, "PHY: S31 UTMI (TinyUSB managed)");
#elif CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    // S3：USJ 与 USB OTG 共用内置 FSLS PHY，靠 RTCCNTL.sw_usb_phy_sel 切换。
    usb_serial_jtag_ll_phy_enable_pad(false);
#if CONFIG_IDF_TARGET_ESP32S3
    usb_serial_jtag_ll_phy_enable_external(true);
#elif CONFIG_IDF_TARGET_ESP32P4
    usb_wrap_ll_phy_select(&USB_WRAP, 0);
#endif
    gpio_set_drive_capability(USB_OTG_DM_PIN, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(USB_OTG_DP_PIN, GPIO_DRIVE_CAP_3);
    ESP_LOGI(TAG, "PHY: OTG -> GPIO%d/GPIO%d (USJ paused)",
             static_cast<int>(USB_OTG_DM_PIN), static_cast<int>(USB_OTG_DP_PIN));
#endif
}

void RoutePhyToUsj() {
#if CONFIG_IDF_TARGET_ESP32S31
    ESP_LOGI(TAG, "PHY: S31 USJ untouched (separate from UTMI)");
#elif CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
#if CONFIG_IDF_TARGET_ESP32S3
    usb_serial_jtag_ll_phy_enable_external(false);
#elif CONFIG_IDF_TARGET_ESP32P4
    usb_wrap_ll_phy_select(&USB_WRAP, 1);
    usb_serial_jtag_ll_phy_set_defaults();
#endif
    usb_serial_jtag_ll_phy_enable_pad(true);
    ESP_LOGI(TAG, "PHY: USB Serial/JTAG -> GPIO%d/GPIO%d",
             static_cast<int>(USB_OTG_DM_PIN), static_cast<int>(USB_OTG_DP_PIN));
#endif
}

void SyncMountFlagsFromStorage() {
    if (s_storage_hdl == nullptr) {
        s_sd_to_usb.store(false, std::memory_order_relaxed);
        return;
    }
    tinyusb_msc_mount_point_t mp;
    if (tinyusb_msc_get_storage_mount_point(s_storage_hdl, &mp) != ESP_OK) {
        return;
    }
    s_sd_to_usb.store(mp == TINYUSB_MSC_STORAGE_MOUNT_USB, std::memory_order_relaxed);
}

void SyncSdCardManagerFlags() {
    auto& sd = SdCardManager::GetInstance();
    if (s_sd_to_usb.load(std::memory_order_relaxed)) {
        sd.NotifyExportedToHost();
    } else if (s_sd_card != nullptr) {
        sd.NotifyExternalAppMount(s_sd_card);
    }
}

void SyncUiFromState() {
    if (s_gadget_active.load(std::memory_order_relaxed)) {
        if (s_sd_to_usb.load(std::memory_order_relaxed)) {
            SetHint(UsbVirtualDisk::UiHint::EnabledHost);
        } else {
            SetHint(UsbVirtualDisk::UiHint::EnabledLocal);
        }
    } else {
        SetHint(UsbVirtualDisk::UiHint::Disabled);
    }
    SyncSdCardManagerFlags();
    NotifyUi();
}

// PC 可能已改书库；交还本机后作废缓存，下次进阅读 Load 再扫盘。
void InvalidateBookLibraryAfterHostExport() {
    reader::InvalidateBookLibraryCache();
}

esp_err_t MountToApp() {
    if (s_storage_hdl == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    tinyusb_msc_mount_point_t mp;
    ESP_RETURN_ON_ERROR(tinyusb_msc_get_storage_mount_point(s_storage_hdl, &mp), TAG,
                        "get mount point failed");
    if (mp == TINYUSB_MSC_STORAGE_MOUNT_APP) {
        s_sd_to_usb.store(false, std::memory_order_relaxed);
        SyncSdCardManagerFlags();
        return ESP_OK;
    }
    ESP_LOGI(TAG, "SD -> APP");
    esp_err_t err =
        tinyusb_msc_set_storage_mount_point(s_storage_hdl, TINYUSB_MSC_STORAGE_MOUNT_APP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount APP failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_RETURN_ON_ERROR(tinyusb_msc_get_storage_mount_point(s_storage_hdl, &mp), TAG,
                        "get mount point failed");
    if (mp != TINYUSB_MSC_STORAGE_MOUNT_APP) {
        return ESP_FAIL;
    }
    s_sd_to_usb.store(false, std::memory_order_relaxed);
    SyncSdCardManagerFlags();
    InvalidateBookLibraryAfterHostExport();
    return ESP_OK;
}

esp_err_t MountToUsb() {
    if (s_storage_hdl == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    tinyusb_msc_mount_point_t mp;
    ESP_RETURN_ON_ERROR(tinyusb_msc_get_storage_mount_point(s_storage_hdl, &mp), TAG,
                        "get mount point failed");
    if (mp == TINYUSB_MSC_STORAGE_MOUNT_USB) {
        s_sd_to_usb.store(true, std::memory_order_relaxed);
        SyncSdCardManagerFlags();
        return ESP_OK;
    }
    ESP_LOGI(TAG, "SD -> USB host");
    esp_err_t err =
        tinyusb_msc_set_storage_mount_point(s_storage_hdl, TINYUSB_MSC_STORAGE_MOUNT_USB);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount USB failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_RETURN_ON_ERROR(tinyusb_msc_get_storage_mount_point(s_storage_hdl, &mp), TAG,
                        "get mount point failed");
    if (mp != TINYUSB_MSC_STORAGE_MOUNT_USB) {
        return ESP_FAIL;
    }
    s_sd_to_usb.store(true, std::memory_order_relaxed);
    SyncSdCardManagerFlags();
    return ESP_OK;
}

void StorageMountChangedCb(tinyusb_msc_storage_handle_t /*handle*/, tinyusb_msc_event_t* event,
                           void* /*arg*/) {
    if (event == nullptr) {
        return;
    }
    if (event->id == TINYUSB_MSC_EVENT_FORMAT_REQUIRED) {
        ESP_LOGE(TAG, "SD needs FAT32 format");
        SetHint(UsbVirtualDisk::UiHint::FormatRequired);
        NotifyUi();
        return;
    }
    if (event->id == TINYUSB_MSC_EVENT_MOUNT_FAILED) {
        ESP_LOGE(TAG, "SD mount/unmount failed");
        SyncMountFlagsFromStorage();
        SyncSdCardManagerFlags();
        if (!s_usb_switching.load(std::memory_order_relaxed)) {
            SetHint(UsbVirtualDisk::UiHint::HostBusy);
            NotifyUi();
        }
        return;
    }
    if (event->id != TINYUSB_MSC_EVENT_MOUNT_COMPLETE) {
        return;
    }
    s_sd_to_usb.store(event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB,
                      std::memory_order_relaxed);
    SyncSdCardManagerFlags();
    if (!s_usb_switching.load(std::memory_order_relaxed) &&
        s_gadget_active.load(std::memory_order_relaxed)) {
        SyncUiFromState();
    }
}

void UsbEventCb(tinyusb_event_t* event, void* /*arg*/) {
    if (event == nullptr || s_usb_switching.load(std::memory_order_relaxed) ||
        !s_gadget_active.load(std::memory_order_relaxed)) {
        return;
    }
    switch (event->id) {
        case TINYUSB_EVENT_ATTACHED:
            ESP_LOGI(TAG, "USB attached");
            PostMountRequest(kReqSdToUsb);
            break;
        case TINYUSB_EVENT_DETACHED:
            ESP_LOGI(TAG, "USB detached");
            PostMountRequest(kReqSdToApp);
            break;
        default:
            break;
    }
}

esp_err_t EnsureMscStorage() {
    if (s_storage_hdl != nullptr) {
        return ESP_OK;
    }

    auto& sd = SdCardManager::GetInstance();
    // 无卡可挂时直接失败（避免空 init）
    if (!sd.IsMounted() && !sd.HasCard()) {
        ESP_LOGE(TAG, "no SD card present");
        SetHint(UsbVirtualDisk::UiHint::NoSdCard);
        NotifyUi();
        return ESP_ERR_INVALID_STATE;
    }

    // unmount 后原 card 已 free，必须重新 sdmmc_card_init。
    sdmmc_card_t* card = sd.InitRawCardForMsc();
    if (card == nullptr) {
        ESP_LOGE(TAG, "InitRawCardForMsc failed");
        SetHint(UsbVirtualDisk::UiHint::NoSdCard);
        NotifyUi();
        // 尽量恢复本机浏览
        sd.RemountVfsFromCard();
        return ESP_ERR_INVALID_STATE;
    }
    s_sd_card = card;

    tinyusb_msc_driver_config_t msc_driver_cfg = {};
    msc_driver_cfg.user_flags.auto_mount_off = 1;
    msc_driver_cfg.callback = StorageMountChangedCb;
    msc_driver_cfg.callback_arg = nullptr;

    esp_err_t err = tinyusb_msc_install_driver(&msc_driver_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "msc install driver failed: %s", esp_err_to_name(err));
        s_sd_card = nullptr;
        sd.RemountVfsFromCard();
        return err;
    }

    tinyusb_msc_storage_config_t storage_cfg = {};
    storage_cfg.mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP;
    storage_cfg.fat_fs.base_path = s_msc_base_path;
    storage_cfg.fat_fs.config.max_files = 5;
    storage_cfg.fat_fs.config.format_if_mount_failed = false;
    storage_cfg.fat_fs.do_not_format = true;
    storage_cfg.medium.card = s_sd_card;

    err = tinyusb_msc_new_storage_sdmmc(&storage_cfg, &s_storage_hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc new storage failed: %s", esp_err_to_name(err));
        s_storage_hdl = nullptr;
        s_sd_card = nullptr;
        sd.RemountVfsFromCard();
        return err;
    }

    sd.NotifyExternalAppMount(s_sd_card);
    ESP_LOGI(TAG, "MSC storage ready at %s (APP)", s_msc_base_path);
    return ESP_OK;
}

esp_err_t UsbGadgetTeardownUnlocked() {
    esp_err_t first_err = ESP_OK;
    if (tinyusb_driver_uninstall() != ESP_OK) {
        ESP_LOGW(TAG, "tinyusb_driver_uninstall failed (maybe not installed)");
    }
#if !CONFIG_IDF_TARGET_ESP32S31
    if (s_phy_hdl != nullptr) {
        esp_err_t err = usb_del_phy(s_phy_hdl);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "usb_del_phy failed: %s", esp_err_to_name(err));
            if (first_err == ESP_OK) {
                first_err = err;
            }
        }
        s_phy_hdl = nullptr;
    }
#endif
    // 先断连再切回 USJ，给主机时间卸掉 MSC，再重新枚举调试口。
    vTaskDelay(pdMS_TO_TICKS(150));
    RoutePhyToUsj();
    vTaskDelay(pdMS_TO_TICKS(100));
    s_gadget_active.store(false, std::memory_order_relaxed);
    s_sd_to_usb.store(false, std::memory_order_relaxed);
    SetStandbyBlock(false);
#if CONFIG_IDF_TARGET_ESP32S31
    epd_board_metalio_eink4_plus_ana_sw_set(true); // 关闭虚拟 U 盘：ANA_SW(GPIO36) 置高
#endif
    SyncSdCardManagerFlags();
    return first_err;
}

esp_err_t MountToAppWithRetry(int attempts, uint32_t delay_ms) {
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < attempts; ++i) {
        err = MountToApp();
        if (err == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "MountToApp retry %d/%d failed: %s", i + 1, attempts,
                 esp_err_to_name(err));
        if (i + 1 < attempts) {
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }
    return err;
}

// USB 已拆后收回 SD：先走 MSC→APP，失败再删 storage 本机 remount。
esp_err_t ReclaimSdAfterUsbDown() {
    if (s_storage_hdl != nullptr) {
        if (MountToAppWithRetry(5, 100) == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "MountToApp after USB down failed, rebuild VFS");
        if (tinyusb_msc_delete_storage(s_storage_hdl) != ESP_OK) {
            ESP_LOGW(TAG, "tinyusb_msc_delete_storage failed; keep MSC handle");
            SyncMountFlagsFromStorage();
            SyncSdCardManagerFlags();
            return ESP_FAIL;
        }
        s_storage_hdl = nullptr;
        s_sd_card = nullptr;
        if (tinyusb_msc_uninstall_driver() != ESP_OK) {
            ESP_LOGW(TAG, "tinyusb_msc_uninstall_driver failed");
        }
    }
    auto& sd = SdCardManager::GetInstance();
    if (!sd.RemountVfsFromCard()) {
        ESP_LOGE(TAG, "RemountVfsFromCard failed after USB down");
        return ESP_FAIL;
    }
    // MountToApp 未成功时上面不会 Invalidate；重建 VFS 后同样作废书库缓存
    InvalidateBookLibraryAfterHostExport();
    return ESP_OK;
}

esp_err_t UsbGadgetStart() {
    esp_err_t err = EnsureMscStorage();
    if (err != ESP_OK) {
        return err;
    }

    if (s_gadget_active.load(std::memory_order_relaxed)) {
        err = MountToUsb();
        SyncUiFromState();
        return err;
    }

    s_usb_switching.store(true, std::memory_order_relaxed);
    RoutePhyToOtg();

#if CONFIG_IDF_TARGET_ESP32S31
    // S31：HS UTMI，TinyUSB 默认建 PHY（skip_setup=false）
    tinyusb_config_t tusb_cfg = TINYUSB_CONFIG_HIGH_SPEED(UsbEventCb, nullptr);
    tusb_cfg.task =
        TINYUSB_TASK_CUSTOM(TINYUSB_DEFAULT_TASK_SIZE, TINYUSB_DEFAULT_TASK_PRIO, 0);
    tusb_cfg.descriptor.device = &s_device_desc;
    tusb_cfg.descriptor.full_speed_config = s_msc_fs_config_desc;
    tusb_cfg.descriptor.high_speed_config = s_msc_hs_config_desc;
    tusb_cfg.descriptor.string = (const char**)s_string_desc;
    tusb_cfg.descriptor.string_count = sizeof(s_string_desc) / sizeof(s_string_desc[0]);

    err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
        RoutePhyToUsj();
        s_usb_switching.store(false, std::memory_order_relaxed);
        return err;
    }
#else
    usb_phy_config_t phy_conf = {};
    phy_conf.controller = USB_PHY_CTRL_OTG;
    phy_conf.target = USB_PHY_TARGET_INT;
    phy_conf.otg_mode = USB_OTG_MODE_DEVICE;
    phy_conf.otg_speed = USB_PHY_SPEED_FULL;
    err = usb_new_phy(&phy_conf, &s_phy_hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_new_phy failed: %s", esp_err_to_name(err));
        RoutePhyToUsj();
        s_usb_switching.store(false, std::memory_order_relaxed);
        return err;
    }

    gpio_set_drive_capability(USB_OTG_DM_PIN, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(USB_OTG_DP_PIN, GPIO_DRIVE_CAP_3);

    tinyusb_config_t tusb_cfg = TINYUSB_CONFIG_FULL_SPEED(UsbEventCb, nullptr);
    tusb_cfg.phy.skip_setup = true;
    tusb_cfg.task =
        TINYUSB_TASK_CUSTOM(TINYUSB_DEFAULT_TASK_SIZE, TINYUSB_DEFAULT_TASK_PRIO, 0);
    tusb_cfg.descriptor.device = &s_device_desc;
    tusb_cfg.descriptor.full_speed_config = s_msc_fs_config_desc;
    tusb_cfg.descriptor.string = (const char**)s_string_desc;
    tusb_cfg.descriptor.string_count = sizeof(s_string_desc) / sizeof(s_string_desc[0]);

    err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
        usb_del_phy(s_phy_hdl);
        s_phy_hdl = nullptr;
        RoutePhyToUsj();
        s_usb_switching.store(false, std::memory_order_relaxed);
        return err;
    }
#endif

    s_gadget_active.store(true, std::memory_order_relaxed);
    err = MountToUsb();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount USB after start failed, rollback");
        MountToAppWithRetry(3, 150);
        UsbGadgetTeardownUnlocked();
        s_usb_switching.store(false, std::memory_order_relaxed);
        SyncUiFromState();
        return err;
    }

    s_usb_switching.store(false, std::memory_order_relaxed);
    SetStandbyBlock(true);
#if CONFIG_IDF_TARGET_ESP32S31
    epd_board_metalio_eink4_plus_ana_sw_set(false); // 虚拟 U 盘：ANA_SW(GPIO36) 置低
#endif
    SyncUiFromState();
    ESP_LOGI(TAG, "virtual U-disk enabled");
    return ESP_OK;
}

// 停用：必须先拆 TinyUSB/PHY，再收回 SD。
// 旧顺序（先 MountToApp）在电脑未弹出时会卡在 MSC mux（portMAX_DELAY），
// UI 一直「正在切换」且 s_op_in_progress 永不清除。
esp_err_t UsbGadgetStop(bool /*force*/) {
    if (!s_gadget_active.load(std::memory_order_relaxed)) {
        RoutePhyToUsj();
        s_want_active.store(false, std::memory_order_relaxed);
        SyncUiFromState();
        return ESP_OK;
    }

    s_usb_switching.store(true, std::memory_order_relaxed);

    esp_err_t err = UsbGadgetTeardownUnlocked();
    if (ReclaimSdAfterUsbDown() != ESP_OK) {
        ESP_LOGW(TAG, "USB down but SD reclaim incomplete");
    }

    s_want_active.store(false, std::memory_order_relaxed);
    s_usb_switching.store(false, std::memory_order_relaxed);
    SyncUiFromState();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "virtual U-disk disabled, USJ restored");
    }
    return err;
}

void UsbWorkerTask(void* /*arg*/) {
    UsbReq req = kReqDisableGadget;
    while (true) {
        if (xQueueReceive(s_usb_req_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        s_op_in_progress.store(true, std::memory_order_relaxed);
        SetHint(UsbVirtualDisk::UiHint::Switching);
        NotifyUi();

        switch (req) {
            case kReqEnableGadget:
                // 切换中途被取消：跳过本次启用
                if (!s_want_active.load(std::memory_order_relaxed)) {
                    SyncUiFromState();
                    break;
                }
                SetHint(UsbVirtualDisk::UiHint::Enabling);
                NotifyUi();
                if (UsbGadgetStart() != ESP_OK) {
                    s_want_active.store(false, std::memory_order_relaxed);
                    SetHint(UsbVirtualDisk::UiHint::EnableFailed);
                    NotifyUi();
                }
                break;
            case kReqDisableGadget:
                if (s_want_active.load(std::memory_order_relaxed)) {
                    SyncUiFromState();
                    break;
                }
                SetHint(UsbVirtualDisk::UiHint::Disabling);
                NotifyUi();
                if (UsbGadgetStop(false) != ESP_OK) {
                    SetHint(UsbVirtualDisk::UiHint::DisableFailed);
                    NotifyUi();
                }
                break;
            case kReqForceDisableGadget:
                s_want_active.store(false, std::memory_order_relaxed);
                SetHint(UsbVirtualDisk::UiHint::Disabling);
                NotifyUi();
                if (UsbGadgetStop(true) != ESP_OK) {
                    SetHint(UsbVirtualDisk::UiHint::DisableFailed);
                    NotifyUi();
                }
                break;
            case kReqSdToUsb:
                if (s_gadget_active.load(std::memory_order_relaxed) &&
                    s_want_active.load(std::memory_order_relaxed) &&
                    !s_usb_switching.load(std::memory_order_relaxed)) {
                    if (MountToUsb() != ESP_OK) {
                        SyncMountFlagsFromStorage();
                        SetHint(UsbVirtualDisk::UiHint::HostBusy);
                        NotifyUi();
                    } else {
                        SyncUiFromState();
                    }
                }
                break;
            case kReqSdToApp:
                if (s_gadget_active.load(std::memory_order_relaxed) &&
                    s_want_active.load(std::memory_order_relaxed) &&
                    !s_usb_switching.load(std::memory_order_relaxed)) {
                    if (MountToAppWithRetry(3, 200) != ESP_OK) {
                        SyncMountFlagsFromStorage();
                        SetHint(UsbVirtualDisk::UiHint::HostBusy);
                        NotifyUi();
                    } else {
                        SyncUiFromState();
                    }
                }
                break;
            default:
                break;
        }

        s_op_in_progress.store(false, std::memory_order_relaxed);
        // 仅当忙时用户改过期望态才补发启停，避免停用失败后死循环重试。
        if (s_reconcile_pending.exchange(false, std::memory_order_relaxed)) {
            PostReconcileLocked();
        }
    }
}

}  // namespace

UsbVirtualDisk& UsbVirtualDisk::GetInstance() {
    static UsbVirtualDisk instance;
    return instance;
}

void UsbVirtualDisk::Init() {
    if (s_inited.exchange(true)) {
        return;
    }
    RoutePhyToUsj();
    s_usb_req_queue = xQueueCreate(1, sizeof(UsbReq));
    if (s_usb_req_queue == nullptr) {
        ESP_LOGE(TAG, "create usb queue failed");
        s_inited.store(false);
        return;
    }
    BaseType_t ok =
        xTaskCreate(UsbWorkerTask, "usb_vd_worker", 12288, nullptr, 6, nullptr);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create usb worker failed");
        vQueueDelete(s_usb_req_queue);
        s_usb_req_queue = nullptr;
        s_inited.store(false);
        return;
    }
    ESP_LOGI(TAG, "UsbVirtualDisk ready (default USJ)");
}

bool UsbVirtualDisk::IsSupported() const {
    return true;
}

bool UsbVirtualDisk::IsGadgetActive() const {
    return s_gadget_active.load(std::memory_order_relaxed);
}

bool UsbVirtualDisk::IsBusy() const {
    return s_op_in_progress.load(std::memory_order_relaxed);
}

bool UsbVirtualDisk::IsSdExportedToHost() const {
    return s_sd_to_usb.load(std::memory_order_relaxed);
}

UsbVirtualDisk::UiHint UsbVirtualDisk::GetUiHint() const {
    return static_cast<UiHint>(s_ui_hint.load(std::memory_order_relaxed));
}

void UsbVirtualDisk::Toggle() {
    Init();
    if (s_usb_req_queue == nullptr) {
        SetHint(UiHint::EnableFailed);
        NotifyUi();
        return;
    }

    // 忙：翻转期望态，等当前请求结束后 reconcile；闲：相对实际态取反。
    const bool busy = s_op_in_progress.load(std::memory_order_relaxed);
    const bool new_want =
        busy ? !s_want_active.load(std::memory_order_relaxed)
             : !s_gadget_active.load(std::memory_order_relaxed);

    if (new_want) {
        auto& sd = SdCardManager::GetInstance();
        if (!sd.IsMounted() && !sd.HasCard() &&
            !s_gadget_active.load(std::memory_order_relaxed)) {
            SetHint(UiHint::NoSdCard);
            NotifyUi();
            return;
        }
    }

    s_want_active.store(new_want, std::memory_order_relaxed);
    if (busy) {
        s_reconcile_pending.store(true, std::memory_order_relaxed);
        SetHint(UiHint::Switching);
        NotifyUi();
        return;
    }
    SetHint(new_want ? UiHint::Enabling : UiHint::Disabling);
    NotifyUi();
    PostReconcileLocked();
}

void UsbVirtualDisk::DisableIfActive() {
    Init();
    if (s_usb_req_queue == nullptr) {
        return;
    }
    s_want_active.store(false, std::memory_order_relaxed);
    // 已启用，或正在启用/切换：强制停用覆盖队列，离开页面必须恢复 USJ。
    if (!s_gadget_active.load(std::memory_order_relaxed) &&
        !s_op_in_progress.load(std::memory_order_relaxed)) {
        return;
    }
    ESP_LOGI(TAG, "auto force-disable virtual U-disk (leaving settings)");
    SetHint(UiHint::Disabling);
    PostGadgetRequest(kReqForceDisableGadget);
}

void UsbVirtualDisk::SetUiNotify(UiNotifyFn fn) {
    std::lock_guard<std::mutex> lock(s_notify_mu);
    s_ui_notify = std::move(fn);
}

const char* UsbVirtualDisk::HintText(UiHint hint) {
    switch (hint) {
        case UiHint::Switching:
            return Lang::Strings::SETTINGS_STORAGE_USB_HINT_SWITCHING;
        case UiHint::Enabling:
            return Lang::Strings::SETTINGS_STORAGE_USB_HINT_ENABLING;
        case UiHint::Disabling:
            return Lang::Strings::SETTINGS_STORAGE_USB_HINT_DISABLING;
        case UiHint::EnabledHost:
            return Lang::Strings::SETTINGS_STORAGE_USB_HINT_HOST;
        case UiHint::EnabledLocal:
            return Lang::Strings::SETTINGS_STORAGE_USB_HINT_LOCAL;
        case UiHint::Disabled:
            return Lang::Strings::SETTINGS_STORAGE_USB_HINT_DISABLED;
        case UiHint::EnableFailed:
            return Lang::Strings::SETTINGS_STORAGE_USB_HINT_ENABLE_FAIL;
        case UiHint::DisableFailed:
            return Lang::Strings::SETTINGS_STORAGE_USB_HINT_DISABLE_FAIL;
        case UiHint::NoSdCard:
            return Lang::Strings::SETTINGS_STORAGE_USB_HINT_NO_SD;
        case UiHint::FormatRequired:
            return Lang::Strings::SETTINGS_STORAGE_USB_HINT_FORMAT;
        case UiHint::HostBusy:
            return Lang::Strings::SETTINGS_STORAGE_USB_HINT_HOST_BUSY;
        case UiHint::Idle:
        default:
            return Lang::Strings::SETTINGS_STORAGE_USB_HINT_IDLE;
    }
}

#else  // !S3/P4 or MSC disabled

UsbVirtualDisk& UsbVirtualDisk::GetInstance() {
    static UsbVirtualDisk instance;
    return instance;
}

void UsbVirtualDisk::Init() {}

bool UsbVirtualDisk::IsSupported() const {
    return false;
}

bool UsbVirtualDisk::IsGadgetActive() const {
    return false;
}

bool UsbVirtualDisk::IsBusy() const {
    return false;
}

bool UsbVirtualDisk::IsSdExportedToHost() const {
    return false;
}

UsbVirtualDisk::UiHint UsbVirtualDisk::GetUiHint() const {
    return UiHint::Idle;
}

void UsbVirtualDisk::Toggle() {}

void UsbVirtualDisk::DisableIfActive() {}

void UsbVirtualDisk::SetUiNotify(UiNotifyFn /*fn*/) {}

const char* UsbVirtualDisk::HintText(UiHint /*hint*/) {
    return Lang::Strings::SETTINGS_STORAGE_USB_DISABLED;
}

#endif
