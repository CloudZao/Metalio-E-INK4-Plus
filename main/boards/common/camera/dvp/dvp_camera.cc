#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <driver/gpio.h>
#include <driver/jpeg_encode.h>
#include <esp_cam_ctlr_dvp.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "dvp_camera.h"
#include "boards/common/bus/flash_access.h"
#include "boards/common/bus/i2c/shared_i2c_bus.h"
#include "boards/common/bus/i2c/power_i2c.h"
#include "boards/common/camera/dvp/gc2145/gc2145_vga_settings.h"
#include "boards/common/display/epdiy/panel/epd_board_adapter.h"
#include "config.h"
#include "infra/diagnostics/firmware_memory.h"
#include "sysmgmt/power/policy/power_policy.h"

static constexpr const char* kTag = "DvpCamera";
static constexpr int kWidth = 640, kHeight = 480;
static constexpr size_t kFrameBytes = kWidth * kHeight * 2;
static constexpr unsigned kWarmupFrames = 8;
// IDF DVP/GDMA 控制块、中断和 PM 锁的小额内部申请准入；帧和描述符另走 PSRAM。
static constexpr size_t kControllerInternalBudgetBytes = 8 * 1024;

// GC2145 sends the high RGB565 byte first (V4L2 RGB565X).
static constexpr uint8_t Rgb565BeLuma(uint8_t high, uint8_t low) {
    const unsigned rgb = (static_cast<unsigned>(high) << 8) | low;
    const unsigned r5 = (rgb >> 11) & 31, g6 = (rgb >> 5) & 63, b5 = rgb & 31;
    const unsigned r = (r5 << 3) | (r5 >> 2);
    const unsigned g = (g6 << 2) | (g6 >> 4);
    const unsigned b = (b5 << 3) | (b5 >> 2);
    return static_cast<uint8_t>((77 * r + 150 * g + 29 * b + 128) >> 8);
}

static std::mutex s_operation;
static const power_i2c_t kBus{I2C_NUM_1, shared_i2c_bus_lock, shared_i2c_bus_unlock, shared_i2c_bus_device_locked};

static esp_err_t LogStep(const char* step, esp_err_t err) {
    if (err == ESP_OK) {
        ESP_LOGI(kTag, "%s: OK", step);
    } else {
        ESP_LOGE(kTag, "%s: %s (0x%x)", step, esp_err_to_name(err), unsigned(err));
    }
    return err;
}

class DvpCameraSession {
public:
    explicit DvpCameraSession(const DvpCameraRequest& request)
        : request_(request), deadline_(esp_timer_get_time() + int64_t(request.timeout_ms) * 1000) {}

    ~DvpCameraSession() {
        // DMA and ISR must stop before freeing the callback context/frame.
        if (started_) {
            esp_cam_ctlr_stop(controller_);
        }
        if (enabled_) {
            esp_cam_ctlr_disable(controller_);
        }
        if (stream_on_) {
            const uint8_t zero = 0;
            power_i2c_write_reg(&kBus, CAM_SCCB_ADDR, 0xfe, &zero, 1, 100);
            power_i2c_write_reg(&kBus, CAM_SCCB_ADDR, 0xf2, &zero, 1, 100);
        }
        if (controller_) {
            LogStep("LCD_CAM controller/MCLK release", esp_cam_ctlr_del(controller_));
            gpio_reset_pin(CAM_PIN_MCLK);
        }
        heap_caps_free(frame_);
        if (done_) {
            vSemaphoreDelete(done_);
        }
        if (rail_lease_) {
            epd_board_adapter_camera_release();
        }
        if (power_lease_) {
            PowerPolicy::GetInstance().Release(PowerNeed::StandbyInhibit);
        }
    }

    esp_err_t Check() const {
        if (request_.cancelled && request_.cancelled(request_.context)) {
            return ESP_ERR_INVALID_STATE;
        }
        return esp_timer_get_time() >= deadline_ ? ESP_ERR_TIMEOUT : ESP_OK;
    }

    esp_err_t Write(uint8_t reg, uint8_t value) {
        esp_err_t err = Check();
        return err == ESP_OK ? power_i2c_write_reg(&kBus, CAM_SCCB_ADDR, reg, &value, 1, 100) : err;
    }

    esp_err_t Read(uint8_t reg, uint8_t& value) {
        esp_err_t err = Check();
        return err == ESP_OK ? power_i2c_write_read(&kBus, CAM_SCCB_ADDR, reg, &value, 1, 100) : err;
    }

    esp_err_t Prepare(DvpCameraDetectInfo& info) {
        esp_err_t err = Check();
        if (err != ESP_OK) {
            return err;
        }
        if (request_.keep_awake) {
            PowerPolicy::GetInstance().Acquire(PowerNeed::StandbyInhibit);
            power_lease_ = true;
        }
        ESP_LOGI(kTag, "prepare: I2C1 SDA=%d SCL=%d, sensor=0x%02x, shared P5/P12 check", int(I2C_SDA_PIN),
                 int(I2C_SCL_PIN), CAM_SCCB_ADDR);
        err = epd_board_adapter_camera_acquire(100);
        LogStep("power/reset lease", err);
        if (err != ESP_OK) {
            return err;
        }
        rail_lease_ = true;
        // Like the demo: initialize the DVP controller and its LCD_CAM XCLK
        // before SCCB access. Keep this same controller until the session ends.
        ESP_LOGI(kTag, "MCLK: LCD_CAM controller=0 GPIO%d target=%u Hz", int(CAM_PIN_MCLK), unsigned(CAM_MCLK_HZ));
        err = InitController();
        LogStep("LCD_CAM controller/MCLK init", err);
        if (err != ESP_OK) {
            return err;
        }
        // This is the requested hardware configuration, not a GPIO measurement.
        ESP_LOGI(kTag, "MCLK: LCD_CAM configured %u Hz on GPIO%d, settle=10ms", unsigned(CAM_MCLK_HZ),
                 int(CAM_PIN_MCLK));
        vTaskDelay(pdMS_TO_TICKS(10));
        err = Check();
        if (err != ESP_OK) {
            return LogStep("before SCCB probe", err);
        }
        err = power_i2c_probe(&kBus, CAM_SCCB_ADDR, 100);
        LogStep("SCCB probe 0x3C (address ACK)", err);
        if (err != ESP_OK) {
            return err;
        }
        err = Write(0xfe, 0);  // Select sensor page only; never toggle shared P12.
        LogStep("SCCB select page 0 (reg 0xFE=0x00)", err);
        if (err != ESP_OK) {
            return err;
        }
        uint8_t pid[2]{};
        // Match the sensor driver's single-register SCCB reads; do not rely on
        // an undocumented sequential-read/auto-increment behavior.
        err = Read(0xf0, pid[0]);
        LogStep("PID high (reg 0xF0)", err);
        if (err != ESP_OK) {
            return err;
        }
        ESP_LOGI(kTag, "PID high: 0x%02x", pid[0]);
        err = Read(0xf1, pid[1]);
        LogStep("PID low (reg 0xF1)", err);
        if (err != ESP_OK) {
            return err;
        }
        ESP_LOGI(kTag, "PID low: 0x%02x", pid[1]);
        info.pid = (uint16_t(pid[0]) << 8) | pid[1];
        ESP_LOGI(kTag, "sensor @0x%02x PID=0x%04x MCLK=%d", CAM_SCCB_ADDR, info.pid, CAM_MCLK_HZ);
        if (info.pid != 0x2145) {
            ESP_LOGE(kTag, "PID mismatch: got=0x%04x expected GC2145=0x2145", info.pid);
            return ESP_ERR_NOT_FOUND;
        }
        return Check();
    }

    esp_err_t ConfigureCapture() {
        // Official VGA table starts with the GC2145 software reset + 16ms delay.
        // Mark output for cleanup before applying a partially successful table.
        stream_on_ = true;
        for (const auto& entry : kGc2145Vga) {
            esp_err_t err = Check();
            if (err != ESP_OK) {
                return err;
            }
            if (entry.reg == 0xff) {
                vTaskDelay(pdMS_TO_TICKS(entry.value));
            } else if ((err = Write(entry.reg, entry.value)) != ESP_OK) {
                return err;
            }
        }
        esp_err_t err = Write(0xfe, 0);
        if (err == ESP_OK) {
            err = Write(0xf2, 0);
        }
        if (err != ESP_OK) {
            return err;
        }

        frame_ = static_cast<uint8_t*>(
            esp_cam_ctlr_alloc_buffer(controller_, kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
        done_ = xSemaphoreCreateBinaryStatic(&done_storage_);
        if (!frame_ || !done_) {
            return ESP_ERR_NO_MEM;
        }
        esp_cam_ctlr_evt_cbs_t callbacks{};
        callbacks.on_get_new_trans = NextFrame;
        callbacks.on_trans_finished = FrameDone;
        err = esp_cam_ctlr_register_event_callbacks(controller_, &callbacks, this);
        if (err != ESP_OK) {
            return err;
        }
        return ESP_OK;
    }

    esp_err_t Grab() {
        struct StopOnExit {
            DvpCameraSession& session;
            ~StopOnExit() {
                if (session.started_) {
                    esp_cam_ctlr_stop(session.controller_);
                    session.started_ = false;
                }
                if (session.enabled_) {
                    esp_cam_ctlr_disable(session.controller_);
                    session.enabled_ = false;
                }
            }
        } stop{*this};
        deadline_ = esp_timer_get_time() + int64_t(request_.timeout_ms) * 1000;
        esp_err_t err = Check();
        if (err == ESP_OK && !frame_) {
            err = ConfigureCapture();
        }
        if (err != ESP_OK) {
            return err;
        }
        requests_ = 0;
        received_ = 0;
        xSemaphoreTake(done_, 0);
        // IDF 的 stop() 会关闭采集硬件；start() 仅启动 DMA，必须重新配对 enable/disable。
        err = esp_cam_ctlr_enable(controller_);
        if (err != ESP_OK) {
            return LogStep("capture enable", err);
        }
        enabled_ = true;
        // start() changes state even if its initial DMA setup fails, so clean up either way.
        started_ = true;
        err = esp_cam_ctlr_start(controller_);
        if (err == ESP_OK) {
            err = Write(0xf2, 0x0f);
        }
        if (err != ESP_OK) {
            return err;
        }
        while (xSemaphoreTake(done_, pdMS_TO_TICKS(20)) != pdTRUE) {
            if ((err = Check()) != ESP_OK) {
                if (err == ESP_ERR_TIMEOUT) {
                    ESP_LOGE(kTag, "operation=capture stage=wait_frame code=%d transactions=%u received=%u expected=%u",
                             int(err), requests_.load(), unsigned(received_), unsigned(kFrameBytes));
                }
                return err;
            }
        }
        err = Check();
        if (err != ESP_OK) {
            return err;
        }
        err = esp_cam_ctlr_stop(controller_);
        if (err != ESP_OK) {
            return err;
        }
        started_ = false;
        err = esp_cam_ctlr_disable(controller_);
        if (err != ESP_OK) {
            return LogStep("capture disable", err);
        }
        enabled_ = false;
        err = Write(0xf2, 0);
        if (err != ESP_OK) {
            return err;
        }
        warmup_ = 0;
        ESP_LOGD(kTag, "captured %ux%u RGB565X, %u bytes", kWidth, kHeight, unsigned(received_));
        return received_ == kFrameBytes ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }

    const uint8_t* Frame() const {
        return frame_;
    }

    uint8_t* TakeFrame() {
        return std::exchange(frame_, nullptr);
    }

private:
    esp_err_t InitController() {
        esp_cam_ctlr_dvp_pin_config_t pins{};
        pins.data_width = CAM_CTLR_DATA_WIDTH_8;
        for (int i = 0; i < ESP_CAM_CTLR_DVP_DATA_SIG_NUM; ++i) {
            pins.data_io[i] = i < 8 ? gpio_num_t(int(CAM_PIN_D0) + i) : GPIO_NUM_NC;
        }
        pins.pclk_io = CAM_PIN_PCLK;
        pins.vsync_io = CAM_PIN_VSYNC;
        pins.de_io = CAM_PIN_HREF;
        pins.xclk_io = CAM_PIN_MCLK;
        esp_cam_ctlr_dvp_config_t config{};
        config.clk_src = CAM_CLK_SRC_DEFAULT;
        config.h_res = kWidth;
        config.v_res = kHeight;
        config.input_data_color_type = CAM_CTLR_COLOR_RGB565;
        config.output_data_color_type = CAM_CTLR_COLOR_RGB565;
        config.cam_data_width = 8;
        config.external_xtal = false;
        config.xclk_freq = CAM_MCLK_HZ;
        config.dma_burst_size = 64;
        config.pin = &pins;
        // Leave byte_swap disabled: preserve the sensor's RGB565_BE byte order.
        std::lock_guard<std::mutex> lock(firmware_internal_budget_mutex);
        const size_t free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (free < kFirmwareInternalFloorBytes + kControllerInternalBudgetBytes) {
            ESP_LOGW(kTag, "operation=prepare stage=controller result=no_memory free=%u required=%u",
                     unsigned(free), unsigned(kFirmwareInternalFloorBytes + kControllerInternalBudgetBytes));
            return ESP_ERR_NO_MEM;
        }
        const esp_err_t error = esp_cam_new_dvp_ctlr(&config, &controller_);
        if (error == ESP_OK &&
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < kFirmwareInternalFloorBytes) {
            const esp_err_t cleanup = esp_cam_ctlr_del(controller_);
            if (cleanup == ESP_OK) {
                controller_ = nullptr;
                gpio_reset_pin(CAM_PIN_MCLK);
            } else {
                LogStep("controller budget rollback", cleanup);
            }
            ESP_LOGE(kTag, "operation=prepare stage=controller result=budget_exceeded");
            return ESP_ERR_NO_MEM;
        }
        return error;
    }

    static bool IRAM_ATTR NextFrame(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t* trans, void* arg) {
        auto* self = static_cast<DvpCameraSession*>(arg);
        // The IDF starts the NEXT DMA transaction before delivering FrameDone.
        // Supply our output only once; all warmup/subsequent frames use the
        // driver's backup buffer so the completed photo can never be overwritten.
        if (++self->requests_ == self->warmup_ + 1) {
            trans->buffer = self->frame_;
            trans->buflen = kFrameBytes;
        }
        return false;
    }

    static bool IRAM_ATTR FrameDone(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t* trans, void* arg) {
        auto* self = static_cast<DvpCameraSession*>(arg);
        if (trans->buffer != self->frame_) {
            return false;
        }
        self->received_ = trans->received_size;
        BaseType_t wake = pdFALSE;
        xSemaphoreGiveFromISR(self->done_, &wake);
        return wake == pdTRUE;
    }

    DvpCameraRequest request_;
    int64_t deadline_;
    bool power_lease_ = false, rail_lease_ = false, stream_on_ = false;
    bool enabled_ = false, started_ = false;
    esp_cam_ctlr_handle_t controller_ = nullptr;
    uint8_t* frame_ = nullptr;
    SemaphoreHandle_t done_ = nullptr;
    StaticSemaphore_t done_storage_{}; // 控制块随会话保存，不再从内部堆动态申请
    std::atomic<unsigned> requests_{0}; // ISR 提交数用于区分无帧和交付超时
    unsigned warmup_ = kWarmupFrames;
    size_t received_ = 0;
};

esp_err_t DvpCameraDetect(DvpCameraDetectInfo& info, const DvpCameraRequest& request) {
    info = {};
    std::unique_lock<std::mutex> lock(s_operation, std::try_to_lock);
    if (!lock.owns_lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    // Controller initialization/cleanup touches LCD_CAM shared with the panel.
    FlashCacheGuard flash_guard;
    DvpCameraSession session(request);
    return session.Prepare(info);
}

esp_err_t DvpCameraCapture(DvpCameraGrayImage& image, const DvpCameraRequest& request) {
    image = {};
    std::unique_lock<std::mutex> lock(s_operation, std::try_to_lock);
    if (!lock.owns_lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    // Serialize PSRAM DMA with panel waveforms/OTA. Never wait for LVGL under this gate.
    FlashCacheGuard flash_guard;
    DvpCameraSession session(request);
    DvpCameraDetectInfo info;
    esp_err_t err = session.Prepare(info);
    if (err == ESP_OK) {
        err = session.Grab();
    }
    if (err != ESP_OK) {
        return err;
    }
    const auto* raw = session.Frame();
    image.width = kWidth;
    image.height = kHeight;
    try {
        image.pixels.resize(size_t(image.width) * image.height);
    } catch (const std::bad_alloc&) {
        image = {};
        return ESP_ERR_NO_MEM;
    }
    for (unsigned y = 0; y < image.height; ++y) {
        if ((err = session.Check()) != ESP_OK) {
            image = {};
            return err;
        }
        for (unsigned x = 0; x < image.width; ++x) {
            const size_t index = size_t(y) * image.width + x;
            const uint8_t gray = Rgb565BeLuma(raw[index * 2], raw[index * 2 + 1]);
            image.pixels[index] = gray;
        }
    }
    ESP_LOGI(kTag, "gray preview ready: %ux%u, %u bytes", image.width, image.height, unsigned(image.pixels.size()));
    return ESP_OK;
}

esp_err_t DvpCameraCaptureJpeg(uint8_t** out_data, size_t* out_len, const DvpCameraRequest& request) {
    if (out_data) {
        *out_data = nullptr;
    }
    if (out_len) {
        *out_len = 0;
    }
    if (!out_data || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    std::unique_lock<std::mutex> lock(s_operation, std::try_to_lock);
    if (!lock.owns_lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> frame(nullptr, heap_caps_free);
    // DVP 与 JPEG 的 PSRAM DMA 均与刷屏互斥，HTTP 上传在此作用域外执行。
    FlashCacheGuard flash_guard;
    {
        DvpCameraSession session(request);
        DvpCameraDetectInfo info;
        esp_err_t err = session.Prepare(info);
        if (err == ESP_OK) {
            err = session.Grab();
        }
        if (err != ESP_OK) {
            return err;
        }
        frame.reset(session.TakeFrame());
    }
    // GC2145 的高字节在前，硬件 JPEG 的 RGB565 输入使用小端像素。
    for (size_t i = 0; i < kFrameBytes; i += 2) {
        std::swap(frame.get()[i], frame.get()[i + 1]);
    }
    jpeg_encode_memory_alloc_cfg_t memory{};
    memory.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
    size_t capacity = 0;
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> jpeg(
        static_cast<uint8_t*>(jpeg_alloc_encoder_mem(kFrameBytes, &memory, &capacity)), heap_caps_free);
    if (!jpeg) {
        return ESP_ERR_NO_MEM;
    }
    jpeg_encode_cfg_t config{};
    config.width = kWidth;
    config.height = kHeight;
    config.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
    config.sub_sample = JPEG_DOWN_SAMPLING_YUV420;
    config.image_quality = 80;
    jpeg_encode_engine_cfg_t engine_config{};
    engine_config.timeout_ms = 1000;
    jpeg_encoder_handle_t encoder = nullptr;
    esp_err_t err = jpeg_new_encoder_engine(&engine_config, &encoder);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t length = 0;
    err = jpeg_encoder_process(encoder, &config, frame.get(), kFrameBytes, jpeg.get(), capacity, &length);
    jpeg_del_encoder_engine(encoder);
    if (err != ESP_OK || length == 0 || length > capacity) {
        ESP_LOGE(kTag, "JPEG encode failed: %s", esp_err_to_name(err));
        return err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
    }
    auto* compact =
        static_cast<uint8_t*>(heap_caps_realloc(jpeg.get(), size_t(length), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (compact) {
        jpeg.release();
        jpeg.reset(compact);
    }
    *out_len = size_t(length);
    *out_data = jpeg.release();
    ESP_LOGI(kTag, "JPEG ready: %ux%u, %u bytes", kWidth, kHeight, unsigned(length));
    return ESP_OK;
}

esp_err_t DvpCameraStreamPreview(const CameraCaptureRequest& request, CameraPreviewFrameHandler handler, void* context) {
    if (!handler || request.max_width < 1 || request.max_height < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    std::unique_lock<std::mutex> operation(s_operation, std::try_to_lock);
    if (!operation.owns_lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    FirmwareMemoryDemand demand(kFrameBytes * 2);
    if (!FirmwarePsramCanAllocate(kFrameBytes * 2)) {
        return ESP_ERR_NO_MEM;
    }
    const auto destroy = [](DvpCameraSession* session) {
        FlashCacheGuard gate;
        delete session;
    };
    const DvpCameraRequest capture{request.timeout_ms, request.cancelled, request.context, false};
    std::unique_ptr<DvpCameraSession, decltype(destroy)> session(new (std::nothrow) DvpCameraSession(capture), destroy);
    if (!session) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result;
    {
        FlashCacheGuard gate;
        DvpCameraDetectInfo info;
        result = session->Prepare(info);
        if (result == ESP_OK) {
            result = session->Grab();
        }
    }
    const double scale = std::min({1.0, double(request.max_width) / kWidth, double(request.max_height) / kHeight});
    while (result == ESP_OK && !(request.cancelled && request.cancelled(request.context))) {
        RasterImage image;
        image.width = std::max(1, int(kWidth * scale));
        image.height = std::max(1, int(kHeight * scale));
        image.pixels.resize(size_t(image.width) * image.height);
        const uint8_t* raw = session->Frame();
        for (int y = 0; y < image.height; ++y) {
            const int sy = int64_t(y) * kHeight / image.height;
            for (int x = 0; x < image.width; ++x) {
                const int sx = int64_t(x) * kWidth / image.width;
                const size_t pixel = (size_t(sy) * kWidth + sx) * 2;
                image.pixels[size_t(y) * image.width + x] = Rgb565BeLuma(raw[pixel], raw[pixel + 1]);
            }
        }
        image.BindDsc();
        if (!handler(std::move(image), context)) {
            break;
        }
        if (!(request.cancelled && request.cancelled(request.context))) {
            FlashCacheGuard gate;
            result = session->Grab();
        }
    }
    return result;
}
