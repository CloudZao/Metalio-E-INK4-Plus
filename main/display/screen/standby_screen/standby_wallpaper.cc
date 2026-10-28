#include "standby_screen/standby_wallpaper.h"
#include "sd_paths.h"

#include "wallpaper_screen/wallpaper_active.h"

#include "image_util.h"
#include "reader_types.h"
#include "sd_access.h"

#include <atomic>
#include <cstdio>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <lvgl.h>
#include "assets/lang_config.h"


static constexpr const char* TAG = "StandbyWp";
static constexpr uint32_t kLoadStack = 12 * 1024;

enum class StandbyWpLoadState : uint8_t { Idle = 0, Loading = 1, Ready = 2, Failed = 3 };

static bool s_alive = false;
static uint32_t s_epoch = 0;
static reader::RasterImage* s_raster = nullptr;
static std::atomic<bool> s_load_busy{false};
static std::atomic<int64_t> s_decode_started_us{0}; // 解码开始时间，完成后清零
static std::atomic<StandbyWpLoadState> s_load_state{StandbyWpLoadState::Idle};

struct StandbyWpLoadResult {
    uint32_t epoch = 0; // 对应的界面版本
    bool ok = false; // 解码是否成功
    char err[64] = {}; // 失败原因
    reader::RasterImage* img = nullptr; // 接收前由结果持有的像素
};

static std::atomic<StandbyWpLoadResult*> s_pending_result{nullptr}; // 后台发布、显示任务接收的结果

static void MarkLoadDone(StandbyWpLoadState st) {
    s_load_state.store(st, std::memory_order_release);
}
static void FreeDecodedImage() {
    if (s_raster != nullptr) {
        delete s_raster;
        s_raster = nullptr;
    }
}

static void ApplyLoadResult(StandbyWpLoadResult* msg) {
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
    msg->img = nullptr;
    // 待机与关机共用全屏绘制，不挂入底层页面的 LVGL 层级。
    MarkLoadDone(StandbyWpLoadState::Ready);
    delete msg;
}

static void LoadTask(void* arg) {
    auto* msg = static_cast<StandbyWpLoadResult*>(arg);

    {
        sd_access::Permit permit;
        char path[SD_FILE_PATH_CAPACITY] = {};
        if (!Wallpaper_TryResolveStandbyWallpaperPath(path, sizeof(path))) {
            std::snprintf(msg->err, sizeof(msg->err), "%s", Lang::Strings::STANDBY_WP_NOT_FOUND);
        } else {
            auto* img = new reader::RasterImage();
            if (!reader::DecodeImageFileToL8(path, 0, 0, *img) || img->empty()) {
                std::snprintf(msg->err, sizeof(msg->err), "%s", Lang::Strings::STANDBY_DECODE_FAIL);
                delete img;
            } else {
                msg->ok = true;
                msg->img = img;
                ESP_LOGI(TAG, "decoded standby wallpaper %s (%ux%u)", path,
                         static_cast<unsigned>(img->width), static_cast<unsigned>(img->height));
            }
        }
    }
    s_decode_started_us.store(0, std::memory_order_release);
    s_pending_result.store(msg, std::memory_order_release);
    vTaskDeleteWithCaps(nullptr);
}


void StandbyWallpaper_Build(lv_obj_t* root) {
    if (root == nullptr) {
        return;
    }
    ++s_epoch;
    s_alive = true;
    MarkLoadDone(StandbyWpLoadState::Idle);

    // 解码期间保留底层页面，完整壁纸由 Park 直接写入帧缓冲。
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

}

void StandbyWallpaper_StartLoad() {
    if (!s_alive) {
        return;
    }
    if (s_load_busy.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    MarkLoadDone(StandbyWpLoadState::Loading);
    s_decode_started_us.store(esp_timer_get_time(), std::memory_order_release);
    auto* msg = new StandbyWpLoadResult{};
    msg->epoch = s_epoch;
    // 栈放 SPIRAM：解码峰值与壁纸缩略图任务同级
    if (xTaskCreatePinnedToCoreWithCaps(LoadTask, "stb_wp_load", kLoadStack, msg, 5, nullptr, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        delete msg;
        s_decode_started_us.store(0, std::memory_order_release);
        s_load_busy.store(false, std::memory_order_release);
        MarkLoadDone(StandbyWpLoadState::Failed);
        ESP_LOGW(TAG, "load task create failed");
    }
}

bool StandbyWallpaper_IsContentReady() {
    const StandbyWpLoadState st = s_load_state.load(std::memory_order_acquire);
    return st == StandbyWpLoadState::Ready || st == StandbyWpLoadState::Failed;
}

void StandbyWallpaper_Poll() {
    if (auto* msg = s_pending_result.exchange(nullptr, std::memory_order_acquire)) {
        ApplyLoadResult(msg);
    }
}

bool StandbyWallpaper_IsLoadingSlow() {
    const int64_t started = s_decode_started_us.load(std::memory_order_acquire);
    return s_alive && s_load_state.load(std::memory_order_acquire) == StandbyWpLoadState::Loading &&
           started != 0 && esp_timer_get_time() - started >= 1000000;
}

const reader::RasterImage* StandbyWallpaper_DecodedImage() {
    return s_alive && s_load_state.load(std::memory_order_acquire) == StandbyWpLoadState::Ready
        ? s_raster : nullptr;
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

