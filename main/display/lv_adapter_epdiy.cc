/**
 * @file lv_adapter_epdiy.cc
 * @brief LVGL9 I1 真刷 → EPDiy；开机加载 HomeScreen
 */

#include "lv_adapter_epdiy.h"

#include "lv_adapter_epdiy_touch.h"

#include "application.h"
#include "assets/lang_config.h"
#include "assistant_screen/assistant_screen.h"
#include "audio_codec.h"
#include "board.h"
#include "boot_key_handler.h"
#include "config.h"
#include "device_state.h"
#include "display_orient.h"
#include "epd_board_metalio_eink4_plus.h"
#include "epd_font24.h" // EPD_LOGICAL_W/H、epd_logical_to_physical
#include "epd_highlevel.h"
#include "epdiy.h"
#include "fontpack_lvgl.h"
#include "frontlight.h"
#include "home_screen/home_screen.h"
#include "metalio_epd.h"
#include "metalio_keys.h"
#include "metalio_touch.h"
#include "ota_confirm_dialog/ota_confirm_dialog.h"
#include "ota_upgrade_screen/ota_upgrade_screen.h"
#include "power_hw.h"
#include "power_policy.h"
#include "screen_common.h"
#include "standby_screen/standby_screen.h"
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
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <mmap_generate_resources.h>
#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <font_awesome.h>
#include <vector>

#define TAG "lv_epdiy"

static constexpr int kPartialBeforeFull = 5; // 局刷累计后 GC16 清残影（原 10 偏疏）
static constexpr int kEpdTempC = 25;
static int s_partial_count = 0;
static std::atomic<bool> s_force_full{false};
static std::atomic<bool> s_stream_paint{false};
static std::atomic<bool> s_freeze_updates{false};
static std::atomic<bool> s_defer_standby_commit{false}; // 进待机先攒 FB，Park 再一次全刷上屏
static std::atomic<bool> s_restore_frontlight_after_commit{false}; // 退出待机：底层上墨后再开前光
static bool s_fb_white_for_force = false;
static SemaphoreHandle_t s_lvgl_mux = nullptr;

static mmap_assets_handle_t s_resources = nullptr;
static lv_display_t* s_disp = nullptr;
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
            if (MetalioKeys_TakeShutdownPulse()) {
                // 与电源长按策略同路：先刷关机图再脉冲（幂等）
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
    if (px & 1) {
        *buf_ptr = static_cast<uint8_t>((*buf_ptr & 0x0F) | (color & 0xF0));
    } else {
        *buf_ptr = static_cast<uint8_t>((*buf_ptr & 0xF0) | (color >> 4));
    }
}

// 统一落墨闸门：busy 供 WiFi 等 defer；硬冲突在此 pause；刷前锁满频
static void epd_panel_enter() {
    Display::SetPanelBusy(true);
    power_hw_cpu_freq_set(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    // tip/add76573：落墨期间勿并发采触摸（I2C1 与 TCA/TPS 争用会出假点）
    metalio_touch_set_feed_paused(true);
}

static void epd_panel_exit() {
    metalio_touch_set_feed_paused(false);
    Display::SetPanelBusy(false);
}

static void epd_commit_update(EpdiyHighlevelState* hl, bool force_full) {
    if (hl == nullptr) {
        return;
    }
    // 流式对话：只走 DU，避免每 10 帧 GC16 全刷风暴
    const bool stream = s_stream_paint.load();
    const bool full = force_full || (!stream && s_partial_count >= kPartialBeforeFull);
    const int64_t t0 = esp_timer_get_time();
    epd_panel_enter();
    epd_poweron();
    taskYIELD();
    if (full) {
        epd_hl_update_screen_from_white(hl, MODE_GC16, kEpdTempC);
        s_partial_count = 0;
    } else {
        epd_hl_update_screen(hl, MODE_GL16, kEpdTempC);
        if (!stream) {
            s_partial_count += 1;
        }
    }
    epd_poweroff();
    epd_panel_exit();
    if (full) {
        ESP_LOGI(TAG, "commit GC16 partial=%d/%d stream=%d cost=%lldms", s_partial_count,
                 kPartialBeforeFull, stream ? 1 : 0,
                 (long long)((esp_timer_get_time() - t0) / 1000));
    } else {
        ESP_LOGI(TAG, "commit GL16 partial=%d/%d stream=%d cost=%lldms", s_partial_count,
                 kPartialBeforeFull, stream ? 1 : 0,
                 (long long)((esp_timer_get_time() - t0) / 1000));
    }
}

static uint32_t lv_tick_cb(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/**
 * LVGL I1（1=白）→ EPDiy FB；逻辑竖屏 + 内容区 inset。
 *
 * PARTIAL：px_map 仅含本脏区，buf 原点=(area.x1,area.y1)，stride 按 area 宽；
 * 误用全屏绝对 y / disp_w stride 会在中部局刷时拉成横线，底边脏区写成黑带。
 */
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
    if (x2 < x1 || y2 < y1) {
        return;
    }
    const int area_w = x2 - x1 + 1;
    const int area_h = y2 - y1 + 1;
    const uint32_t pal_bytes =
        LV_COLOR_INDEXED_PALETTE_SIZE(LV_COLOR_FORMAT_I1) * (uint32_t)sizeof(lv_color32_t);
    // PARTIAL reshape：stride 对应该脏区宽度，不是全屏 disp_w
    const uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)area_w, LV_COLOR_FORMAT_I1);
    const uint8_t* bits = px_map + pal_bytes;
    const int left = DISPLAY_CONTENT_LEFT_INSET;
    const int top = DISPLAY_CONTENT_TOP_INSET;
    const int fb_w = epd_width();
    const int fb_h = epd_height();
    const int64_t t0 = esp_timer_get_time();

    // 底边/非整屏局刷：打一次关键日志，便于对照「横线 / 底黑条」
    if (y1 > 0 || y2 < disp_h - 1 || x1 > 0 || x2 < disp_w - 1) {
        ESP_LOGI(TAG, "blit partial area=%d,%d-%d,%d aw=%d ah=%d stride=%u disp=%dx%d", x1, y1, x2,
                 y2, area_w, area_h, (unsigned)stride, disp_w, disp_h);
    }

    for (int ly = y1; ly <= y2; ++ly) {
        const uint8_t* row = bits + (size_t)(ly - y1) * stride;
        // logical→physical: xP=yL+top（落在 native 宽 1216，上下留白）
        const int px = ly + top;
        if (px < 0 || px >= fb_w) {
            continue;
        }
        for (int lx = x1; lx <= x2; ++lx) {
            const int bx = lx - x1; // 行内相对列
            const uint8_t byte = row[bx >> 3];
            const uint8_t bit = (byte >> (7 - (bx & 7))) & 1;
            const uint8_t color = bit ? 0xFF : 0x00;
            const int py = EPD_LOGICAL_W - 1 - (lx + left);
            if (py < 0 || py >= fb_h) {
                continue;
            }
            FbPutNative(fb, px, py, color, fb_w);
        }
        if ((ly & 63) == 0) {
            taskYIELD();
        }
    }
    ESP_LOGD(TAG, "blit area %d,%d-%d,%d cost=%lldms", x1, y1, x2, y2,
             (long long)((esp_timer_get_time() - t0) / 1000));
}

static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    if (s_freeze_updates.load(std::memory_order_relaxed)) {
        lv_display_flush_ready(disp);
        return;
    }
    EpdiyHighlevelState* hl = MetalioEpd_Hl();
    uint8_t* fb = hl != nullptr ? epd_hl_get_framebuffer(hl) : nullptr;
    if (fb == nullptr || px_map == nullptr || area == nullptr) {
        lv_display_flush_ready(disp);
        return;
    }

    const int disp_w = (int)lv_display_get_horizontal_resolution(disp);
    const int disp_h = (int)lv_display_get_vertical_resolution(disp);
    // force/GC16：先抹白再叠脏区；普通 PARTIAL 仍叠上一帧（翻页）
    if (s_force_full.load(std::memory_order_relaxed) && !s_fb_white_for_force) {
        epd_hl_set_all_white(hl);
        s_fb_white_for_force = true;
        ESP_LOGI(TAG, "force-full: FB wiped white before blit (inset T%d B%d L%d R%d)",
                 DISPLAY_CONTENT_TOP_INSET, DISPLAY_CONTENT_BOTTOM_INSET, DISPLAY_CONTENT_LEFT_INSET,
                 DISPLAY_CONTENT_RIGHT_INSET);
    }
    blit_i1_area_to_epdiy(px_map, area, fb, disp_w, disp_h);

    if (lv_display_flush_is_last(disp)) {
        const int64_t t0 = esp_timer_get_time();
        const bool force = s_force_full.exchange(false);
        ESP_LOGI(TAG, "flush last area=%d,%d-%d,%d force=%d defer=%d", area->x1, area->y1, area->x2,
                 area->y2, force ? 1 : 0, s_defer_standby_commit.load() ? 1 : 0);
        // 进待机：只把 LVGL 叠进 FB，不上墨；由 Park 一次 GC16 带出待机画面
        if (!s_defer_standby_commit.load(std::memory_order_relaxed)) {
            epd_commit_update(hl, force);
            if (s_restore_frontlight_after_commit.exchange(false, std::memory_order_relaxed)) {
                Frontlight::GetInstance().RestoreBrightness();
            }
        }
        s_fb_white_for_force = false;
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

    EpdiyHighlevelState* hl = MetalioEpd_Hl();
    epd_panel_enter();
    epd_poweron();
    epd_fullclear(hl, kEpdTempC);
    epd_poweroff();
    epd_panel_exit();
    s_partial_count = 0;

    lv_init();
    lv_tick_set_cb(lv_tick_cb);

    s_disp = lv_display_create(width_, height_);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_I1);
    DisplayUiBind(s_disp, epd_width(), epd_height());

    const uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)width_, LV_COLOR_FORMAT_I1);
    const uint32_t pal_bytes =
        LV_COLOR_INDEXED_PALETTE_SIZE(LV_COLOR_FORMAT_I1) * (uint32_t)sizeof(lv_color32_t);
    const size_t buf_size = (size_t)pal_bytes + (size_t)stride * (size_t)height_;
    uint8_t* buf1 = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf1 == nullptr) {
        buf1 = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (buf1 == nullptr) {
        ESP_LOGE(TAG, "no draw buffer %u", (unsigned)buf_size);
        return false;
    }
    lv_display_set_buffers(s_disp, buf1, nullptr, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
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
    s_force_full.store(true);
    lv_refr_now(s_disp);

    {
        esp_timer_create_args_t notification_timer_args = {
            .callback =
                [](void* arg) {
                    auto* display = static_cast<LvAdapterEpdiy*>(arg);
                    DisplayLockGuard lock(display);
                    if (display->notification_label_ != nullptr) {
                        lv_obj_add_flag(display->notification_label_, LV_OBJ_FLAG_HIDDEN);
                    }
                    if (display->status_label_ != nullptr && !AssistantScreen::IsPttWaveVisible()) {
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
    return OtaUpgradeScreen::IsActive() || OtaConfirmDialog::IsActive();
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

// MetalioKeys index：0=BOOT 1=音量+ 2=音量- 3=电源；长按阈值同盖板虚拟键（电源长按走 TakeShutdownPulse）
static bool s_side_prev_down[4] = {};
static int64_t s_side_down_us[4] = {};
static bool s_side_long_fired[4] = {};
static bool s_side_synced = false;

static void PollOneSideKey(int index, uint32_t long_ms, void (*on_click)(), void (*on_long)()) {
    if (index < 0 || index >= 4) {
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
    }
    s_side_prev_down[index] = down;
}

static void OnVolUpClick() {
    ApplyVolumeDelta(10);
}
static void OnVolUpLong() {
    ApplyVolumeAbsolute(100, Lang::Strings::MAX_VOLUME);
}
static void OnVolDownClick() {
    ApplyVolumeDelta(-10);
}
static void OnVolDownLong() {
    ApplyVolumeAbsolute(0, Lang::Strings::MUTED);
}
static void OnPowerClick() {
    PowerKey_OnClick();
}
static void OnPowerLong() {
    // 硬件关机脉冲由 DisplayTickTask 里 TakeShutdownPulse 触发；此处仅记日志/策略钩子
    PowerKey_OnLongPress();
}

static void PollSideKeys() {
    // 浅睡 mask 期间 IsDown 恒 false；勿把「假松开」合成音量 click
    if (!MetalioKeys_IsReady()) {
        s_side_synced = false;
        return;
    }
    if (!s_side_synced) {
        // 恢复瞬间若键仍按着：吞掉本段手势（长按/短按都不当新事件）。
        // 切勿把 down_us 置 0：否则下一拍 (now-0)≥阈值，电源短按会被当成已满 3s 关机。
        const int64_t now = esp_timer_get_time();
        for (int i = 1; i <= 3; ++i) {
            const bool down = MetalioKeys_IsDown(i);
            s_side_prev_down[i] = down;
            s_side_long_fired[i] = down;
            s_side_down_us[i] = down ? now : 0;
        }
        s_side_synced = true;
        return;
    }
    PollOneSideKey(1, TOUCH_VK_LONG_MS, OnVolUpClick, OnVolUpLong);
    PollOneSideKey(2, TOUCH_VK_LONG_MS, OnVolDownClick, OnVolDownLong);
    // 电源长按阈值与 MetalioKeys 内脉冲一致，避免短按误触
    PollOneSideKey(3, POWER_LONG_PRESS_MS, OnPowerClick, OnPowerLong);
}

void LvAdapterEpdiy::Tick() {
    if (!lvgl_ready_) {
        return;
    }
    if (Lock(50)) {
        // OpenDone 无锁投递：先于 timer_handler，避免整屏 I1 占锁时回调永远进不来
        BookDrainPendingOpenDone();
        const int64_t t0 = esp_timer_get_time();
        lv_timer_handler();
        const int64_t dt = esp_timer_get_time() - t0;
        Unlock();
        if (dt > 200 * 1000) {
            ESP_LOGW(TAG, "lv_timer_handler slow %lldms", (long long)(dt / 1000));
        }
    }
    PollBootKey();
    PollSideKeys();
    while (MetalioKeys_TakeEdge()) {
    }
}

void LvAdapterEpdiy::SetSystemReady() {
    SetStatus(Lang::Strings::STANDBY);
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
    lv_obj_t* scr = lv_screen_active();
    if (scr != nullptr) {
        lv_obj_invalidate(scr);
    }
    if (Lock(1000)) {
        lv_refr_now(s_disp);
        Unlock();
    }
}

void LvAdapterEpdiy::SetStreamPaintMode(bool on) {
    s_stream_paint.store(on);
    if (!on) {
        s_force_full.store(true);
    }
}

void LvAdapterEpdiy::FreezeFlush(bool on) {
    s_freeze_updates.store(on, std::memory_order_relaxed);
}

void LvAdapterEpdiy::BeginStandbyEnterPaint() {
    s_stream_paint.store(false);
    s_defer_standby_commit.store(true, std::memory_order_relaxed);
    RequestFullOnNextCommit();
    ESP_LOGI(TAG, "standby enter: defer panel commit until Park FULL");
}

void LvAdapterEpdiy::ParkEpdForStandby() {
    s_defer_standby_commit.store(false, std::memory_order_relaxed);
    SetStreamPaintMode(false);
    FullRefresh();
    // 待机画面已上墨后再关前光，避免关得过早还看得见底层页
    Frontlight::GetInstance().ForceAllLow();
    FreezeFlush(true);
    ESP_LOGI(TAG, "standby: FULL parked, flush frozen");
}

void LvAdapterEpdiy::WakeEpdFromStandby() {
    s_defer_standby_commit.store(false, std::memory_order_relaxed);
    FreezeFlush(false);
    SetStreamPaintMode(false);
    RequestFullOnNextCommit();
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

/** L8（0=黑 255=白）→ EPDiy FB，在内容区内居中；逻辑竖屏映射同 blit_i1 */
bool BlitL8ToEpdContent(const reader::RasterImage& img) {
    EpdiyHighlevelState* hl = MetalioEpd_Hl();
    uint8_t* fb = hl != nullptr ? epd_hl_get_framebuffer(hl) : nullptr;
    if (fb == nullptr || img.empty()) {
        return false;
    }
    epd_hl_set_all_white(hl);
    const int max_w = DISPLAY_CONTENT_W;
    const int max_h = DISPLAY_CONTENT_H;
    const int draw_w = img.width < max_w ? img.width : max_w;
    const int draw_h = img.height < max_h ? img.height : max_h;
    const int left = DISPLAY_CONTENT_LEFT_INSET + (max_w - draw_w) / 2;
    const int top = DISPLAY_CONTENT_TOP_INSET + (max_h - draw_h) / 2;
    const int fb_w = epd_width();
    const int fb_h = epd_height();
    for (int ly = 0; ly < draw_h; ++ly) {
        const int px = ly + top;
        if (px < 0 || px >= fb_w) {
            continue;
        }
        const uint8_t* row = img.pixels.data() + static_cast<size_t>(ly) * img.width;
        for (int lx = 0; lx < draw_w; ++lx) {
            const int py = EPD_LOGICAL_W - 1 - (lx + left);
            if (py < 0 || py >= fb_h) {
                continue;
            }
            FbPutNative(fb, px, py, row[lx], fb_w);
        }
        if ((ly & 63) == 0) {
            taskYIELD();
        }
    }
    epd_commit_update(hl, true);
    return true;
}

bool TryDecodeShutdownFromSd(reader::RasterImage& out) {
    char path[192] = {};
    if (!Wallpaper_TryResolveActiveWallpaperPath(path, sizeof(path))) {
        ESP_LOGI(TAG, "shutdown img: no NVS shutdown file");
        return false;
    }
    if (!reader::DecodeImageFileToL8(path, DISPLAY_CONTENT_W, DISPLAY_CONTENT_H, out) ||
        out.empty()) {
        ESP_LOGW(TAG, "shutdown img: SD decode fail %s", path);
        return false;
    }
    ESP_LOGI(TAG, "shutdown img: SD %s %dx%d", path, out.width, out.height);
    return true;
}

bool TryDecodeShutdownBuiltin(reader::RasterImage& out) {
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
    if (!reader::DecodeImageToL8(p, n, DISPLAY_CONTENT_W, DISPLAY_CONTENT_H, out) || out.empty()) {
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

}  // namespace

void LvAdapterEpdiy::ShowPoweredOffScreen() {
    // 解冻以便本帧能刷；关机画面只走这一次全刷上墨
    FreezeFlush(false);
    s_defer_standby_commit.store(false, std::memory_order_relaxed);
    s_stream_paint.store(false);

    DisplayLockGuard lock(this);
    BindStatusWidgets(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);

    reader::RasterImage img;
    bool have_img = TryDecodeShutdownFromSd(img);
    if (!have_img) {
        have_img = TryDecodeShutdownBuiltin(img);
    }

    if (have_img) {
        if (!BlitL8ToEpdContent(img)) {
            have_img = false;
        }
    }

    if (!have_img) {
        ShowPoweredOffTextFallback();
    }

    FreezeFlush(true);
    ESP_LOGI(TAG, "powered-off screen shown (a2i1=%d)", have_img ? 1 : 0);
}

void LvAdapterEpdiy::SetStatusTitlePrefix(const char* prefix) {
    DisplayLockGuard lock(this);
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
        if (!AssistantScreen::IsPttWaveVisible()) {
            lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
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
    if (status_label_ == nullptr) {
        return;
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
    if (wifi_busy != nullptr && status_label_ != nullptr) {
        ApplyStatusTextLocked(wifi_busy);
        if (!AssistantScreen::IsPttWaveVisible()) {
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
        if (!AssistantScreen::IsPttWaveVisible()) {
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
