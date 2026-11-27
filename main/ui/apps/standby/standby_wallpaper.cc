#include <atomic>
#include <cstdio>
#include <memory>
#include <new>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <lvgl.h>
#include "infra/tasks/task_create.h"
#include "sysmgmt/power/standby_wallpaper.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "boards/common/storage/sd_access.h"
#include "infra/storage/sd_paths.h"
#include "infra/image/image_decode.h"
#include "business/reader/reader_types.h"
#include "business/wallpaper/wallpaper_active.h"
#include "config.h"
#include "ui/common/drawing/rendering/wallpaper_frame.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"

static constexpr const char* TAG = "StandbyWp";
static constexpr uint32_t kLoadStack = 12 * 1024;

enum class StandbyWpLoadState : uint8_t { Idle = 0, Loading = 1, Ready = 2, Failed = 3 };

static bool s_alive = false;
static uint32_t s_epoch = 0;
static RasterImage* s_raster = nullptr;
static bool s_overlay = false;
static std::vector<uint8_t> s_alpha;
static std::atomic<bool> s_load_busy{false};
static std::atomic<int64_t> s_decode_started_us{0};  // 解码开始时间，完成后清零
static std::atomic<StandbyWpLoadState> s_load_state{StandbyWpLoadState::Idle};

struct StandbyWpLoadResult : ScreenUiCompletion {
    uint32_t epoch = 0;          // 对应的界面版本
    bool ok = false;             // 解码是否成功
    char err[64] = {};           // 失败原因
    RasterImage* img = nullptr;  // 接收前由结果持有的像素
    bool overlay = false;        // 等比缩小并保留透明度
    lv_point_t size{};           // 竖屏壁纸解码范围
    std::vector<uint8_t> alpha;  // 图片自带的透明度
};

static void MarkLoadDone(StandbyWpLoadState st) {
    s_load_state.store(st, std::memory_order_release);
}

static void FreeDecodedImage() {
    std::vector<uint8_t>().swap(s_alpha);
    if (s_raster != nullptr) {
        delete s_raster;
        s_raster = nullptr;
    }
}

static void ApplyLoadResult(ScreenUiCompletion* completion) {
    auto* msg = static_cast<StandbyWpLoadResult*>(completion);
    s_load_busy.store(false, std::memory_order_release);
    if (!s_alive || msg->epoch != s_epoch) {
        delete msg->img;
        delete msg;
        if (s_alive) {
            StandbyWallpaper_StartLoad();
        }
        return;
    }
    if (!msg->ok || msg->img == nullptr || msg->img->empty()) {
        ESP_LOGW(TAG, "load failed: %s", msg->err[0] != '\0' ? msg->err : "unknown");
        MarkLoadDone(StandbyWpLoadState::Failed);
        delete msg->img;
        delete msg;
        return;
    }

    FreeDecodedImage();
    s_raster = msg->img;
    s_alpha = std::move(msg->alpha);
    msg->img = nullptr;
    // 待机与关机共用全屏绘制，不挂入底层页面的 LVGL 层级。
    MarkLoadDone(StandbyWpLoadState::Ready);
    delete msg;
}

static void LoadTask(void* arg) {
    auto* msg = static_cast<StandbyWpLoadResult*>(arg);

    try {
        SdAccessPermit permit;
        char path[SD_FILE_PATH_CAPACITY] = {};
        if (!Wallpaper_TryResolveStandbyWallpaperPath(path, sizeof(path))) {
            std::snprintf(msg->err, sizeof(msg->err), "%s", LangString_STANDBY_WP_NOT_FOUND);
        } else {
            auto img = std::make_unique<RasterImage>();
            const bool decoded = msg->overlay ? DecodeImageFileOverlay(path, msg->size.x, msg->size.y, *img, msg->alpha)
                                              : DecodeImageFileToL8(path, msg->size.x, msg->size.y, *img);
            if (!decoded || img->empty()) {
                std::snprintf(msg->err, sizeof(msg->err), "%s", LangString_STANDBY_DECODE_FAIL);
            } else {
                msg->ok = true;
                msg->img = img.release();
                ESP_LOGI(TAG, "decoded standby wallpaper %ux%u", static_cast<unsigned>(msg->img->width),
                         static_cast<unsigned>(msg->img->height));
            }
        }
    } catch (const std::bad_alloc&) {
        msg->ok = false;
        std::snprintf(msg->err, sizeof(msg->err), "%s", LangString_COMMON_MEMORY_RETRY);
    }
    s_decode_started_us.store(0, std::memory_order_release);
    ScreenCompleteWork(msg, true);
    vTaskDeleteWithCaps(nullptr);
}

void StandbyWallpaper_Build(lv_obj_t* root) {
    if (root == nullptr) {
        return;
    }
    ++s_epoch;
    s_alive = true;
    s_overlay = Wallpaper_IsOverlay(true) && WallpaperFrame_HasBase();
    MarkLoadDone(StandbyWpLoadState::Idle);

    // 解码期间保留底层页面，完整壁纸由 Park 直接写入帧缓冲。
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
}

void StandbyWallpaper_StartLoad() try {
    if (!s_alive) {
        return;
    }
    if (s_load_busy.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    MarkLoadDone(StandbyWpLoadState::Loading);
    s_decode_started_us.store(esp_timer_get_time(), std::memory_order_release);
    auto* msg = new StandbyWpLoadResult{};
    msg->apply = ApplyLoadResult;
    msg->epoch = s_epoch;
    msg->overlay = s_overlay;
    msg->size = s_overlay ? WallpaperFrame_Size() : lv_point_t{DISPLAY_WIDTH, DISPLAY_HEIGHT};
    // 栈放 SPIRAM：解码峰值与壁纸缩略图任务同级
    if (FirmwareCreateTask(LoadTask, "stb_wp_load", kLoadStack, msg, 5, nullptr, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        delete msg;
        s_decode_started_us.store(0, std::memory_order_release);
        s_load_busy.store(false, std::memory_order_release);
        MarkLoadDone(StandbyWpLoadState::Failed);
        ESP_LOGW(TAG, "load task create failed");
    }
} catch (const std::bad_alloc&) {
    s_decode_started_us.store(0, std::memory_order_release);
    s_load_busy.store(false, std::memory_order_release);
    MarkLoadDone(StandbyWpLoadState::Failed);
    ESP_LOGW(TAG, "operation=load stage=allocate result=no_memory");
}

bool StandbyWallpaper_IsContentReady() {
    const StandbyWpLoadState st = s_load_state.load(std::memory_order_acquire);
    return st == StandbyWpLoadState::Ready || st == StandbyWpLoadState::Failed;
}

bool StandbyWallpaper_IsLoadingSlow() {
    const int64_t started = s_decode_started_us.load(std::memory_order_acquire);
    return s_alive && s_load_state.load(std::memory_order_acquire) == StandbyWpLoadState::Loading && started != 0 &&
           esp_timer_get_time() - started >= 1000000;
}

const RasterImage* StandbyWallpaper_DecodedImage() {
    return s_alive && s_load_state.load(std::memory_order_acquire) == StandbyWpLoadState::Ready ? s_raster : nullptr;
}

bool StandbyWallpaper_IsOverlay() {
    return s_alive && s_overlay;
}

const std::vector<uint8_t>& StandbyWallpaper_DecodedAlpha() {
    return s_alpha;
}

void StandbyWallpaper_ReleasePixels() {
    FreeDecodedImage();
}

void StandbyWallpaper_Teardown() {
    ++s_epoch;  // 丢弃在途 StandbyWpLoadResult，避免 UAF
    s_alive = false;
    FreeDecodedImage();
    MarkLoadDone(StandbyWpLoadState::Idle);
    // 在途结果由显示任务接收后释放；此前保持 busy，避免重叠解码。
}

void StandbyWallpaper_InvalidateDecoded() {
    ++s_epoch;
    FreeDecodedImage();
    if (s_alive) {
        MarkLoadDone(StandbyWpLoadState::Idle);
    }
}
