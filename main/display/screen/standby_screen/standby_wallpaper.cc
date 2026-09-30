#include "standby_screen/standby_wallpaper.h"

#include "wallpaper_screen/wallpaper_active.h"

#include "config.h"
#include "image_util.h"
#include "reader_types.h"

#include <atomic>
#include <cstdio>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <lvgl.h>
#include "assets/lang_config.h"


static constexpr const char* TAG = "StandbyWp";
static constexpr int kDecodeMaxW = DISPLAY_WIDTH;
static constexpr int kDecodeMaxH = DISPLAY_HEIGHT;
static constexpr uint32_t kLoadStack = 12 * 1024;

enum class StandbyWpLoadState : uint8_t { Idle = 0, Loading = 1, Ready = 2, Failed = 3 };

struct StandbyWpUiState {
    lv_obj_t* root = nullptr;
    lv_obj_t* img = nullptr;
};

static StandbyWpUiState s_ui;
static bool s_alive = false;
static uint32_t s_epoch = 0;
static reader::RasterImage* s_raster = nullptr;
static std::atomic<bool> s_load_busy{false};
static std::atomic<StandbyWpLoadState> s_load_state{StandbyWpLoadState::Idle};

struct StandbyWpLoadWork {
    uint32_t epoch = 0;
};

struct StandbyWpLoadResult {
    uint32_t epoch = 0;
    bool ok = false;
    char err[64] = {};
    reader::RasterImage* img = nullptr;
};

static void MarkLoadDone(StandbyWpLoadState st) {
    s_load_state.store(st, std::memory_order_release);
}
static void DetachImage() {
    if (s_ui.img != nullptr) {
        lv_image_set_src(s_ui.img, nullptr);
        lv_obj_add_flag(s_ui.img, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_raster != nullptr) {
        delete s_raster;
        s_raster = nullptr;
    }
}

static void ApplyLoadAsync(void* p) {
    auto* msg = static_cast<StandbyWpLoadResult*>(p);
    s_load_busy.store(false, std::memory_order_release);
    if (msg == nullptr) {
        return;
    }
    if (!s_alive || msg->epoch != s_epoch) {
        delete msg->img;
        delete msg;
        return;
    }
    if (!msg->ok || msg->img == nullptr || msg->img->empty()) {
        ESP_LOGW(TAG, "load failed: %s", msg->err[0] != '\0' ? msg->err : "unknown");
        MarkLoadDone(StandbyWpLoadState::Failed);
        delete msg->img;
        delete msg;
        return;
    }

    DetachImage();
    s_raster = msg->img;
    msg->img = nullptr;
    if (s_ui.img != nullptr) {
        lv_image_set_src(s_ui.img, &s_raster->dsc);
        lv_obj_set_size(s_ui.img, s_raster->width, s_raster->height);
        lv_obj_center(s_ui.img);
        lv_obj_clear_flag(s_ui.img, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ui.root != nullptr) {
        lv_obj_invalidate(s_ui.root);
        lv_refr_now(nullptr);
    }
    MarkLoadDone(StandbyWpLoadState::Ready);
    delete msg;
}

static void LoadTask(void* arg) {
    auto* work = static_cast<StandbyWpLoadWork*>(arg);
    auto* msg = new StandbyWpLoadResult{};
    msg->epoch = work != nullptr ? work->epoch : 0;
    delete work;

    char path[192] = {};
    if (!Wallpaper_TryResolveStandbyWallpaperPath(path, sizeof(path))) {
        std::snprintf(msg->err, sizeof(msg->err), "%s", Lang::Strings::STANDBY_WP_NOT_FOUND);
    } else {
        auto* img = new reader::RasterImage();
        // 与壁纸预览一致走 L8：本板全屏 I1 lv_image 易白底
        if (!reader::DecodeImageFileToL8(path, kDecodeMaxW, kDecodeMaxH, *img) || img->empty()) {
            std::snprintf(msg->err, sizeof(msg->err), "%s", Lang::Strings::STANDBY_DECODE_FAIL);
            delete img;
        } else {
            msg->ok = true;
            msg->img = img;
            ESP_LOGI(TAG, "decoded standby wallpaper %s (%ux%u)", path,
                     static_cast<unsigned>(img->width), static_cast<unsigned>(img->height));
        }
    }

    if (lv_async_call(ApplyLoadAsync, msg) != LV_RESULT_OK) {
        delete msg->img;
        delete msg;
        s_load_busy.store(false, std::memory_order_release);
        MarkLoadDone(StandbyWpLoadState::Failed);
    }
    vTaskDelete(nullptr);
}


void StandbyWallpaper_Build(lv_obj_t* root) {
    if (root == nullptr) {
        return;
    }
    ++s_epoch;
    s_alive = true;
    s_ui = StandbyWpUiState{};
    MarkLoadDone(StandbyWpLoadState::Idle);

    lv_obj_set_style_bg_color(root, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.root = root;
    s_ui.img = lv_image_create(root);
    lv_obj_add_flag(s_ui.img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.img, LV_OBJ_FLAG_CLICKABLE);
}

void StandbyWallpaper_StartLoad() {
    if (!s_alive) {
        return;
    }
    if (s_load_busy.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    MarkLoadDone(StandbyWpLoadState::Loading);
    auto* work = new StandbyWpLoadWork{s_epoch};
    // 栈放 SPIRAM：解码峰值与壁纸缩略图任务同级
    if (xTaskCreatePinnedToCoreWithCaps(LoadTask, "stb_wp_load", kLoadStack, work, 5, nullptr, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        delete work;
        s_load_busy.store(false, std::memory_order_release);
        MarkLoadDone(StandbyWpLoadState::Failed);
        ESP_LOGW(TAG, "load task create failed");
    }
}

bool StandbyWallpaper_IsContentReady() {
    const StandbyWpLoadState st = s_load_state.load(std::memory_order_acquire);
    return st == StandbyWpLoadState::Ready || st == StandbyWpLoadState::Failed;
}

void StandbyWallpaper_Teardown() {
    ++s_epoch;  // 丢弃在途 StandbyWpLoadResult，避免 UAF
    s_alive = false;
    DetachImage();
    s_ui = StandbyWpUiState{};
    MarkLoadDone(StandbyWpLoadState::Idle);
    // 不强制清 s_load_busy：任务结束时 ApplyLoadAsync / 失败路径会清；
    // 若任务仍跑，epoch 不匹配会释放像素且清 busy。
}

void StandbyWallpaper_InvalidateDecoded() {
    ++s_epoch;
    DetachImage();
    if (s_alive) {
        MarkLoadDone(StandbyWpLoadState::Idle);
    }
}

