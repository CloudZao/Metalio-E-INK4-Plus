#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_intr_alloc.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include "esp_private/usb_phy.h"
#include "hal/usb_serial_jtag_ll.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"
#endif
#include "infra/tasks/task_create.h"
#include "assets/lang_screen.h"
#include "boards/common/io/IOExpander.hpp"
#include "boards/common/power/cx25601n/cx25601n.h"
#include "boards/common/storage/usb_role.h"
#include "boards/common/storage/usb_virtual_disk.h"
#include "usb_uvc_still.h"
#include "assets/lang_config.h"
#include "sysmgmt/power/policy/power_policy.h"

static constexpr const char* TAG = "UsbUvcStill";

#if CONFIG_IDF_TARGET_ESP32S3

static constexpr int kUsbHostPriority = 15;
static constexpr int kFrameQueueLen = 2;
static constexpr int kSkipFrames = 2;
static constexpr int kMuxSettleMs = 80;
static constexpr int kOpenTimeoutMs = 8000;
// URB 必须落在内部 DMA 堆；整机 LVGL 占用后 4×8KB 常失败，静拍用更小配置
static constexpr int kStillNumUrbs = 2;
static constexpr size_t kStillUrbQvga = 2 * 1024;
static constexpr size_t kStillUrbVga = 4 * 1024;
static constexpr size_t kStillUrbSvga = 6 * 1024;
static constexpr size_t kStillUrb720p = 8 * 1024;

static constexpr size_t kFrameBuf720p = 280 * 1024;
static constexpr size_t kFrameBufSvga = 160 * 1024;
static constexpr size_t kFrameBufVga = 100 * 1024;
static constexpr size_t kFrameBufQvga = 40 * 1024;

static SemaphoreHandle_t s_session_mu = nullptr;
static SemaphoreHandle_t s_all_free_sem = nullptr;
static bool s_op_busy = false;
static std::atomic<bool> s_session_active{false};  // UVC 已装且可用于 Capture/Detect
static bool s_host_installed = false;              // 控制器完成卸载前持续持有摄像头角色
static bool s_uvc_installed = false;

static usb_phy_handle_t s_phy = nullptr;
static TaskHandle_t s_usb_lib_task = nullptr;
static std::atomic<bool> s_usb_lib_run{false};
static StaticSemaphore_t s_usb_lib_done_storage;
static SemaphoreHandle_t s_usb_lib_done = nullptr;
static QueueHandle_t s_frame_q = nullptr;
static uvc_host_stream_hdl_t s_stream = nullptr;
static std::atomic<bool> s_uvc_seen{false};
static volatile uint8_t s_conn_addr = 0;
static volatile uint8_t s_conn_stream = 0;
static volatile size_t s_conn_formats = 0;

static constexpr uint32_t kDisconnectWaitMs = 2000;
static constexpr uint32_t kAllFreeWaitMs = 2000;
static constexpr uint32_t kHubSettleMs = 120;

static void EnsureSessionMutex() {
    if (s_session_mu == nullptr) {
        s_session_mu = xSemaphoreCreateMutex();
    }
}

static void EnsureAllFreeSem() {
    if (s_all_free_sem == nullptr) {
        s_all_free_sem = xSemaphoreCreateBinary();
    }
}

static void DrainAllFreeSem() {
    EnsureAllFreeSem();
    if (s_all_free_sem == nullptr) {
        return;
    }
    while (xSemaphoreTake(s_all_free_sem, 0) == pdTRUE) {
    }
}

static bool WaitAllDevicesFree(uint32_t timeout_ms) {
    EnsureAllFreeSem();
    if (s_all_free_sem == nullptr || !s_host_installed) {
        return !s_host_installed;
    }
    if (xSemaphoreTake(s_all_free_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "operation=stop_session stage=free_devices result=timeout timeout_ms=%u",
                 static_cast<unsigned>(timeout_ms));
        return false;
    }
    return true;
}

static void WaitUvcDisconnected(uint32_t timeout_ms) {
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (s_uvc_seen && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_uvc_seen) {
        ESP_LOGW(TAG, "wait UVC disconnect timeout %ums", static_cast<unsigned>(timeout_ms));
    }
}

static size_t UrbSizeForRes(uint16_t h, uint16_t v) {
    const uint32_t pixels = static_cast<uint32_t>(h) * v;
    if (pixels >= 1280 * 720) {
        return kStillUrb720p;
    }
    if (pixels >= 800 * 600) {
        return kStillUrbSvga;
    }
    if (pixels >= 640 * 480) {
        return kStillUrbVga;
    }
    return kStillUrbQvga;
}

static size_t FrameBufForRes(uint16_t h, uint16_t v) {
    const uint32_t pixels = static_cast<uint32_t>(h) * v;
    if (pixels >= 1280 * 720) {
        return kFrameBuf720p;
    }
    if (pixels >= 800 * 600) {
        return kFrameBufSvga;
    }
    if (pixels >= 640 * 480) {
        return kFrameBufVga;
    }
    return kFrameBufQvga;
}

static bool LooksLikeJpeg(const uint8_t* data, size_t len) {
    return data != nullptr && len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

static uint8_t* AllocJpegCopy(const uint8_t* src, size_t len) {
    uint8_t* copy = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy == nullptr) {
        copy = static_cast<uint8_t*>(malloc(len));
    }
    if (copy == nullptr) {
        return nullptr;
    }
    memcpy(copy, src, len);
    return copy;
}

static void SelectMuxCamera(bool camera) {
    auto& io = IOExpander::getInstance();
    if (!io.isInitialized()) {
        ESP_LOGW(TAG, "IOExpander not ready, skip USB_MUX_SEL");
        return;
    }
    const esp_err_t err = io.setLevel(IOExpander::Pin::USB_MUX_SEL, !camera);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "USB_MUX_SEL=%s failed: %s", camera ? "camera" : "flash", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "USB_MUX_SEL → %s", camera ? "camera(LOW)" : "flash(HIGH)");
    vTaskDelay(pdMS_TO_TICKS(kMuxSettleMs));
}

static void RoutePhyToUsj() {
    usb_serial_jtag_ll_phy_enable_external(false);
    usb_serial_jtag_ll_phy_enable_pad(true);
    ESP_LOGI(TAG, "PHY restored to USB Serial/JTAG");
}

static esp_err_t InitHostPhy() {
    usb_serial_jtag_ll_phy_enable_pad(false);
    usb_serial_jtag_ll_phy_enable_external(true);

    const usb_phy_config_t phy_config = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_HOST,
        .otg_speed = USB_PHY_SPEED_UNDEFINED,
        .ext_io_conf = nullptr,
        .otg_io_conf = nullptr,
    };
    esp_err_t err = usb_new_phy(&phy_config, &s_phy);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_new_phy: %s", esp_err_to_name(err));
        RoutePhyToUsj();
        s_phy = nullptr;
        return err;
    }
    return ESP_OK;
}

static void DeinitHostPhy() {
    if (s_phy != nullptr) {
        const esp_err_t result = usb_del_phy(s_phy);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "operation=stop_session stage=delete_phy code=%d", result);
            return;
        }
        s_phy = nullptr;
    }
    RoutePhyToUsj();
}

struct UsbUvcStillHostInstallArg {
    const usb_host_config_t* cfg = nullptr;
    esp_err_t err = ESP_FAIL;
    SemaphoreHandle_t done = nullptr;
};

static void HostInstallTask(void* arg) {
    auto* a = static_cast<UsbUvcStillHostInstallArg*>(arg);
    a->err = usb_host_install(a->cfg);
    xSemaphoreGive(a->done);
    vTaskDelete(nullptr);
}

// 在 core1 分配 USB 中断，避开百问会话时 core0 中断槽耗尽。
static esp_err_t UsbHostInstallOnCore1(const usb_host_config_t* cfg) {
    UsbUvcStillHostInstallArg arg;
    arg.cfg = cfg;
    arg.done = xSemaphoreCreateBinary();
    if (arg.done == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    TaskHandle_t th = nullptr;
    const BaseType_t ok = FirmwareCreateTask(HostInstallTask, "usb_h_inst", 4096, &arg, kUsbHostPriority, &th, 1, 0);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "core1 install task create failed, fallback current core");
        const esp_err_t err = usb_host_install(cfg);
        vSemaphoreDelete(arg.done);
        return err;
    }
    xSemaphoreTake(arg.done, portMAX_DELAY);
    vSemaphoreDelete(arg.done);
    return arg.err;
}

static void UsbLibTask(void* /*arg*/) {
    while (s_usb_lib_run) {
        uint32_t flags = 0;
        const esp_err_t err = usb_host_lib_handle_events(pdMS_TO_TICKS(200), &flags);
        if (err == ESP_OK) {
            // 卸 UVC 客户端后需 free devices；不做 root power-cycle 时与参考工程一致可安全 free
            if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
                const esp_err_t free_err = usb_host_device_free_all();
                if (free_err == ESP_OK) {
                    // 已无设备可释放：不会再来 ALL_FREE，直接唤醒 ParkSession
                    EnsureAllFreeSem();
                    if (s_all_free_sem != nullptr) {
                        xSemaphoreGive(s_all_free_sem);
                    }
                } else if (free_err != ESP_ERR_NOT_FINISHED) {
                    ESP_LOGW(TAG, "device_free_all: %s", esp_err_to_name(free_err));
                }
            }
            if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
                EnsureAllFreeSem();
                if (s_all_free_sem != nullptr) {
                    xSemaphoreGive(s_all_free_sem);
                }
            }
        }
    }
    xSemaphoreGive(s_usb_lib_done);
    vTaskDelete(nullptr);
}

static bool FrameCb(const uvc_host_frame_t* frame, void* user_ctx) {
    auto* q = static_cast<QueueHandle_t>(user_ctx);
    if (q == nullptr || frame == nullptr) {
        return true;
    }
    if (xQueueSendToBack(q, &frame, 0) != pdPASS) {
        return true;
    }
    return false;
}

static void StreamEventCb(const uvc_host_stream_event_data_t* event, void* /*user_ctx*/) {
    if (event == nullptr) {
        return;
    }
    switch (event->type) {
        case UVC_HOST_TRANSFER_ERROR:
            ESP_LOGE(TAG, "UVC transfer error: %d", event->transfer_error.error);
            break;
        case UVC_HOST_DEVICE_DISCONNECTED:
            ESP_LOGW(TAG, "UVC disconnected");
            s_uvc_seen = false;
            break;
        case UVC_HOST_FRAME_BUFFER_OVERFLOW:
            ESP_LOGW(TAG, "UVC frame overflow");
            break;
        case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
            ESP_LOGW(TAG, "UVC frame underflow");
            break;
        default:
            break;
    }
}

static void UvcDriverEventCb(const uvc_host_driver_event_data_t* event, void* /*user_ctx*/) {
    if (event != nullptr && event->type == UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) {
        s_conn_addr = event->device_connected.dev_addr;
        s_conn_stream = event->device_connected.uvc_stream_index;
        s_conn_formats = event->device_connected.frame_info_num;
        s_uvc_seen = true;
        ESP_LOGI(TAG, "UVC connected addr=%d stream=%d formats=%u", s_conn_addr, s_conn_stream,
                 static_cast<unsigned>(s_conn_formats));
    }
}

static void DrainFrameQueue(uvc_host_stream_hdl_t stream) {
    if (s_frame_q == nullptr) {
        return;
    }
    uvc_host_frame_t* frame = nullptr;
    while (xQueueReceive(s_frame_q, &frame, 0) == pdPASS) {
        if (stream != nullptr && frame != nullptr) {
            uvc_host_frame_return(stream, frame);
        }
    }
}

static bool CloseStreamIfAny() {
    if (s_stream == nullptr) {
        return true;
    }
    DrainFrameQueue(s_stream);
    uvc_host_stream_stop(s_stream);
    DrainFrameQueue(s_stream);
    const esp_err_t close_err = uvc_host_stream_close(s_stream);
    if (close_err != ESP_OK) {
        ESP_LOGE(TAG, "operation=stop_session stage=close_stream code=%d", close_err);
        return false;
    }
    s_stream = nullptr;
    return true;
}

// 先停止根端口并断开摄像头，再注销客户端、排空设备和事件，最后卸载控制器与 PHY。
static void ParkSession() {
    if (!CloseStreamIfAny()) {
        return;
    }
    if (s_host_installed) {
        const esp_err_t result = usb_host_lib_set_root_port_power(false);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "operation=stop_session stage=stop_root code=%d", result);
            return;
        }
    }
    SelectMuxCamera(false);
    if (s_uvc_seen || s_uvc_installed) {
        WaitUvcDisconnected(kDisconnectWaitMs);
        vTaskDelay(pdMS_TO_TICKS(kHubSettleMs));
    }
    DrainAllFreeSem();
    if (s_uvc_installed) {
        const esp_err_t result = uvc_host_uninstall();
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "operation=stop_session stage=uninstall_uvc code=%d", result);
            return;
        }
        s_uvc_installed = false;
        if (!WaitAllDevicesFree(kAllFreeWaitMs)) {
            return;
        }
    }
    if (s_host_installed) {
        usb_host_lib_info_t info{};
        const esp_err_t info_result = usb_host_lib_info(&info);
        if (info_result != ESP_OK || info.num_clients != 0) {
            ESP_LOGE(TAG, "operation=stop_session stage=check_host code=%d clients=%d", info_result, info.num_clients);
            return;
        }
        if (info.num_devices != 0) {
            const esp_err_t result = usb_host_device_free_all();
            if (result == ESP_ERR_NOT_FINISHED) {
                if (!WaitAllDevicesFree(kAllFreeWaitMs)) {
                    return;
                }
            } else if (result != ESP_OK) {
                ESP_LOGE(TAG, "operation=stop_session stage=drain_host code=%d devices=%d", result, info.num_devices);
                return;
            }
        }
        if (s_usb_lib_task) {
            s_usb_lib_run.store(false);
            xSemaphoreTake(s_usb_lib_done, portMAX_DELAY);
            s_usb_lib_task = nullptr;
        }
        uint32_t events = 0;
        const esp_err_t event_result = usb_host_lib_handle_events(0, &events);
        if (event_result != ESP_OK && event_result != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "operation=stop_session stage=drain_events code=%d", event_result);
            return;
        }
        const esp_err_t result = usb_host_uninstall();
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "operation=stop_session stage=uninstall_host code=%d", result);
            return;
        }
        s_host_installed = false;
    }
    if (s_frame_q) {
        vQueueDelete(s_frame_q);
        s_frame_q = nullptr;
    }
    DeinitHostPhy();
    if (s_phy) {
        return;
    }
    s_uvc_seen = false;
    s_conn_addr = 0;
    s_conn_stream = 0;
    s_conn_formats = 0;
    s_session_active = false;
    ESP_LOGI(TAG, "USB camera session released; Serial/JTAG restored");
}

static esp_err_t InstallUvcDriver() {
    if (s_uvc_installed) {
        return ESP_OK;
    }
    const uvc_host_driver_config_t uvc_cfg = {
        .driver_task_stack_size = 6 * 1024,
        .driver_task_priority = kUsbHostPriority + 1,
        .xCoreID = 1,
        .create_background_task = true,
        .event_cb = UvcDriverEventCb,
        .user_ctx = nullptr,
    };
    const esp_err_t err = uvc_host_install(&uvc_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uvc_host_install: %s", esp_err_to_name(err));
        return err;
    }
    s_uvc_installed = true;
    return ESP_OK;
}

static bool CancelRequested(const UsbUvcStill::Request* request) {
    return request && request->cancelled && request->cancelled(request->context);
}

static esp_err_t WaitUvcConnected(uint32_t enum_wait_ms, const UsbUvcStill::Request* request) {
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(enum_wait_ms);
    while (!s_uvc_seen && xTaskGetTickCount() < deadline) {
        if (CancelRequested(request)) {
            return ESP_ERR_INVALID_STATE;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return s_uvc_seen ? ESP_OK : ESP_ERR_TIMEOUT;
}

static bool RefuseIfDiskBusy() {
    auto& disk = UsbVirtualDisk::GetInstance();
    if (disk.IsGadgetActive() || disk.IsBusy()) {
        ESP_LOGW(TAG, "refuse: virtual U-disk active/busy");
        return true;
    }
    return false;
}

static void FillDetectId(UsbUvcStill::DetectInfo& out) {
    out.dev_addr = s_conn_addr;
    out.stream_index = s_conn_stream;
    out.format_count = s_conn_formats;
    out.h_res = 0;
    out.v_res = 0;
    out.id_str[0] = '\0';

    if (!s_uvc_seen || s_conn_addr == 0) {
        return;
    }

    uvc_host_frame_info_t list[16];
    size_t list_size = sizeof(list) / sizeof(list[0]);
    uvc_host_frame_info_t(*list_ptr)[] = &list;
    const esp_err_t ferr = uvc_host_get_frame_list(s_conn_addr, s_conn_stream, list_ptr, &list_size);
    if (ferr == ESP_OK && list_size > 0) {
        size_t pick = 0;
        for (size_t i = 0; i < list_size; ++i) {
            if (list[i].format == UVC_VS_FORMAT_MJPEG) {
                pick = i;
                break;
            }
        }
        out.h_res = static_cast<uint16_t>(list[pick].h_res);
        out.v_res = static_cast<uint16_t>(list[pick].v_res);
        snprintf(out.id_str, sizeof(out.id_str), "a%u·%ux%u", static_cast<unsigned>(out.dev_addr),
                 static_cast<unsigned>(out.h_res), static_cast<unsigned>(out.v_res));
    } else {
        snprintf(out.id_str, sizeof(out.id_str), "a%u·fmt%u", static_cast<unsigned>(out.dev_addr),
                 static_cast<unsigned>(out.format_count));
    }
}

static esp_err_t StartSessionUnlocked(uint32_t enum_wait_ms, const UsbUvcStill::Request* request) {
    if (s_session_active && s_uvc_installed) {
        return ESP_OK;
    }
    if (RefuseIfDiskBusy()) {
        return ESP_ERR_INVALID_STATE;
    }

    s_uvc_seen = false;
    s_conn_addr = 0;
    s_conn_stream = 0;
    s_conn_formats = 0;

    if (s_host_installed) {
        ParkSession();
        if (s_host_installed) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (!s_usb_lib_done) {
        s_usb_lib_done = xSemaphoreCreateBinaryStatic(&s_usb_lib_done_storage);
    }
    xSemaphoreTake(s_usb_lib_done, 0);

    esp_err_t err = InitHostPhy();
    if (err != ESP_OK) {
        return err;
    }

    s_frame_q = xQueueCreate(kFrameQueueLen, sizeof(uvc_host_frame_t*));
    if (s_frame_q == nullptr) {
        DeinitHostPhy();
        return ESP_ERR_NO_MEM;
    }

    usb_host_config_t host_config = {};
    host_config.skip_phy_setup = true;
    host_config.root_port_unpowered = true;
    // 百问会话时 core0 常无空闲 LEVEL1 槽；LOWMED 允许 1~3，并在 core1 上 install
    host_config.intr_flags = ESP_INTR_FLAG_LOWMED;
    err = UsbHostInstallOnCore1(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(err));
        vQueueDelete(s_frame_q);
        s_frame_q = nullptr;
        DeinitHostPhy();
        return err;
    }

    s_host_installed = true;
    s_usb_lib_run = true;
    if (FirmwareCreateTask(UsbLibTask, "uvc_usb_lib", 4096, nullptr, kUsbHostPriority, &s_usb_lib_task, 1, 0) !=
        pdPASS) {
        s_usb_lib_run = false;
        s_usb_lib_task = nullptr;
        ParkSession();
        return ESP_ERR_NO_MEM;
    }

    err = InstallUvcDriver();
    if (err != ESP_OK) {
        ParkSession();
        return err;
    }

    err = usb_host_lib_set_root_port_power(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "operation=start_session stage=start_root code=%d", err);
        ParkSession();
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    err = WaitUvcConnected(enum_wait_ms, request);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "StartSession: Host up but no UVC within %ums", static_cast<unsigned>(enum_wait_ms));
        return err;
    }
    s_session_active = true;
    ESP_LOGI(TAG, "StartSession OK, UVC seen");
    return ESP_OK;
}

static esp_err_t OpenBestStream(const UsbUvcStill::Request* request) {
    esp_err_t last_error = ESP_ERR_INVALID_RESPONSE;
    // 先按设备真实 format 列表开流（fps=0 = 默认间隔），失败再回落硬编码档。
    uvc_host_frame_info_t list[24];
    size_t list_size = sizeof(list) / sizeof(list[0]);
    uvc_host_frame_info_t(*list_ptr)[] = &list;
    const uint8_t stream_idx = s_conn_stream;
    const uint8_t dev_addr = s_conn_addr;

    if (dev_addr != 0) {
        const esp_err_t ferr = uvc_host_get_frame_list(dev_addr, stream_idx, list_ptr, &list_size);
        if (ferr == ESP_OK && list_size > 0) {
            ESP_LOGI(TAG, "device formats (%u), stream_idx=%u:", static_cast<unsigned>(list_size), stream_idx);
            for (size_t i = 0; i < list_size; ++i) {
                float def_fps = 0.0f;
                if (list[i].default_interval > 0) {
                    def_fps = 10000000.0f / static_cast<float>(list[i].default_interval);
                }
                ESP_LOGI(TAG, "  [%u] %ux%u fmt=%d def≈%.1ffps", static_cast<unsigned>(i), list[i].h_res, list[i].v_res,
                         static_cast<int>(list[i].format), def_fps);
            }

            // 只开 MJPEG；按像素从小到大试（静拍带宽更稳）
            size_t order[24];
            size_t n_mjpeg = 0;
            for (size_t i = 0; i < list_size && n_mjpeg < 24; ++i) {
                if (list[i].format == UVC_VS_FORMAT_MJPEG) {
                    order[n_mjpeg++] = i;
                }
            }
            for (size_t a = 0; a + 1 < n_mjpeg; ++a) {
                for (size_t b = a + 1; b < n_mjpeg; ++b) {
                    const uint32_t pa = static_cast<uint32_t>(list[order[a]].h_res) * list[order[a]].v_res;
                    const uint32_t pb = static_cast<uint32_t>(list[order[b]].h_res) * list[order[b]].v_res;
                    if (pb < pa) {
                        const size_t tmp = order[a];
                        order[a] = order[b];
                        order[b] = tmp;
                    }
                }
            }

            for (size_t oi = 0; oi < n_mjpeg; ++oi) {
                if (CancelRequested(request)) {
                    return ESP_ERR_INVALID_STATE;
                }
                const size_t i = order[oi];
                const uint16_t h = static_cast<uint16_t>(list[i].h_res);
                const uint16_t v = static_cast<uint16_t>(list[i].v_res);
                const size_t frame_size = FrameBufForRes(h, v);
                const size_t urb_size = UrbSizeForRes(h, v);

                uvc_host_stream_config_t cfg = {};
                cfg.event_cb = StreamEventCb;
                cfg.frame_cb = FrameCb;
                cfg.user_ctx = s_frame_q;
                cfg.usb.dev_addr = dev_addr;
                cfg.usb.vid = UVC_HOST_ANY_VID;
                cfg.usb.pid = UVC_HOST_ANY_PID;
                cfg.usb.uvc_stream_index = stream_idx;
                cfg.vs_format.h_res = h;
                cfg.vs_format.v_res = v;
                cfg.vs_format.fps = 0;  // 默认 FPS，避免硬编码不匹配
                cfg.vs_format.format = UVC_VS_FORMAT_MJPEG;
                cfg.advanced.number_of_frame_buffers = kFrameQueueLen;
                cfg.advanced.frame_size = frame_size;
                cfg.advanced.number_of_urbs = kStillNumUrbs;
                cfg.advanced.urb_size = urb_size;
                cfg.advanced.frame_heap_caps = MALLOC_CAP_SPIRAM;

                ESP_LOGI(TAG, "try open from list %ux%u@default MJPEG stream=%u urbs=%d×%u DMA_free=%u", h, v,
                         stream_idx, kStillNumUrbs, static_cast<unsigned>(urb_size),
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)));
                const esp_err_t err = uvc_host_stream_open(&cfg, pdMS_TO_TICKS(kOpenTimeoutMs), &s_stream);
                if (err == ESP_OK && s_stream != nullptr) {
                    ESP_LOGI(TAG, "stream open ok: %ux%u (device list)", h, v);
                    return ESP_OK;
                }
                if (CancelRequested(request)) {
                    return ESP_ERR_INVALID_STATE;
                }
                last_error = err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
                ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(err));
                s_stream = nullptr;
            }
        } else {
            ESP_LOGW(TAG, "get_frame_list failed: %s", esp_err_to_name(ferr));
        }
    }

    // 回落：小 URB，适配内部 DMA 紧张
    static const struct {
        uint16_t h;
        uint16_t v;
        float fps;
    } kTries[] = {
        {320, 240, 0.0f},  {320, 240, 15.0f}, {640, 360, 0.0f},  {640, 480, 0.0f},
        {640, 480, 15.0f}, {800, 600, 0.0f},  {1280, 720, 0.0f},
    };

    for (size_t i = 0; i < sizeof(kTries) / sizeof(kTries[0]); ++i) {
        if (CancelRequested(request)) {
            return ESP_ERR_INVALID_STATE;
        }
        const size_t frame_size = FrameBufForRes(kTries[i].h, kTries[i].v);
        const size_t urb_size = UrbSizeForRes(kTries[i].h, kTries[i].v);
        uvc_host_stream_config_t cfg = {};
        cfg.event_cb = StreamEventCb;
        cfg.frame_cb = FrameCb;
        cfg.user_ctx = s_frame_q;
        cfg.usb.dev_addr = dev_addr;
        cfg.usb.vid = UVC_HOST_ANY_VID;
        cfg.usb.pid = UVC_HOST_ANY_PID;
        cfg.usb.uvc_stream_index = stream_idx;
        cfg.vs_format.h_res = kTries[i].h;
        cfg.vs_format.v_res = kTries[i].v;
        cfg.vs_format.fps = kTries[i].fps;
        cfg.vs_format.format = UVC_VS_FORMAT_MJPEG;
        cfg.advanced.number_of_frame_buffers = kFrameQueueLen;
        cfg.advanced.frame_size = frame_size;
        cfg.advanced.number_of_urbs = kStillNumUrbs;
        cfg.advanced.urb_size = urb_size;
        cfg.advanced.frame_heap_caps = MALLOC_CAP_SPIRAM;

        ESP_LOGI(TAG, "try open fallback %ux%u@%.0f MJPEG urbs=%d×%u", kTries[i].h, kTries[i].v, kTries[i].fps,
                 kStillNumUrbs, static_cast<unsigned>(urb_size));
        const esp_err_t err = uvc_host_stream_open(&cfg, pdMS_TO_TICKS(kOpenTimeoutMs), &s_stream);
        if (err == ESP_OK && s_stream != nullptr) {
            ESP_LOGI(TAG, "stream open ok: %ux%u@%.0f", kTries[i].h, kTries[i].v, kTries[i].fps);
            return ESP_OK;
        }
        if (CancelRequested(request)) {
            return ESP_ERR_INVALID_STATE;
        }
        last_error = err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
        ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(err));
        s_stream = nullptr;
    }
    return last_error;
}

static esp_err_t WaitAndCopyFrame(UsbUvcStill::JpegBuffer& out, uint32_t timeout_ms,
                                  const UsbUvcStill::Request* request) {
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    int skipped = 0;

    while (true) {
        if (CancelRequested(request)) {
            return ESP_ERR_INVALID_STATE;
        }
        const TickType_t now = xTaskGetTickCount();
        if (now >= deadline) {
            return ESP_ERR_TIMEOUT;
        }
        const TickType_t wait = request ? std::min<TickType_t>(deadline - now, pdMS_TO_TICKS(50)) : deadline - now;
        uvc_host_frame_t* frame = nullptr;
        if (xQueueReceive(s_frame_q, &frame, wait) != pdPASS || frame == nullptr) {
            continue;
        }

        const bool ok_jpeg = LooksLikeJpeg(frame->data, frame->data_len);
        if (!ok_jpeg || skipped < kSkipFrames) {
            if (ok_jpeg) {
                ++skipped;
            } else {
                ESP_LOGW(TAG, "non-JPEG frame len=%u", static_cast<unsigned>(frame->data_len));
            }
            uvc_host_frame_return(s_stream, frame);
            continue;
        }

        uint8_t* copy = AllocJpegCopy(frame->data, frame->data_len);
        const size_t len = frame->data_len;
        uvc_host_frame_return(s_stream, frame);
        if (copy == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        out.data = copy;
        out.len = len;
        ESP_LOGD(TAG, "captured JPEG %u bytes (skipped %d)", static_cast<unsigned>(len), skipped);
        return ESP_OK;
    }
}

static esp_err_t CaptureOneUnlocked(UsbUvcStill::JpegBuffer& out, uint32_t timeout_ms,
                                    const UsbUvcStill::Request* request, bool keep_stream) {
    out.Release();
    if (!s_session_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_uvc_seen) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (!keep_stream && !CloseStreamIfAny()) {
        return ESP_ERR_INVALID_STATE;
    }

    const bool reuse = keep_stream && s_stream != nullptr;
    esp_err_t err = ESP_OK;
    if (CancelRequested(request)) {
        err = ESP_ERR_INVALID_STATE;
    } else if (!reuse) {
        err = OpenBestStream(request);
    }
    if (err != ESP_OK) {
        CloseStreamIfAny();
        return err;
    }

    if (CancelRequested(request)) {
        CloseStreamIfAny();
        return ESP_ERR_INVALID_STATE;
    }
    err = reuse ? ESP_OK : uvc_host_stream_start(s_stream);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stream_start: %s", esp_err_to_name(err));
        CloseStreamIfAny();
        return err;
    }

    if (reuse) {
        DrainFrameQueue(s_stream);
    }
    err = CancelRequested(request) ? ESP_ERR_INVALID_STATE : WaitAndCopyFrame(out, timeout_ms, request);
    if (!keep_stream || err != ESP_OK) {
        const bool closed = CloseStreamIfAny();
        if (!closed && err == ESP_OK) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    return err;
}

static bool TryBeginOp() {
    EnsureSessionMutex();
    if (s_session_mu == nullptr) {
        return false;
    }
    if (xSemaphoreTake(s_session_mu, 0) != pdTRUE) {
        return false;
    }
    if (s_op_busy) {
        xSemaphoreGive(s_session_mu);
        return false;
    }
    s_op_busy = true;
    xSemaphoreGive(s_session_mu);
    return true;
}

static void EndOp() {
    if (s_session_mu == nullptr) {
        return;
    }
    xSemaphoreTake(s_session_mu, portMAX_DELAY);
    s_op_busy = false;
    xSemaphoreGive(s_session_mu);
}

#endif  // CONFIG_IDF_TARGET_ESP32S3

void UsbUvcStill::JpegBuffer::Release() {
    if (data != nullptr) {
        free(data);
        data = nullptr;
    }
    len = 0;
}

void UsbUvcStill::SelectUsbMux(bool camera) {
    #if CONFIG_IDF_TARGET_ESP32S3
    SelectMuxCamera(camera);
    #else
    (void)camera;
    #endif
}

bool UsbUvcStill::IsBusy() {
    #if CONFIG_IDF_TARGET_ESP32S3
    EnsureSessionMutex();
    if (s_session_mu == nullptr) {
        return false;
    }
    if (xSemaphoreTake(s_session_mu, 0) != pdTRUE) {
        return true;
    }
    const bool busy = s_op_busy;
    xSemaphoreGive(s_session_mu);
    return busy;
    #else
    return false;
    #endif
}

bool UsbUvcStill::IsSessionActive() {
    #if CONFIG_IDF_TARGET_ESP32S3
    return s_session_active;
    #else
    return false;
    #endif
}

esp_err_t UsbUvcStill::StartSession(uint32_t enum_wait_ms, const Request* request) {
    #if !CONFIG_IDF_TARGET_ESP32S3
    (void)enum_wait_ms;
    return ESP_ERR_NOT_SUPPORTED;
    #else
    EnsureSessionMutex();
    if (s_session_mu == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_session_mu, portMAX_DELAY);
    if (s_session_active) {
        xSemaphoreGive(s_session_mu);
        return ESP_OK;
    }
    if (s_op_busy) {
        xSemaphoreGive(s_session_mu);
        return ESP_ERR_INVALID_STATE;
    }
    if (UsbRoleOwner().load(std::memory_order_acquire) != UsbRole::CameraHost) {
        xSemaphoreGive(s_session_mu);
        return ESP_ERR_INVALID_STATE;
    }
    s_op_busy = true;
    xSemaphoreGive(s_session_mu);

    const esp_err_t err =
        CancelRequested(request) ? ESP_ERR_INVALID_STATE : StartSessionUnlocked(enum_wait_ms, request);

    EndOp();
    return err;
    #endif
}

void UsbUvcStill::StopSession() {
    #if CONFIG_IDF_TARGET_ESP32S3
    EnsureSessionMutex();
    if (s_session_mu == nullptr) {
        return;
    }
    for (;;) {
        xSemaphoreTake(s_session_mu, portMAX_DELAY);
        if (!s_op_busy) {
            s_op_busy = true;
            xSemaphoreGive(s_session_mu);
            break;
        }
        xSemaphoreGive(s_session_mu);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ParkSession();

    EndOp();
    #endif
}

esp_err_t UsbUvcStill::Detect(DetectInfo& out, uint32_t timeout_ms, const Request* request) {
    #if !CONFIG_IDF_TARGET_ESP32S3
    (void)timeout_ms;
    out = {};
    return ESP_ERR_NOT_SUPPORTED;
    #else
    out = {};
    if (!TryBeginOp()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    if (!s_session_active) {
        err = CancelRequested(request) ? ESP_ERR_INVALID_STATE : StartSessionUnlocked(timeout_ms, request);
    } else if (!s_uvc_seen) {
        err = WaitUvcConnected(timeout_ms, request);
    }

    if (CancelRequested(request)) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK && s_uvc_seen) {
        vTaskDelay(pdMS_TO_TICKS(50));
        FillDetectId(out);
        out.ok = (out.id_str[0] != '\0');
        if (!out.ok) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
    } else if (err == ESP_OK && !s_uvc_seen) {
        err = ESP_ERR_TIMEOUT;
    }

    EndOp();
    return err;
    #endif
}

esp_err_t UsbUvcStill::CaptureOne(JpegBuffer& out, uint32_t timeout_ms, const Request* request, bool keep_stream) {
    #if !CONFIG_IDF_TARGET_ESP32S3
    (void)timeout_ms;
    out.Release();
    return ESP_ERR_NOT_SUPPORTED;
    #else
    if (!TryBeginOp()) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = CaptureOneUnlocked(out, timeout_ms, request, keep_stream);
    EndOp();
    return err;
    #endif
}

const char* UsbUvcStill::ErrMessage(esp_err_t err) {
    switch (err) {
        case ESP_OK:
            return LangString_SETTINGS_TEST_CAMERA_OK;
        case ESP_ERR_TIMEOUT:
            return LangString_SETTINGS_TEST_CAMERA_TIMEOUT;
        case ESP_ERR_NOT_FOUND:
            // 仅 usb_host_install 中断槽不足会原样返回此码
            return LangString_SETTINGS_TEST_CAMERA_USB_IRQ;
        case ESP_ERR_INVALID_RESPONSE:
            return LangString_SETTINGS_TEST_NOT_DETECTED;
        case ESP_ERR_NO_MEM:
            return LangString_BOOK_OUT_OF_MEMORY;
        case ESP_ERR_INVALID_STATE:
            return LangString_SETTINGS_TEST_CAMERA_USB_BUSY;
        case ESP_ERR_NOT_SUPPORTED:
            return LangString_SETTINGS_TEST_SIGNAL_BOARD_UNSUP;
        default:
            return LangString_SETTINGS_TEST_CAMERA_FAIL;
    }
}

static std::atomic<bool> s_capture_owned{false};
static bool s_capture_keep_awake = false; // 仅当前摄像头所有者修改

bool UsbUvcStill::TryAcquireCapture(bool keep_awake) {
    bool expected = false;
    if (!s_capture_owned.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;
    }
    #if CONFIG_IDF_TARGET_ESP32S3
    if (UsbRoleOwner().load(std::memory_order_acquire) != UsbRole::CameraHost && !UsbRoleAcquire(UsbRole::CameraHost)) {
        s_capture_owned.store(false, std::memory_order_release);
        return false;
    }
    #endif
    s_capture_keep_awake = keep_awake;
    if (keep_awake) {
        PowerPolicy::GetInstance().Acquire(PowerNeed::StandbyInhibit);
    }
    return true;
}

void UsbUvcStill::ReleaseCapture() {
    #if CONFIG_IDF_TARGET_ESP32S3
    // 释放失败时继续持有角色，禁止存储侧接管仍在使用的控制器。
    if (!s_host_installed && !s_phy) {
        UsbRoleRelease(UsbRole::CameraHost);
    }
    #endif
    if (s_capture_keep_awake) {
        PowerPolicy::GetInstance().Release(PowerNeed::StandbyInhibit);
        s_capture_keep_awake = false;
    }
    s_capture_owned.store(false, std::memory_order_release);
}

esp_err_t UsbUvcStill::CaptureManaged(JpegBuffer* jpeg, uint32_t timeout_ms, const Request* request,
                                      uint32_t enum_wait_ms, CameraJpegFrameHandler handler, void* context) {
    if (!TryAcquireCapture(handler == nullptr)) {
        return ESP_ERR_INVALID_STATE;
    }

    struct CaptureGuard {
        bool powered = false;   // 本次操作持有 OTG
        bool selected = false;  // 本次操作切换了路由

        ~CaptureGuard() {
            if (selected) {
                StopSession();
            }
            if (powered) {
                vTaskDelay(pdMS_TO_TICKS(50));
                const esp_err_t result = cx25601n_enable_otg(false);
                if (result != ESP_OK) {
                    ESP_LOGW(TAG, "operation=capture stage=release_otg code=%d", result);
                }
            }
            ReleaseCapture();
        }
    } guard;

    #if CONFIG_IDF_TARGET_ESP32S3
    if (!IsBusy() && (s_host_installed || s_phy)) {
        StopSession();
        if (s_host_installed || s_phy) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    #endif
    if (IsBusy() || IsSessionActive() || UsbVirtualDisk::GetInstance().IsGadgetActive() ||
        UsbVirtualDisk::GetInstance().IsBusy()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (request && request->cancelled && request->cancelled(request->context)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cx25601n_is_ready()) {
        const esp_err_t result = cx25601n_enable_otg(true);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "operation=capture stage=enable_otg code=%d", result);
            return result;
        }
        guard.powered = true;
    }
    if (request && request->cancelled && request->cancelled(request->context)) {
        return ESP_ERR_INVALID_STATE;
    }
    SelectUsbMux(true);
    guard.selected = true;
    esp_err_t result;
    if (jpeg) {
        result = StartSession(enum_wait_ms, request);
        if (result == ESP_OK) {
            do {
                jpeg->Release();
                result = CaptureOne(*jpeg, timeout_ms, request, handler != nullptr);
            } while (result == ESP_OK && handler && handler(jpeg->data, jpeg->len, context) &&
                     !(request && request->cancelled && request->cancelled(request->context)));
        }
    } else {
        DetectInfo info;
        result = Detect(info, timeout_ms, request);
    }
    if (request && request->cancelled && request->cancelled(request->context)) {
        result = ESP_ERR_INVALID_STATE;
    }
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "operation=capture stage=%s code=%d", jpeg ? "image" : "detect", result);
    }
    return result;
}
