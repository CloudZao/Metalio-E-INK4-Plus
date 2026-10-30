/**
 * @file lv_adapter_epdiy.cc
 * @brief LVGL9 L8 灰度渲染 → EPDiy；开机加载 HomeScreen
 */

#include "lv_adapter_epdiy.h"
#include "sd_paths.h"
#include "epd_refresh.h"
#include "epd_prepare.h"
#include "epd_content_policy.h"
#include "reader/image_tone.h"
 
#include "lv_adapter_epdiy_touch.h"

#include "application.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "assistant_screen/assistant_screen.h"
#include "screen/workbuddy_screen/workbuddy_screen.h"
#include "audio_codec.h"
#include "board.h"
#include "boot_key_handler.h"
#include "config.h"
#include "device_state.h"
#include "display_orient.h"
#include "epd_font24.h" // EPD_LOGICAL_W/H、epd_logical_to_physical
#include "epdiy.h"
#include "fontpack_lvgl.h"
#include "frontlight.h"
#include "home_screen/home_screen.h"
#include "metalio_epd.h"
#include "metalio_keys.h"
#include "metalio_touch.h"
#include "ota_confirm_dialog/ota_confirm_dialog.h"
#include "ota_upgrade_screen/ota_upgrade_screen.h"
#include "power_policy.h"
#include "screen_common.h"
#include "sd_access.h"
#include "standby_screen/standby_screen.h"
#include "standby_screen/standby_wallpaper.h"

namespace {
bool BlitL8ToEpdFullScreen(const reader::RasterImage& img);
}
#include "book_screen/book_screen.h"
#include "book_screen/book_nav.h"
#include "vk_key_handler.h"
#include "book_screen/reader/book_open_worker.h"
#include "book_screen/reader/book_reader_prefs.h"
#include "book_screen/settings/book_tap_zones.h"
#include "image_util.h"
#include "reader/book_home_snapshot.h"
#include "wallpaper_screen/wallpaper_active.h"

#include <cstdio>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lv_decoder.h>
#include <esp_lv_fs.h>
#include <esp_mmap_assets.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <mmap_generate_resources.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <font_awesome.h>
#include <vector>

extern "C" void* __wrap_lv_malloc_core(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

extern "C" void* __wrap_lv_realloc_core(void* memory, size_t size) {
    return heap_caps_realloc(memory, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

#define TAG "lv_epdiy"

static std::atomic<bool> s_force_full{false};
static std::atomic<bool> s_stream_paint{false};
static std::atomic<bool> s_freeze_updates{false};
static std::atomic<bool> s_defer_standby_commit{false}; // 进待机先攒 FB，Park 再推白上屏
static bool s_standby_loading_notice_shown = false; // 每次进入待机最多显示一次加载提示
static const lv_area_t* s_wallpaper_notice_clip = nullptr; // 同步刷新提示时仅写入提示矩形
static void ShowWallpaperLoading();
static std::atomic<bool> s_restore_frontlight_after_commit{false}; // 退出待机：底层上墨后再开前光
static SemaphoreHandle_t s_lvgl_mux = nullptr;

static mmap_assets_handle_t s_resources = nullptr;
static lv_display_t* s_disp = nullptr;
static uint8_t* s_draw_buffer = nullptr;
static size_t s_draw_buffer_bytes = 0;
static LvAdapterEpdiy* s_instance = nullptr;

LvAdapterEpdiy* LvAdapterEpdiy::Instance() {
    return s_instance;
}

/** StartNetwork/激活占着 app_main：须另起 Tick 才能把顶栏刷到墨水屏（对齐 397 LVGL 任务） */
static void DisplayTickTask(void* /*arg*/) {
    while (true) {
        auto* d = LvAdapterEpdiy::Instance();
        if (d != nullptr) {
            d->Tick();
            // 待机由 LP 任务消费长按请求，确保硬件恢复完成后再关机。
            if (!StandbyScreen::IsActive() && MetalioKeys_TakeShutdownPulse()) {
                PowerPolicy::GetInstance().RequestPowerOff();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void StartDisplayTickTaskOnce() {
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    // 栈字节；对齐 397 LVGL 16KB（局刷深 + 部分页勿再往栈上塞大缓冲）
    xTaskCreatePinnedToCore(DisplayTickTask, "lv_epdiy_tick", 16 * 1024, nullptr, 2, nullptr, 1);
}

/** LANDSCAPE 下直接写 4bpp FB（等同 epd_draw_pixel，避免每像素函数调用） */
static inline void FbPutNative(uint8_t* fb, int px, int py, uint8_t color, int fb_w) {
    uint8_t* buf_ptr = &fb[py * (fb_w / 2) + (px / 2)];
    const unsigned gray = (unsigned(color) * 15 + 127) / 255;
    if (px & 1) {
        *buf_ptr = static_cast<uint8_t>((*buf_ptr & 0x0F) | (gray << 4));
    } else {
        *buf_ptr = static_cast<uint8_t>((*buf_ptr & 0xF0) | gray);
    }
}

static void epd_commit_update(bool force_full) {
    EpdSubmit(force_full, s_stream_paint.load(),
              s_restore_frontlight_after_commit.exchange(false));
}

static uint32_t lv_tick_cb(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// Display resolution already excludes the panel's reserved border. Apply the
// same drawable bounds to every screen and overlay, including direct loads.
static void ConstrainDrawableRoot(lv_obj_t* root, int width, int height) {
    if (root == nullptr) return;
    lv_obj_remove_flag(root, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, width, height);
}

static void drawable_bounds_cb(lv_event_t* event) {
    auto* disp = static_cast<lv_display_t*>(lv_event_get_target(event));
    const int width = lv_display_get_horizontal_resolution(disp);
    const int height = lv_display_get_vertical_resolution(disp);
    lv_obj_t* screen = lv_event_get_code(event) == LV_EVENT_SCREEN_LOAD_START
        ? static_cast<lv_obj_t*>(lv_event_get_param(event)) : lv_display_get_screen_active(disp);
    ConstrainDrawableRoot(screen, width, height);
    ConstrainDrawableRoot(lv_display_get_layer_top(disp), width, height);
    ConstrainDrawableRoot(lv_display_get_layer_sys(disp), width, height);
    if (lv_event_get_code(event) == LV_EVENT_RESOLUTION_CHANGED) {
        // 全屏背景退出后清除边缘，防止旧像素残留在普通 UI 的内缩区。
        if (uint8_t* fb = EpdRenderBuffer()) {
            memset(fb, 0xFF, static_cast<size_t>(epd_width() / 2) * epd_height());
        }
        if (screen != nullptr) lv_obj_invalidate(screen);
    }
}

// LVGL owns the complete L8 result, including clipping, masks and layer blending.
// PARTIAL buffers start at area.x1/y1; only convert pixels inside this dirty area.
static void blit_l8_area_to_epdiy(const uint8_t* pixels, const lv_area_t* area, uint8_t* fb,
                                 int disp_w, int disp_h) {
    if (!pixels || !area || !fb || disp_w <= 0 || disp_h <= 0) return;
    int x1 = std::max<int>(area->x1, 0), y1 = std::max<int>(area->y1, 0);
    int x2 = std::min<int>(area->x2, disp_w - 1), y2 = std::min<int>(area->y2, disp_h - 1);
    if (s_wallpaper_notice_clip != nullptr) {
        x1 = std::max<int>(x1, s_wallpaper_notice_clip->x1);
        y1 = std::max<int>(y1, s_wallpaper_notice_clip->y1);
        x2 = std::min<int>(x2, s_wallpaper_notice_clip->x2);
        y2 = std::min<int>(y2, s_wallpaper_notice_clip->y2);
    }
    if (x1 > x2 || y1 > y2) return;
    const int stride = lv_draw_buf_width_to_stride(lv_area_get_width(area), LV_COLOR_FORMAT_L8);
    int px0, py0, px1, py1, px2, py2;
    DisplayUiLogicalToPanel(x1, y1, &px0, &py0);
    DisplayUiLogicalToPanel(x1 + 1, y1, &px1, &py1);
    DisplayUiLogicalToPanel(x1, y1 + 1, &px2, &py2);
    EpdBlitL8(fb, pixels, stride, area->x1, area->y1, x1, y1, x2, y2,
              epd_width(), epd_height(), px0, py0, px1-px0, py1-py0, px2-px0, py2-py0);
}

static void blit_i1_area_to_epdiy(const uint8_t* px_map, const lv_area_t* area, uint8_t* fb,
                                  int disp_w, int disp_h) {
    if (px_map == nullptr || area == nullptr || fb == nullptr || disp_w <= 0 || disp_h <= 0) {
        return;
    }
    int x1 = area->x1;
    int y1 = area->y1;
    int x2 = area->x2;
    int y2 = area->y2;
    if (x1 < 0) {
        x1 = 0;
    }
    if (y1 < 0) {
        y1 = 0;
    }
    if (x2 >= disp_w) {
        x2 = disp_w - 1;
    }
    if (y2 >= disp_h) {
        y2 = disp_h - 1;
    }
    if (s_wallpaper_notice_clip != nullptr) {
        x1 = std::max<int>(x1, s_wallpaper_notice_clip->x1);
        y1 = std::max<int>(y1, s_wallpaper_notice_clip->y1);
        x2 = std::min<int>(x2, s_wallpaper_notice_clip->x2);
        y2 = std::min<int>(y2, s_wallpaper_notice_clip->y2);
    }
    if (x2 < x1 || y2 < y1) {
        return;
    }
    const int area_w = x2 - x1 + 1;
    const int area_h = y2 - y1 + 1;
    const uint32_t pal_bytes =
        LV_COLOR_INDEXED_PALETTE_SIZE(LV_COLOR_FORMAT_I1) * (uint32_t)sizeof(lv_color32_t);
    // PARTIAL reshape：stride 对应该脏区宽度，不是全屏 disp_w
    const uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)(area->x2-area->x1+1), LV_COLOR_FORMAT_I1);
    const uint8_t* bits = px_map + pal_bytes;
    const int fb_w = epd_width();
    const int fb_h = epd_height();
    const int64_t t0 = esp_timer_get_time();

    // 底边/非整屏局刷：打一次关键日志，便于对照「横线 / 底黑条」
    if (y1 > 0 || y2 < disp_h - 1 || x1 > 0 || x2 < disp_w - 1) {
        ESP_LOGD(TAG, "blit partial area=%d,%d-%d,%d aw=%d ah=%d stride=%u disp=%dx%d", x1, y1, x2,
                 y2, area_w, area_h, (unsigned)stride, disp_w, disp_h);
    }

    int px0,py0,px1,py1,px2,py2;
    DisplayUiLogicalToPanel(x1,y1,&px0,&py0);
    DisplayUiLogicalToPanel(x1+1,y1,&px1,&py1);
    DisplayUiLogicalToPanel(x1,y1+1,&px2,&py2);
    EpdBlitI1(fb,bits,stride,area->x1,area->y1,x1,y1,x2,y2,fb_w,fb_h,
              px0,py0,px1-px0,py1-py0,px2-px0,py2-py0);
    ESP_LOGD(TAG, "blit area %d,%d-%d,%d cost=%lldms", x1, y1, x2, y2,
             (long long)((esp_timer_get_time() - t0) / 1000));
}

static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    if (s_freeze_updates.load(std::memory_order_relaxed)) {
        lv_display_flush_ready(disp);
        return;
    }
    uint8_t* fb = EpdRenderBuffer();
    if (fb == nullptr || px_map == nullptr || area == nullptr) {
        lv_display_flush_ready(disp);
        return;
    }

    const int disp_w = (int)lv_display_get_horizontal_resolution(disp);
    const int disp_h = (int)lv_display_get_vertical_resolution(disp);
    // Full refresh drives every pixel, but preserves the complete rendered target.
    // A partial invalidation must never erase the rest of the framebuffer.
    if(lv_display_get_color_format(disp)==LV_COLOR_FORMAT_I1)
        blit_i1_area_to_epdiy(px_map,area,fb,disp_w,disp_h);
    else blit_l8_area_to_epdiy(px_map,area,fb,disp_w,disp_h);

    if (lv_display_flush_is_last(disp)) {
        const int64_t t0 = esp_timer_get_time();
        const bool force = s_force_full.exchange(false);
        ESP_LOGD(TAG, "flush last area=%d,%d-%d,%d force=%d defer=%d", area->x1, area->y1, area->x2,
                 area->y2, force ? 1 : 0, s_defer_standby_commit.load() ? 1 : 0);
        // 待机或关机：只收集完整画面，由定格路径统一提交推白
        if (!s_defer_standby_commit.load(std::memory_order_relaxed)) {
            epd_commit_update(force);
        }
        StandbyScreen::NotifyFlushCompleted();
        ESP_LOGD(TAG, "commit cost=%lldms", (long long)((esp_timer_get_time() - t0) / 1000));
    }
    lv_display_flush_ready(disp);
}

bool LvAdapterEpdiy::Init() {
    s_instance = this;
    if (lvgl_ready_) {
        return true;
    }
    if (!MetalioEpd_Ready()) {
        ESP_LOGE(TAG, "epd not ready");
        return false;
    }

    if (s_lvgl_mux == nullptr) {
        s_lvgl_mux = xSemaphoreCreateRecursiveMutex();
    }

    if (!fontpack_lv_ensure_ready()) {
        ESP_LOGE(TAG, "fontpack init failed (flash use_font/font.fontpack to font_data)");
        return false;
    }

    width_ = DISPLAY_CONTENT_W;
    height_ = DISPLAY_CONTENT_H;

    if (!EpdRefreshInit()) {
        ESP_LOGE(TAG, "refresh worker/buffers unavailable");
        return false;
    }
    lv_init();
    if(!EpdContentBlendInit()) return false;
    lv_tick_set_cb(lv_tick_cb);
    s_disp = lv_display_create(width_, height_);
    lv_display_add_event_cb(s_disp, drawable_bounds_cb, LV_EVENT_SCREEN_LOAD_START, nullptr);
    lv_display_add_event_cb(s_disp, drawable_bounds_cb, LV_EVENT_RESOLUTION_CHANGED, nullptr);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_I1);
    DisplayUiBind(s_disp, epd_width(), epd_height());

    // A bounded stripe works in either orientation. LVGL renders larger dirty
    // areas in chunks; no full-screen shadow/coverage buffer is needed.
    constexpr uint32_t kDrawRows = 64;
    const uint32_t max_stride = lv_draw_buf_width_to_stride(
        std::max(DISPLAY_WIDTH, DISPLAY_HEIGHT), LV_COLOR_FORMAT_L8);
    const size_t binary_bytes=LV_COLOR_INDEXED_PALETTE_SIZE(LV_COLOR_FORMAT_I1)*sizeof(lv_color32_t)+
        std::max(size_t(lv_draw_buf_width_to_stride(DISPLAY_WIDTH,LV_COLOR_FORMAT_I1))*DISPLAY_HEIGHT,
                 size_t(lv_draw_buf_width_to_stride(DISPLAY_HEIGHT,LV_COLOR_FORMAT_I1))*DISPLAY_WIDTH);
    // Retain a full I1 frame for fast ordinary pages; gray reuses this allocation.
    const size_t buf_size=std::max<size_t>(binary_bytes,size_t(max_stride)*size_t(kDrawRows));
    uint8_t* buf1 = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf1 == nullptr) {
        buf1 = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (buf1 == nullptr) {
        ESP_LOGE(TAG, "no draw buffer %u", (unsigned)buf_size);
        return false;
    }
    s_draw_buffer=buf1; s_draw_buffer_bytes=buf_size;
    lv_display_set_buffers(s_disp, buf1, nullptr, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(s_disp,[](lv_event_t* event) {
        auto* display=static_cast<lv_display_t*>(lv_event_get_target(event));
        auto* root=lv_display_get_screen_active(display);
        if(!root) return;
        lv_obj_update_layout(root);
        lv_obj_update_layout(lv_display_get_layer_top(display));
        lv_obj_update_layout(lv_display_get_layer_sys(display));
        const auto format=EpdVisibleContentNeedsGray(display) ? LV_COLOR_FORMAT_L8 : LV_COLOR_FORMAT_I1;
        if(format==lv_display_get_color_format(display)) return;
        lv_display_set_color_format(display,format);
        lv_display_set_buffers(display,s_draw_buffer,nullptr,s_draw_buffer_bytes,LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_obj_invalidate(root);
        lv_obj_invalidate(lv_display_get_layer_top(display));
        lv_obj_invalidate(lv_display_get_layer_sys(display));
        ESP_LOGI(TAG,"content render format=%s",format==LV_COLOR_FORMAT_I1 ? "I1" : "L8");
    },LV_EVENT_REFR_START,nullptr);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, LvAdapterEpdiy_TouchReadCb);
    lv_indev_set_display(indev, s_disp);
    LvAdapterEpdiy_TouchBind();

    const mmap_assets_config_t mmap_cfg = {
        .partition_label = "resources",
        .max_files = MMAP_RESOURCES_FILES,
        .checksum = MMAP_RESOURCES_CHECKSUM,
        .flags = {.mmap_enable = true},
    };
    esp_err_t err = mmap_assets_new(&mmap_cfg, &s_resources);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmap_assets_new failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_lv_fs_handle_t fs_handle = nullptr;
    const fs_cfg_t fs_cfg = {
        .fs_letter = 'A',
        .fs_nums = MMAP_RESOURCES_FILES,
        .fs_assets = s_resources,
    };
    err = esp_lv_fs_desc_init(&fs_cfg, &fs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lv_fs_desc_init failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_lv_decoder_handle_t decoder = nullptr;
    err = esp_lv_decoder_init(&decoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lv_decoder_init failed: %s", esp_err_to_name(err));
        return false;
    }

    Lang::InitFromNvs();
    HomeScreen::LoadCardStyle();
    // 对齐 397：开机在 Init 线程预读 NVS，避免首次进阅读时 FontFile 未 ready 落默认字
    BookReaderPrefsEnsureLoaded();
    BookTapZonesEnsureLoaded();
    Wallpaper_HydrateFromNvsNow();
    reader::book_home_snapshot::HydrateFromNvs();
    ESP_LOGI(TAG, "SetupUI: create HomeScreen %dx%d (inset L%d R%d T%d B%d)", width_, height_,
             DISPLAY_CONTENT_LEFT_INSET, DISPLAY_CONTENT_RIGHT_INSET, DISPLAY_CONTENT_TOP_INSET,
             DISPLAY_CONTENT_BOTTOM_INSET);
    lv_screen_load(HomeScreen::Create());
    lv_refr_now(s_disp);

    {
        esp_timer_create_args_t notification_timer_args = {
            .callback =
                [](void* arg) {
                    auto* display = static_cast<LvAdapterEpdiy*>(arg);
                    DisplayLockGuard lock(display);
                    if (!lock) return;
                    if (display->notification_label_ != nullptr) {
                        lv_obj_add_flag(display->notification_label_, LV_OBJ_FLAG_HIDDEN);
                    }
                    if (display->status_label_ != nullptr && !AssistantScreen::IsPttWaveVisible() && !WorkBuddyScreen::IsPttWaveVisible()) {
                        lv_obj_remove_flag(display->status_label_, LV_OBJ_FLAG_HIDDEN);
                    }
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "epd_notif_timer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&notification_timer_args, &notification_timer_));
    }

    lvgl_ready_ = true;
    StartDisplayTickTaskOnce();
    ESP_LOGI(TAG, "LVGL home ready");
    return true;
}

static void PollBootKey() {
    static bool prev_down = false;
    static int64_t down_us = 0;
    static bool long_fired = false;
    const bool down = MetalioKeys_IsDown(0);
    const int64_t now = esp_timer_get_time();
    if (down && !prev_down) {
        down_us = now;
        long_fired = false;
        BootKey_OnPressDown();
        PowerPolicy::GetInstance().NotifyUserActivity();
    } else if (down && prev_down && !long_fired) {
        if ((now - down_us) / 1000 >= TOUCH_VK_LONG_MS) {
            long_fired = true;
            BootKey_OnLongPress();
        }
    } else if (!down && prev_down) {
        BootKey_OnPressUp();
        if (!long_fired) {
            BootKey_OnClick();
        }
    }
    prev_down = down;
}

static bool SideKeysBlocked() {
    return StandbyScreen::IsActive() || OtaUpgradeScreen::IsActive() || OtaConfirmDialog::IsActive();
}

static void ApplyVolumeDelta(int delta) {
    auto& board = Board::GetInstance();
    auto* codec = board.GetAudioCodec();
    auto* display = board.GetDisplay();
    if (codec == nullptr) {
        return;
    }
    int volume = codec->output_volume() + delta;
    if (volume > 100) {
        volume = 100;
    }
    if (volume < 0) {
        volume = 0;
    }
    codec->SetOutputVolume(volume);
    ESP_LOGI(TAG, "volume=%d (delta=%d)", volume, delta);
    if (display != nullptr) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s%d", Lang::Strings::VOLUME, volume);
        display->ShowNotification(buf);
    }
}

static void ApplyVolumeAbsolute(int volume, const char* note) {
    auto& board = Board::GetInstance();
    auto* codec = board.GetAudioCodec();
    auto* display = board.GetDisplay();
    if (codec == nullptr) {
        return;
    }
    if (volume > 100) {
        volume = 100;
    }
    if (volume < 0) {
        volume = 0;
    }
    codec->SetOutputVolume(volume);
    ESP_LOGI(TAG, "volume=%d (%s)", volume, note != nullptr ? note : "");
    if (display != nullptr && note != nullptr) {
        display->ShowNotification(note);
    }
}

// 音量键沿状态；BOOT 和 POWER 由各自的分发路径处理。
static bool s_side_prev_down[3] = {};
static int64_t s_side_down_us[3] = {};
static bool s_side_long_fired[3] = {};
static bool s_side_synced = false;

static void PollOneSideKey(int index, uint32_t long_ms, void (*on_click)(), void (*on_long)(),
                           void (*on_up)()) {
    if (index < 1 || index > 2) {
        return;
    }

    const bool down = MetalioKeys_IsDown(index);
    const int64_t now = esp_timer_get_time();
    if (down && !s_side_prev_down[index]) {
        s_side_down_us[index] = now;
        s_side_long_fired[index] = false;
        PowerPolicy::GetInstance().NotifyUserActivity();
    } else if (down && s_side_prev_down[index] && !s_side_long_fired[index]) {
        if ((now - s_side_down_us[index]) / 1000 >= static_cast<int64_t>(long_ms)) {
            s_side_long_fired[index] = true;
            if (!SideKeysBlocked() && on_long != nullptr) {
                on_long();
            }
        }
    } else if (!down && s_side_prev_down[index]) {
        if (!s_side_long_fired[index] && !SideKeysBlocked() && on_click != nullptr) {
            on_click();
        }
        // 阅读连翻：长按后松手须停定时器（短按无 repeat 时 OnPressUp 为空操作）
        if (!SideKeysBlocked() && on_up != nullptr) {
            on_up();
        }
    }
    s_side_prev_down[index] = down;
}

// 正文沉浸：音量+=上一页、音量-=下一页（含长按连翻）；目录/设置浮层及其余页仍调音量
static void OnVolUpClick() {
    if (BookScreen::IsReadingActive()) {
        VkKey_Dispatch("vk_prev");
        return;
    }
    ApplyVolumeDelta(10);
}
static void OnVolUpLong() {
    if (BookScreen::IsReadingActive()) {
        VkKey_OnLongPress("vk_prev");
        return;
    }
    ApplyVolumeAbsolute(100, Lang::Strings::MAX_VOLUME);
}
static void OnVolUpUp() {
    if (BookScreen::IsReadingActive()) {
        VkKey_OnPressUp("vk_prev");
    }
}
static void OnVolDownClick() {
    if (BookScreen::IsReadingActive()) {
        VkKey_Dispatch("vk_next");
        return;
    }
    ApplyVolumeDelta(-10);
}
static void OnVolDownLong() {
    if (BookScreen::IsReadingActive()) {
        VkKey_OnLongPress("vk_next");
        return;
    }
    ApplyVolumeAbsolute(0, Lang::Strings::MUTED);
}
static void OnVolDownUp() {
    if (BookScreen::IsReadingActive()) {
        VkKey_OnPressUp("vk_next");
    }
}
static void OnPowerClick() {
    PowerKey_OnClick();
}

static void PollSideKeys() {
    // 浅睡 mask 期间 IsDown 恒 false；勿把「假松开」合成音量 click
    if (!MetalioKeys_IsReady()) {
        s_side_synced = false;
        return;
    }
    if (!s_side_synced) {
        // 恢复时吞掉仍按住的音量键手势，避免产生假单击或长按。
        const int64_t now = esp_timer_get_time();
        for (int i = 1; i <= 2; ++i) {
            const bool down = MetalioKeys_IsDown(i);
            s_side_prev_down[i] = down;
            s_side_long_fired[i] = down;
            s_side_down_us[i] = down ? now : 0;
        }
        s_side_synced = true;
        return;
    }
    PollOneSideKey(1, TOUCH_VK_LONG_MS, OnVolUpClick, OnVolUpLong, OnVolUpUp);
    PollOneSideKey(2, TOUCH_VK_LONG_MS, OnVolDownClick, OnVolDownLong, OnVolDownUp);
    // POWER clicks are latched by the sampling task. During standby the LP
    // task is the sole consumer, including the interval blocked by Park.
    if (!StandbyScreen::IsActive() && MetalioKeys_TakePowerClick()) {
        OnPowerClick();
    }
}

void LvAdapterEpdiy::Tick() {
    if (!lvgl_ready_) {
        return;
    }
    if (Lock(50)) {
        const int64_t round_start = esp_timer_get_time();
        VkKey_DrainTouch();
        PollBootKey();
        PollSideKeys();
        while (MetalioKeys_TakeEdge()) {}
        PollReaderPageWork();
        ScreenLvAsyncDrain();
        // OpenDone 无锁投递：先于 timer_handler，避免整屏绘制占锁时回调永远进不来
        BookDrainPendingOpenDone();
        BookScreen::PollStandbyWork();
        StandbyWallpaper_Poll();
        if (StandbyScreen::IsActive() && !s_standby_loading_notice_shown &&
            StandbyWallpaper_IsLoadingSlow()) {
            s_standby_loading_notice_shown = true;
            ShowWallpaperLoading();
        }
        const int64_t t0 = esp_timer_get_time();
        lv_timer_handler();
        const int64_t dt = esp_timer_get_time() - t0;
        const int64_t round_us = esp_timer_get_time() - round_start;
        Unlock();
        if (round_us > 200 * 1000) {
            static int64_t last_log_us = 0;
            static unsigned suppressed = 0;
            const auto now = esp_timer_get_time();
            if (last_log_us == 0 || now - last_log_us >= 5000000) {
                ESP_LOGW(TAG, "UI round slow total=%lldms timers=%lldms suppressed=%u",
                    static_cast<long long>(round_us / 1000), static_cast<long long>(dt / 1000), suppressed);
                last_log_us = now; suppressed = 0;
            } else ++suppressed;
        }
    }
}

void LvAdapterEpdiy::SetSystemReady() {
    SetStatus("");
}

void LvAdapterEpdiy::RequestFullOnNextCommit() {
    s_force_full.store(true);
}

void LvAdapterEpdiy::FullRefresh() {
    if (!lvgl_ready_ || s_disp == nullptr) {
        return;
    }
    s_stream_paint.store(false);
    s_force_full.store(true);
    if (Lock(1000)) {
        lv_obj_t* scr = lv_screen_active();
        if (scr != nullptr) lv_obj_invalidate(scr);
        lv_refr_now(s_disp);
        Unlock();
    }
}

void LvAdapterEpdiy::SetStreamPaintMode(bool on) {
    const bool was_on=s_stream_paint.exchange(on);
    if (was_on && !on) EpdRequestScreenChange();
}

void LvAdapterEpdiy::FreezeFlush(bool on) {
    s_freeze_updates.store(on, std::memory_order_relaxed);
}

static void ShowWallpaperLoading() {
    lv_obj_t* notice = lv_label_create(lv_layer_top());
    lv_obj_remove_style_all(notice);
    lv_label_set_text(notice, Lang::Strings::WALLPAPER_ENTER_LOADING);
    lv_label_set_long_mode(notice, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(notice, LV_HOR_RES - 32, 0);
    lv_obj_set_style_text_font(notice, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(notice, lv_color_white(), 0);
    lv_obj_set_style_bg_color(notice, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(notice, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(notice, 8, 0);
    lv_obj_set_style_pad_hor(notice, 12, 0);
    lv_obj_set_style_pad_ver(notice, 8, 0);
    lv_obj_clear_flag(notice, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(notice, LV_ALIGN_TOP_MID, 0, ScreenStatusBarHeight() + 8);
    lv_obj_update_layout(notice);
    lv_area_t clip;
    lv_obj_get_coords(notice, &clip);
    // 仅将提示区域叠加到原有帧缓冲，保留已定格壁纸等未挂入 LVGL 的画面。
    const bool was_frozen = s_freeze_updates.exchange(false);
    s_wallpaper_notice_clip = &clip;
    lv_refr_now(s_disp);
    s_wallpaper_notice_clip = nullptr;
    s_freeze_updates.store(was_frozen);
    if (!EpdWait(EpdSubmit(false))) {
        ESP_LOGW(TAG, "wallpaper loading notice failed");
    }
    lv_obj_delete(notice);
}

void LvAdapterEpdiy::BeginStandbyEnterPaint() {
    EpdSetTestActive(false);
    EpdCancelPageTurn();
    s_restore_frontlight_after_commit.store(false);
    s_stream_paint.store(false);
    s_defer_standby_commit.store(true, std::memory_order_relaxed);
    s_force_full.store(false);
    s_standby_loading_notice_shown = false;
    FreezeFlush(Wallpaper_HasStandbySelection());
    ESP_LOGI(TAG, "standby enter: defer panel commit until Park");
}

bool LvAdapterEpdiy::ParkEpdForStandby() {
    DisplayLockGuard lock(this);
    if (!lock) return false;
    if (Wallpaper_HasStandbySelection() && StandbyWallpaper_DecodedImage() == nullptr) {
        // 解码失败时恢复底层页面，移除已落墨的加载提示。
        s_defer_standby_commit.store(true, std::memory_order_relaxed);
        FreezeFlush(false);
        if (lv_screen_active() != nullptr) lv_obj_invalidate(lv_screen_active());
        lv_refr_now(s_disp);
    }
    // 经典页已完成 flush；壁纸直接写入同一缓冲，均只提交一次。
    FreezeFlush(true);
    s_defer_standby_commit.store(false, std::memory_order_relaxed);
    s_stream_paint.store(false);
    s_force_full.store(false);
    if (const auto* wallpaper = StandbyWallpaper_DecodedImage()) {
        DisplayUiSetFullScreen(true);
        DisplayUiSetOrient(kDisplayUiPortrait);
        if (!BlitL8ToEpdFullScreen(*wallpaper)) return false;
    }
    EpdRequestScreenChange(true);
    const uint32_t sequence = EpdSubmit(false);
    if (!EpdWait(sequence)) {
        ESP_LOGE(TAG, "standby frame failed; stay awake");
        return false;
    }
    Frontlight::GetInstance().ForceAllLow();
    ESP_LOGI(TAG, "standby: frame parked, flush frozen");
    return true;
}

void LvAdapterEpdiy::WakeEpdFromStandby() {
    // Wake UI refreshes before the EPD commit that turns the light back on.
    // Populate its target now so sliders never render the standby-only zero.
    Frontlight::GetInstance().PrepareBrightnessRestore();
    s_defer_standby_commit.store(false, std::memory_order_relaxed);
    FreezeFlush(false);
    SetStreamPaintMode(false);
    s_force_full.store(false);
    EpdRequestScreenChange(true);
    if (lv_screen_active() != nullptr) {
        lv_obj_invalidate(lv_screen_active());
    }
}

void LvAdapterEpdiy::RequestRestoreFrontlightAfterCommit() {
    s_restore_frontlight_after_commit.store(true, std::memory_order_relaxed);
}

void LvAdapterEpdiy::PauseStandbyBackgroundWork() {
    metalio_touch_set_feed_paused(true);
}

void LvAdapterEpdiy::ResumeStandbyBackgroundWork() {
    metalio_touch_set_feed_paused(false);
}

namespace {

// L8 背景在完整面板区域等比居中，不使用普通 UI 内缩。
bool BlitL8ToEpdFullScreen(const reader::RasterImage& img) {
    uint8_t* fb = EpdRenderBuffer();
    if (fb == nullptr || img.empty()) {
        return false;
    }
    memset(fb, 0xFF, static_cast<size_t>(epd_width()/2)*epd_height());
    const int max_w = DISPLAY_WIDTH;
    const int max_h = DISPLAY_HEIGHT;
    const double scale = std::min({1.0, double(max_w) / img.width, double(max_h) / img.height});
    const int draw_w = std::max(1, int(img.width * scale));
    const int draw_h = std::max(1, int(img.height * scale));
    const int left = (max_w - draw_w) / 2;
    const int top = (max_h - draw_h) / 2;
    ESP_LOGI(TAG, "wallpaper framebuffer: panel=%dx%d image=%ux%u draw=%dx%d at=(%d,%d)",
             max_w, max_h, static_cast<unsigned>(img.width), static_cast<unsigned>(img.height),
             draw_w, draw_h, left, top);
    const int fb_w = epd_width();
    const int fb_h = epd_height();
    for (int ly = 0; ly < draw_h; ++ly) {
        const int px = ly + top;
        if (px < 0 || px >= fb_w) {
            continue;
        }
        const uint8_t* row = img.pixels.data() + static_cast<size_t>(ly * img.height / draw_h) * img.width;
        for (int lx = 0; lx < draw_w; ++lx) {
            const int py = EPD_LOGICAL_W - 1 - (lx + left);
            if (py < 0 || py >= fb_h) {
                continue;
            }
            FbPutNative(fb, px, py, reader::DarkenImagePixel(row[lx * img.width / draw_w]), fb_w);
        }
        if ((ly & 63) == 0) {
            taskYIELD();
        }
    }
    return true;
}

bool TryDecodeShutdownFromSd(reader::RasterImage& out) {
    // 待机保持 SD 访问暂停，仅允许当前任务读取关机壁纸。
    sd_access::Permit permit;
    char path[SD_FILE_PATH_CAPACITY] = {};
    if (!Wallpaper_TryResolveActiveWallpaperPath(path, sizeof(path))) {
        ESP_LOGI(TAG, "shutdown img: no NVS shutdown file");
        return false;
    }
    if (!reader::DecodeImageFileToL8(path, 0, 0, out) ||
        out.empty()) {
        ESP_LOGW(TAG, "shutdown img: SD decode fail %s", path);
        return false;
    }
    ESP_LOGI(TAG, "shutdown img: SD %s %dx%d", path, out.width, out.height);
    return true;
}

bool TryDecodeShutdownBuiltin(reader::RasterImage& out) {
    // 中英文默认图随应用固件更新，不依赖 resources 分区升级。
    extern const uint8_t zh_start[] asm("_binary_bg_shutdown_zh_a2i1_start");
    extern const uint8_t zh_end[] asm("_binary_bg_shutdown_zh_a2i1_end");
    extern const uint8_t en_start[] asm("_binary_bg_shutdown_en_a2i1_start");
    extern const uint8_t en_end[] asm("_binary_bg_shutdown_en_a2i1_end");
    const bool is_zh = std::strcmp(Lang::CODE, "zh-CN") == 0;
    const uint8_t* start = is_zh ? zh_start : en_start;
    const uint8_t* end = is_zh ? zh_end : en_end;
    if (reader::DecodeImageToL8(start, static_cast<size_t>(end - start),
                              0, 0, out) && !out.empty()) {
        ESP_LOGI(TAG, "shutdown img: embedded %s %dx%d", Lang::CODE, out.width, out.height);
        return true;
    }
    ESP_LOGW(TAG, "shutdown img: embedded %s decode fail", Lang::CODE);
    if (s_resources == nullptr) {
        ESP_LOGW(TAG, "shutdown img: built-in missing (no mmap)");
        return false;
    }
    const uint8_t* mem = mmap_assets_get_mem(s_resources, MMAP_RESOURCES_BG_SHUTDOWN_A2I1);
    const int sz = mmap_assets_get_size(s_resources, MMAP_RESOURCES_BG_SHUTDOWN_A2I1);
    if (mem == nullptr || sz <= 0) {
        ESP_LOGW(TAG, "shutdown img: built-in missing mem=%p sz=%d", mem, sz);
        return false;
    }
    const uint8_t* p = mem;
    size_t n = static_cast<size_t>(sz);
    // mmap 可能带 5A5A 前缀
    if (n >= 6 && p[0] == 0x5A && p[1] == 0x5A && p[2] == 'A' && p[3] == '2' && p[4] == 'I' &&
        p[5] == '1') {
        p += 2;
        n -= 2;
    }
    if (!reader::DecodeImageToL8(p, n, 0, 0, out) || out.empty()) {
        ESP_LOGW(TAG, "shutdown img: built-in decode fail");
        return false;
    }
    ESP_LOGI(TAG, "shutdown img: built-in %dx%d (%u bytes)", out.width, out.height,
             static_cast<unsigned>(sz));
    return true;
}

void ShowPoweredOffTextFallback() {
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* lbl = lv_label_create(scr);
    lv_label_set_text(lbl, Lang::Strings::POWERED_OFF);
    const lv_font_t* font = fontpack_lv_font_get(30, 2);
    if (font == nullptr) {
        font = fontpack_lv_font_ui();
    }
    if (font != nullptr) {
        lv_obj_set_style_text_font(lbl, font, 0);
    }
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_t* old_scr = lv_screen_active();
    lv_screen_load(scr);
    if (old_scr != nullptr && old_scr != scr) {
        lv_obj_delete(old_scr);
    }
    lv_obj_invalidate(scr);
    lv_refr_now(nullptr);
}

struct ShutdownWallpaperWork {
    reader::RasterImage img; // 当前关机流程持有的解码像素
    bool have_img = false; // 解码成功后由 done 发布
    std::atomic<bool> done{false}; // 解码任务完成标记
};

void DecodeShutdownWallpaperTask(void* arg) {
    auto* work = static_cast<ShutdownWallpaperWork*>(arg);
    work->have_img = TryDecodeShutdownFromSd(work->img);
    if (!work->have_img) {
        work->have_img = TryDecodeShutdownBuiltin(work->img);
    }
    work->done.store(true, std::memory_order_release);
    vTaskDeleteWithCaps(nullptr);
}

}  // namespace

bool LvAdapterEpdiy::ShowPoweredOffScreen() {
    DisplayLockGuard lock(this);
    if (!lock) return false;
    // 加载期间保留原帧，提示局部叠加；解码完成后再生成完整关机画面。
    FreezeFlush(true);
    s_defer_standby_commit.store(true, std::memory_order_relaxed);
    s_stream_paint.store(false);

    s_force_full.store(false);
    s_restore_frontlight_after_commit.store(false);
    EpdSetTestActive(false);
    EpdCancelPageTurn();
    ShutdownWallpaperWork work;
    const int64_t started = esp_timer_get_time();
    if (xTaskCreatePinnedToCoreWithCaps(DecodeShutdownWallpaperTask, "off_wp_load", 12 * 1024,
                                      &work, 5, nullptr, 0,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
        bool notice_shown = false;
        while (!work.done.load(std::memory_order_acquire)) {
            if (!notice_shown && esp_timer_get_time() - started >= 1000000) {
                notice_shown = true;
                ShowWallpaperLoading();
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    } else {
        ESP_LOGW(TAG, "shutdown wallpaper task create failed; use built-in image");
        work.have_img = TryDecodeShutdownBuiltin(work.img);
    }
    bool have_img = work.have_img;

    DisplayUiSetFullScreen(true);
    DisplayUiSetOrient(kDisplayUiPortrait);
    BindStatusWidgets(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);

    if (have_img) have_img = BlitL8ToEpdFullScreen(work.img);

    if (!have_img) {
        FreezeFlush(false);
        ShowPoweredOffTextFallback();
    }

    FreezeFlush(true);
    s_defer_standby_commit.store(false, std::memory_order_relaxed);
    s_force_full.store(false);
    EpdRequestScreenChange(true);
    if (!EpdWait(EpdSubmit(false))) {
        ESP_LOGE(TAG, "shutdown frame failed; keep power on");
        return false;
    }
    ESP_LOGI(TAG, "powered-off screen shown (a2i1=%d)", have_img ? 1 : 0);
    return true;
}

void LvAdapterEpdiy::SetStatusTitlePrefix(const char* prefix) {
    DisplayLockGuard lock(this);
    if (!lock) return;
    if (prefix == nullptr || prefix[0] == '\0') {
        status_title_prefix_[0] = '\0';
    } else {
        std::snprintf(status_title_prefix_, sizeof(status_title_prefix_), "%s", prefix);
    }
}

bool LvAdapterEpdiy::TryGetResource(const char* name, const uint8_t** mem, size_t* size) const {
    if (name == nullptr || name[0] == '\0' || mem == nullptr || size == nullptr) {
        return false;
    }
    *mem = nullptr;
    *size = 0;
    if (s_resources == nullptr) {
        return false;
    }
    const int n = mmap_assets_get_stored_files(s_resources);
    for (int i = 0; i < n; ++i) {
        const char* nm = mmap_assets_get_name(s_resources, i);
        if (nm == nullptr || std::strcmp(nm, name) != 0) {
            continue;
        }
        const uint8_t* p = mmap_assets_get_mem(s_resources, i);
        const int sz = mmap_assets_get_size(s_resources, i);
        if (p == nullptr || sz <= 0) {
            return false;
        }
        *mem = p;
        *size = static_cast<size_t>(sz);
        return true;
    }
    return false;
}

void LvAdapterEpdiy::ApplyStatusTextLocked(const char* status) {
    const char* body = (status != nullptr) ? status : "";
    if (std::strcmp(status_body_cache_, body) != 0) {
        std::snprintf(status_body_cache_, sizeof(status_body_cache_), "%s", body);
    }
    if (status_label_ == nullptr) {
        return;
    }
    char buf[96];
    const char* want = status_body_cache_;
    if (status_title_prefix_[0] != '\0') {
        std::snprintf(buf, sizeof(buf), "%s·%s", status_title_prefix_, status_body_cache_);
        want = buf;
    }
    // 文案未变勿 lv_label_set_text：否则 CLOCK_TICK 每秒脏刷顶栏
    const char* cur = lv_label_get_text(status_label_);
    if (cur != nullptr && std::strcmp(cur, want) == 0) {
        return;
    }
    lv_label_set_text(status_label_, want);
}

void LvAdapterEpdiy::RestoreStatusWidgetsLocked() {
    // 切页只换控件指针；图标/电量%/静音/时钟缓存跨页保留，首帧与页面内容同次画出（对齐 397）
    if (network_label_ != nullptr) {
        lv_label_set_text(network_label_, network_icon_ != nullptr ? network_icon_ : "");
    }
    if (battery_label_ != nullptr) {
        lv_label_set_text(battery_label_, battery_icon_ != nullptr ? battery_icon_ : "");
    }
    if (battery_pct_label_ != nullptr) {
        lv_label_set_text(battery_pct_label_, battery_pct_cache_);
    }
    if (mute_label_ != nullptr) {
        lv_label_set_text(mute_label_, muted_ ? FONT_AWESOME_VOLUME_XMARK : "");
    }
    if (notification_label_ != nullptr && !lv_obj_has_flag(notification_label_, LV_OBJ_FLAG_HIDDEN)) {
        if (status_label_ != nullptr) {
            lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (status_label_ != nullptr) {
        const char* body = status_body_cache_[0] != '\0' ? status_body_cache_ : "--:--";
        ApplyStatusTextLocked(body);
        if (!AssistantScreen::IsPttWaveVisible() && !WorkBuddyScreen::IsPttWaveVisible()) {
            lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

EpdStatusBar LvAdapterEpdiy::GetBoundStatusBar() const {
    EpdStatusBar status;
    status.status_label = status_label_;
    status.notification_label = notification_label_;
    status.network_label = network_label_;
    status.battery_label = battery_label_;
    status.battery_pct_label = battery_pct_label_;
    status.mute_label = mute_label_;
    return status;
}

void LvAdapterEpdiy::BindStatusBar(lv_obj_t* status_label, lv_obj_t* notification_label,
                                   lv_obj_t* network_label, lv_obj_t* battery_label,
                                   lv_obj_t* battery_pct_label, lv_obj_t* mute_label) {
    status_label_ = status_label;
    notification_label_ = notification_label;
    network_label_ = network_label;
    battery_label_ = battery_label;
    battery_pct_label_ = battery_pct_label;
    mute_label_ = mute_label;
    RestoreStatusWidgetsLocked();
}

bool LvAdapterEpdiy::Lock(int timeout_ms) {
    if (s_lvgl_mux == nullptr) {
        return true;
    }
    TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lvgl_mux, ticks) == pdTRUE;
}

void LvAdapterEpdiy::Unlock() {
    if (s_lvgl_mux != nullptr) {
        xSemaphoreGiveRecursive(s_lvgl_mux);
    }
}

void LvAdapterEpdiy::SetChatMessage(const char* role, const char* content) {
    if (!AssistantScreen::IsActive()) {
        return;
    }
    AssistantScreen::AddMessage(role, content);
}

void LvAdapterEpdiy::SetEmotion(const char* emotion) {
    if (!AssistantScreen::IsActive()) {
        return;
    }
    DisplayLockGuard lock(this);
    if (!lock) return;
    AssistantScreen::SetEmotion(emotion);
}

void LvAdapterEpdiy::SetStatus(const char* status) {
    // 百问页：屏蔽聆听/说话文案（PTT 波形占顶栏）；Idle 须放行时钟（ApplyIdleStatusBar）
    if (AssistantScreen::IsActive()) {
        const DeviceState state = Application::GetInstance().GetDeviceState();
        if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
            return;
        }
    }
    DisplayLockGuard lock(this);
    if (!lock) return;
    if (status_label_ == nullptr) {
        return;
    }
    if (network_busy_status_) {
        std::snprintf(status_before_network_, sizeof(status_before_network_), "%s", status ? status : "");
    }
    ApplyStatusTextLocked(status);
    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    if (notification_label_ != nullptr) {
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    }
    last_status_update_time_ = std::chrono::system_clock::now();
}

void LvAdapterEpdiy::ShowNotification(const char* notification, int duration_ms) {
    if (duration_ms < 0) {
        duration_ms = 0;
    }
    DisplayLockGuard lock(this);
    if (!lock) return;
    if (notification_label_ == nullptr || notification == nullptr) {
        return;
    }
    lv_label_set_text(notification_label_, notification);
    lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    if (status_label_ != nullptr) {
        lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (notification_timer_ != nullptr) {
        esp_timer_stop(notification_timer_);
        if (duration_ms > 0) {
            ESP_ERROR_CHECK(esp_timer_start_once(notification_timer_,
                                                 static_cast<uint64_t>(duration_ms) * 1000ULL));
        }
    }
}

void LvAdapterEpdiy::UpdateStatusBar(bool update_all) {
    // Background boot/network callbacks must not lazily initialize I2S while
    // standby owns and may power down the peripherals.
    if (StandbyScreen::IsActive()) return;
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();

    char time_str[16] = {};
    bool set_time = false;
    const char* wifi_busy = board.GetNetworkBusyStatusText();
    const bool assistant_page = AssistantScreen::IsActive();
    const auto device_state = app.GetDeviceState();
    // 百问联网/启动中：顶栏保留「连接中…」等，勿用时钟盖住
    const bool assistant_net_hint =
        assistant_page && (device_state == kDeviceStateConnecting ||
                           device_state == kDeviceStateStarting ||
                           device_state == kDeviceStateWifiConfiguring ||
                           device_state == kDeviceStateActivating);
    const bool want_clock = wifi_busy == nullptr && AllowsIdleStatusClock() && !assistant_net_hint &&
                            (assistant_page ||
                             (!app.HasPendingActivationCode() && !app.IsXiaozhiVoiceActive() &&
                              device_state == kDeviceStateIdle));
    if (want_clock &&
        (update_all || last_status_update_time_ + std::chrono::seconds(10) <
                            std::chrono::system_clock::now())) {
        time_t now = time(nullptr);
        struct tm tm_info = {};
        if (localtime_r(&now, &tm_info) != nullptr && tm_info.tm_year >= (2025 - 1900)) {
            strftime(time_str, sizeof(time_str), "%H:%M", &tm_info);
            set_time = true;
        } else if (update_all || assistant_page) {
            // 无可信 RTC 时仍清掉「连接中…」，显示占位
            std::snprintf(time_str, sizeof(time_str), "%s", "--:--");
            set_time = true;
        }
    }

    DisplayLockGuard lock(this);
    if (!lock) return;
    if (wifi_busy == nullptr && network_busy_status_) {
        network_busy_status_ = false;
        ApplyStatusTextLocked(status_before_network_);
    }
    if (wifi_busy != nullptr && status_label_ != nullptr) {
        if (!network_busy_status_) {
            std::snprintf(status_before_network_, sizeof(status_before_network_), "%s", status_body_cache_);
            network_busy_status_ = true;
        }
        ApplyStatusTextLocked(wifi_busy);
        if (!AssistantScreen::IsPttWaveVisible() && !WorkBuddyScreen::IsPttWaveVisible()) {
            if (lv_obj_has_flag(status_label_, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
            }
            if (notification_label_ != nullptr &&
                !lv_obj_has_flag(notification_label_, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else if (set_time && status_label_ != nullptr) {
        ApplyStatusTextLocked(time_str);
        if (!AssistantScreen::IsPttWaveVisible() && !WorkBuddyScreen::IsPttWaveVisible()) {
            const bool notif_on = notification_label_ != nullptr &&
                                  !lv_obj_has_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
            if (!notif_on) {
                if (lv_obj_has_flag(status_label_, LV_OBJ_FLAG_HIDDEN)) {
                    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
                }
                if (notification_label_ != nullptr &&
                    !lv_obj_has_flag(notification_label_, LV_OBJ_FLAG_HIDDEN)) {
                    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
        last_status_update_time_ = std::chrono::system_clock::now();
    }

    if (network_label_ != nullptr) {
        const char* neu = board.GetNetworkStateIcon();
        if (neu != network_icon_) {
            network_icon_ = neu;
            lv_label_set_text(network_label_, neu != nullptr ? neu : "");
        }
    }
    int level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        const char* icon = FONT_AWESOME_BATTERY_FULL;
        if (charging) {
            icon = FONT_AWESOME_BATTERY_BOLT;
        } else if (level <= 10) {
            icon = FONT_AWESOME_BATTERY_EMPTY;
        } else if (level <= 30) {
            icon = FONT_AWESOME_BATTERY_QUARTER;
        } else if (level <= 60) {
            icon = FONT_AWESOME_BATTERY_HALF;
        } else if (level <= 85) {
            icon = FONT_AWESOME_BATTERY_THREE_QUARTERS;
        }
        if (icon != battery_icon_) {
            battery_icon_ = icon;
            if (battery_label_ != nullptr) {
                lv_label_set_text(battery_label_, icon);
            }
        }
        if (level != battery_level_cache_) {
            battery_level_cache_ = level;
            std::snprintf(battery_pct_cache_, sizeof(battery_pct_cache_), "%d%%", level);
            if (battery_pct_label_ != nullptr) {
                lv_label_set_text(battery_pct_label_, battery_pct_cache_);
            }
        }
    } else {
        battery_level_cache_ = -1;
        battery_icon_ = nullptr;
        battery_pct_cache_[0] = '\0';
        if (battery_label_ != nullptr) {
            lv_label_set_text(battery_label_, "");
        }
        if (battery_pct_label_ != nullptr) {
            lv_label_set_text(battery_pct_label_, "");
        }
    }
    if (mute_label_ != nullptr) {
        auto* codec = board.GetAudioCodec();
        const bool muted = (codec != nullptr && codec->output_volume() == 0);
        if (muted != muted_) {
            muted_ = muted;
            lv_label_set_text(mute_label_, muted_ ? FONT_AWESOME_VOLUME_XMARK : "");
        }
    }
}
