#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>
#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <esp_mmap_assets.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sys/stat.h>
#include <wifi_station.h>
#include <font_awesome.h>
#include "esp_lv_adapter.h"
#include "esp_lv_fs.h"
#include "mmap_generate_resources.h"
#include "infra/tasks/task_create.h"
#include "ui/common/runtime/screen_common_nav.h"
#include "ui/common/runtime/display_ui.h"
#include "sysmgmt/power/standby_presentation.h"
#include "sysmgmt/power/standby_wallpaper.h"
#include "business/conversation/conversation_presentation.h"
#include "business/conversation/activation_polling.h"
#include "sysmgmt/system_state.h"
#include "lv_adapter_ssd1677.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "boards/common/audio/audio_codec.h"
#include "boards/common/board/board.h"
#include "boards/common/display/ssd1677/epd_gray_aa.h"
#include "boards/common/display/ssd1677/epd_i1_glyph_thin.h"
#include "boards/common/display/ssd1677/epd_i1_rotate.h"
#include "boards/common/display/ssd1677/epd_reader_transition.h"
#include "boards/common/display/ssd1677/esp_lcd_panel_ssd1677.h"
#include "boards/common/display/ssd1677/esp_lcd_ssd1677_commands.h"
#include "boards/common/input/input.h"
#include "boards/common/power/power_hw.h"
#include "boards/common/storage/SdCardManager.hpp"
#include "infra/storage/sd_paths.h"
#include "infra/image/image_geometry.h"
#include "infra/image/image_tone.h"
#include "infra/image/image_decode.h"
#include "business/reader/reader_types.h"
#include "business/wallpaper/wallpaper_active.h"
#include "config.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "ui/common/drawing/a2ui/a2ui_image.h"
#include "ui/common/drawing/adapter/epd_refresh.h"
#include "ui/common/drawing/display_orient.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/runtime/input/input_dispatch.h"
#include "ui/common/drawing/rendering/network_status.h"
#include "ui/common/drawing/rendering/wallpaper_frame.h"
#include "business/reader/book_reader_prefs.h"
#include "sysmgmt/update/update_presentation.h"
#include "ui/common/runtime/input/pager_key_handler.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/widgets/screen_frontlight.h"
#include "ui/common/runtime/input/screen_gesture.h"
#include "ui/common/widgets/touch_missing_screen.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/drawing/ui_scale.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"



static const char* TAG = "LvAdapterSsd1677";
static const lv_area_t* s_wallpaper_notice_clip = nullptr;
static bool s_wallpaper_loading_shown = false;

LvAdapterSsd1677* LvAdapterSsd1677::instance_ = nullptr;

#pragma pack(push, 1)

struct LvAdapterSsd1677A2i1Header {
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint16_t stride;
    uint16_t reserved;
};

#pragma pack(pop)

static bool ParseA2i1(const uint8_t* buf, size_t len, uint16_t* w, uint16_t* h, uint16_t* stride,
                      const uint8_t** payload) {
    if (buf == nullptr || len < sizeof(LvAdapterSsd1677A2i1Header) + 8) {
        return false;
    }
    const auto* hdr = reinterpret_cast<const LvAdapterSsd1677A2i1Header*>(buf);
    if (hdr->magic != 0x31493241u || hdr->width == 0 || hdr->height == 0 || hdr->stride == 0) {
        return false;
    }
    const size_t need =
        sizeof(LvAdapterSsd1677A2i1Header) + 8u + static_cast<size_t>(hdr->stride) * static_cast<size_t>(hdr->height);
    if (len < need) {
        return false;
    }
    if (w) {
        *w = hdr->width;
    }
    if (h) {
        *h = hdr->height;
    }
    if (stride) {
        *stride = hdr->stride;
    }
    if (payload) {
        *payload = buf + sizeof(LvAdapterSsd1677A2i1Header); /* palette (8) + bitmap */
    }
    return true;
}

// 优先用 SD 上由 NVS 指向的壁纸；调用方负责用 heap_caps_free 释放。
static bool TryLoadShutdownA2i1FromSd(uint8_t** out_buf, size_t* out_len) {
    if (out_buf == nullptr || out_len == nullptr) {
        return false;
    }
    *out_buf = nullptr;
    *out_len = 0;

    if (!SdCardManager::GetInstance().IsMounted()) {
        ESP_LOGD(TAG, "shutdown img: SD not mounted → built-in");
        return false;
    }

    char path[192] = {};
    if (!Wallpaper_TryResolveActiveWallpaperPath(path, sizeof(path))) {
        ESP_LOGD(TAG, "shutdown img: no NVS shutdown file → built-in");
        return false;
    }

    struct stat st{};
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        ESP_LOGD(TAG, "shutdown img: missing; using embedded resource");
        return false;
    }
    // A2I1 头至少 magic+wh+stride+res+palette = 20
    constexpr off_t kMinBytes = 20;
    constexpr off_t kMaxBytes = 256 * 1024;
    if (st.st_size < kMinBytes || st.st_size > kMaxBytes) {
        ESP_LOGW(TAG, "shutdown img: bad size %ld → built-in", static_cast<long>(st.st_size));
        return false;
    }

    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "shutdown img: open failed errno=%d; using embedded resource", errno);
        return false;
    }

    const size_t need = static_cast<size_t>(st.st_size);
    uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        buf = static_cast<uint8_t*>(heap_caps_malloc(need, MALLOC_CAP_8BIT));
    }
    if (buf == nullptr) {
        fclose(f);
        ESP_LOGW(TAG, "shutdown img: alloc %u fail → built-in", static_cast<unsigned>(need));
        return false;
    }

    const size_t nread = fread(buf, 1, need, f);
    fclose(f);
    if (nread != need) {
        heap_caps_free(buf);
        ESP_LOGW(TAG, "shutdown img: short read %u/%u → built-in", static_cast<unsigned>(nread),
                 static_cast<unsigned>(need));
        return false;
    }

    // 快速魔数校验，坏文件留给内置，避免关机白屏/花屏
    if (!(need >= 4 && buf[0] == 'A' && buf[1] == '2' && buf[2] == 'I' && buf[3] == '1')) {
        heap_caps_free(buf);
        ESP_LOGW(TAG, "shutdown img: invalid A2I1; using embedded resource");
        return false;
    }

    ESP_LOGI(TAG, "shutdown img: SD bytes=%u", static_cast<unsigned>(need));
    *out_buf = buf;
    *out_len = need;
    return true;
}

static constexpr size_t kMaxTouchVirtualKeys = 8;
static TouchVirtualKey s_vk_keys[kMaxTouchVirtualKeys];
static size_t s_vk_count = 0;
static TouchVirtualKeyEventCb s_vk_cb = nullptr;
static void* s_vk_user = nullptr;

// long_press_ms==0：按下即 Click（旧行为），仅用 was_down 防抖。
static bool s_vk_instant_was_down = false;
static bool s_finger_was_down = false;

// long_press_ms>0：完整按住生命周期（无堆分配，无定时器句柄）。
struct LvAdapterSsd1677TouchVkHoldState {
    const char* name = nullptr;
    uint16_t long_press_ms = 0;
    int64_t down_us = 0;
    bool long_emitted = false;
    bool long_consumed = false;
};

static LvAdapterSsd1677TouchVkHoldState s_vk_hold{};

// 盖板键与 LVGL 刷屏并行：feed 读 I2C + 发虚拟键；屏内点走边沿锁存再喂 LVGL。
// 1=打印 feed latch / indev deliver 详细轨迹；量产保持 0。
#ifndef LV_ADAPTER_TOUCH_TRACE
#define LV_ADAPTER_TOUCH_TRACE 0
#endif
static constexpr int kTouchFeedPeriodMs = 10;
static constexpr UBaseType_t kTouchFeedPriority = 5;  // > LVGL(1)
static constexpr uint8_t kTouchEdgeQueueDepth = 50;   // LVGL 忙时 down/up 边沿积压上限

struct LvAdapterSsd1677TouchDownEdge {
    uint16_t x = 0;
    uint16_t y = 0;
};

struct LvAdapterSsd1677TouchFeedSnapshot {
    uint8_t live_count = 0;  // 当前是否按着（UI，非盖板键）
    uint16_t live_x = 0;
    uint16_t live_y = 0;
    LvAdapterSsd1677TouchDownEdge down_q[kTouchEdgeQueueDepth]{};  // 按下边沿坐标环（按次交付，勿覆盖）
    uint8_t down_head = 0;                                         // 下一笔待交付
    uint8_t pending_down = 0;                                      // 未交付给 LVGL 的按下边沿
    uint8_t pending_up = 0;                                        // 未交付给 LVGL 的抬起边沿
    bool lvgl_down = false;                                        // LVGL 侧是否已处于 pressed
    bool suppress_lvgl = false;                                    // 盖板键占用中
};

static portMUX_TYPE s_touch_snap_mux = portMUX_INITIALIZER_UNLOCKED;
static LvAdapterSsd1677TouchFeedSnapshot s_touch_snap{};
static lv_point_t s_touch_start{};
static lv_indev_read_cb_t s_adapter_touch_read = nullptr;
static TaskHandle_t s_touch_feed_task = nullptr;
static volatile bool s_touch_feed_run = false;
static esp_lcd_touch_handle_t s_touch_tp = nullptr;
static bool s_standby_background_paused = false;

static const TouchVirtualKey* HitVirtualKey(int x, int y) {
    for (size_t i = 0; i < s_vk_count; ++i) {
        if (x == s_vk_keys[i].x && y == s_vk_keys[i].y) {
            return &s_vk_keys[i];
        }
    }
    return nullptr;
}

static bool EmitVkEvent(const char* name, TouchVkEvent event) {
    if (s_vk_cb == nullptr || name == nullptr) {
        return false;
    }
    // 与 BOOT 键同：可在非 LVGL 任务调用；页面跳转内部须 ScreenLvAsync，勿在此同步碰 LVGL。
    const int64_t t0 = esp_timer_get_time();
    const bool r = s_vk_cb(name, event, s_vk_user);
    const int us = static_cast<int>(esp_timer_get_time() - t0);
    if (event == TouchVkEvent::Click || event == TouchVkEvent::LongPress) {
        ESP_LOGD(TAG, "vk cb %s ev=%d ret=%d took %d us", name, static_cast<int>(event), r ? 1 : 0, us);
        if (us > 20000) {
            ESP_LOGW(TAG, "vk cb %s blocked touch_feed %d us", name, us);
        }
    }
    return r;
}

static void EndHoldPress(bool emit_click_if_unconsumed) {
    if (s_vk_hold.name == nullptr) {
        return;
    }
    const char* name = s_vk_hold.name;
    const bool emit_click = emit_click_if_unconsumed && !s_vk_hold.long_consumed;
    EmitVkEvent(name, TouchVkEvent::PressUp);
    if (emit_click) {
        // 可长按键短按：按下未震，松手 Click 时补震
        HapticPulseIfEnabled();
    }
    s_vk_hold = {};
}

// 结束一次 UI 按下（抬起或改点到盖板键）：锁存 up，避免 LVGL 忙碌时丢掉松手。
static bool TouchFeedUiReleaseLocked() {
    if (s_touch_snap.live_count == 0) {
        return false;
    }
    s_touch_snap.live_count = 0;
    if (s_touch_snap.pending_up < kTouchEdgeQueueDepth) {
        s_touch_snap.pending_up++;
    }
    return true;
}

static bool TouchFeedUiPressLocked(uint16_t x, uint16_t y) {
    s_touch_start = {x, y};
    s_touch_snap.live_count = 1;
    s_touch_snap.live_x = x;
    s_touch_snap.live_y = y;
    if (s_touch_snap.pending_down < kTouchEdgeQueueDepth) {
        const uint8_t i =
            static_cast<uint8_t>((s_touch_snap.down_head + s_touch_snap.pending_down) % kTouchEdgeQueueDepth);
        s_touch_snap.down_q[i].x = x;
        s_touch_snap.down_q[i].y = y;
        s_touch_snap.pending_down++;
    }
    return true;
}

static void TouchFeedKickLvgl(const char* why, uint8_t pend_d, uint8_t pend_u, uint8_t live, bool lvgl_d, uint16_t x,
                              uint16_t y) {
#if LV_ADAPTER_TOUCH_TRACE
    ESP_LOGD(TAG, "feed latch %s pend_d=%u pend_u=%u live=%u lvgl_d=%d xy=%u,%u → kick indev", why, pend_d, pend_u,
             live, lvgl_d ? 1 : 0, x, y);
#else
    (void)why;
    (void)pend_d;
    (void)pend_u;
    (void)live;
    (void)lvgl_d;
    (void)x;
    (void)y;
#endif
    if (auto* disp = LvAdapterSsd1677::Instance()) {
        disp->KickTouchInput();
    }
}

// touch_feed：按下边沿早震；盖板键逻辑；屏内锁存 down/up 边沿供 LVGL 补齐 CLICKED。
static void TouchFeedHandleSample(uint8_t count, int x, int y) {
    if (count == 0) {
        if (s_finger_was_down) {
            ESP_LOGD(TAG, "touch up");
        }
        s_finger_was_down = false;
        s_vk_instant_was_down = false;
        EndHoldPress(true);
        bool kick = false;
        uint8_t pend_d = 0, pend_u = 0, live = 0;
        bool lvgl_d = false;
        uint16_t lx = 0, ly = 0;
        portENTER_CRITICAL(&s_touch_snap_mux);
        s_touch_snap.suppress_lvgl = false;
        kick = TouchFeedUiReleaseLocked();
        pend_d = s_touch_snap.pending_down;
        pend_u = s_touch_snap.pending_up;
        live = s_touch_snap.live_count;
        lvgl_d = s_touch_snap.lvgl_down;
        lx = s_touch_snap.live_x;
        ly = s_touch_snap.live_y;
        portEXIT_CRITICAL(&s_touch_snap_mux);
        if (kick) {
            TouchFeedKickLvgl("ui_up", pend_d, pend_u, live, lvgl_d, lx, ly);
        }
        return;
    }

    PowerPolicy::GetInstance().NotifyUserActivity();

    if (!s_finger_was_down) {
        s_finger_was_down = true;
        const TouchVirtualKey* early_hit = HitVirtualKey(x, y);
        int ui_x = x;
        int ui_y = y;
        if (early_hit == nullptr) {
            DisplayUiTouchToLogical(x, y, &ui_x, &ui_y);
        }
        ESP_LOGD(TAG, "touch down x=%d y=%d %s", ui_x, ui_y, early_hit != nullptr ? early_hit->name : "ui");
        if (early_hit != nullptr) {
            HapticBeginPress();
            // 可长按键：按下不震，短按 Click / 长按达成时再震（避免连震两下）
            if (early_hit->long_press_ms == 0) {
                HapticPulseOnFingerDown();
            }
        } else {
            HapticBeginPress();
            HapticTryPulseAtUiPoint(static_cast<int16_t>(ui_x), static_cast<int16_t>(ui_y));
        }
    }

    const TouchVirtualKey* hit = HitVirtualKey(x, y);
    if (hit == nullptr) {
        s_vk_instant_was_down = false;
        EndHoldPress(true);
        int ui_x = x;
        int ui_y = y;
        DisplayUiTouchToLogical(x, y, &ui_x, &ui_y);
        const uint16_t ux = static_cast<uint16_t>(ui_x);
        const uint16_t uy = static_cast<uint16_t>(ui_y);
        bool kick = false;
        uint8_t pend_d = 0, pend_u = 0, live = 0;
        bool lvgl_d = false;
        portENTER_CRITICAL(&s_touch_snap_mux);
        s_touch_snap.suppress_lvgl = false;
        if (s_touch_snap.live_count == 0) {
            kick = TouchFeedUiPressLocked(ux, uy);
        } else {
            s_touch_snap.live_x = ux;
            s_touch_snap.live_y = uy;
        }
        pend_d = s_touch_snap.pending_down;
        pend_u = s_touch_snap.pending_up;
        live = s_touch_snap.live_count;
        lvgl_d = s_touch_snap.lvgl_down;
        portEXIT_CRITICAL(&s_touch_snap_mux);
        if (kick) {
            TouchFeedKickLvgl("ui_down", pend_d, pend_u, live, lvgl_d, ux, uy);
        }
        return;
    }

    // 盖板键：不喂 LVGL；若刚才在点 UI，补一次抬起边沿
    bool kick = false;
    uint8_t pend_d = 0, pend_u = 0, live = 0;
    bool lvgl_d = false;
    uint16_t lx = 0, ly = 0;
    portENTER_CRITICAL(&s_touch_snap_mux);
    s_touch_snap.suppress_lvgl = true;
    kick = TouchFeedUiReleaseLocked();
    pend_d = s_touch_snap.pending_down;
    pend_u = s_touch_snap.pending_up;
    live = s_touch_snap.live_count;
    lvgl_d = s_touch_snap.lvgl_down;
    lx = s_touch_snap.live_x;
    ly = s_touch_snap.live_y;
    portEXIT_CRITICAL(&s_touch_snap_mux);
    if (kick) {
        TouchFeedKickLvgl("ui_to_vk", pend_d, pend_u, live, lvgl_d, lx, ly);
    }

    if (hit->long_press_ms == 0) {
        EndHoldPress(true);
        if (!s_vk_instant_was_down) {
            s_vk_instant_was_down = true;
            ESP_LOGI(TAG, "virtual key [%s] click", hit->name);
            EmitVkEvent(hit->name, TouchVkEvent::Click);
        }
        return;
    }

    s_vk_instant_was_down = false;

    if (s_vk_hold.name != nullptr && std::strcmp(s_vk_hold.name, hit->name) != 0) {
        EndHoldPress(true);
    }

    if (s_vk_hold.name == nullptr) {
        s_vk_hold.name = hit->name;
        s_vk_hold.long_press_ms = hit->long_press_ms;
        s_vk_hold.down_us = esp_timer_get_time();
        s_vk_hold.long_emitted = false;
        s_vk_hold.long_consumed = false;
        EmitVkEvent(hit->name, TouchVkEvent::PressDown);
        ESP_LOGD(TAG, "virtual key [%s] press-down (long=%ums)", hit->name, static_cast<unsigned>(hit->long_press_ms));
        return;
    }

    if (!s_vk_hold.long_emitted) {
        const int64_t elapsed_ms = (esp_timer_get_time() - s_vk_hold.down_us) / 1000;
        if (elapsed_ms >= static_cast<int64_t>(s_vk_hold.long_press_ms)) {
            s_vk_hold.long_emitted = true;
            ESP_LOGD(TAG, "virtual key [%s] long-press %ums", s_vk_hold.name,
                     static_cast<unsigned>(s_vk_hold.long_press_ms));
            s_vk_hold.long_consumed = EmitVkEvent(s_vk_hold.name, TouchVkEvent::LongPress);
        }
    }
}

static void TouchFeedTask(void* arg) {
    auto* tp = static_cast<esp_lcd_touch_handle_t>(arg);
    esp_lcd_touch_point_data_t pts[1] = {};
    ESP_LOGD(TAG, "touch_feed start prio=%u period=%dms", static_cast<unsigned>(kTouchFeedPriority),
             kTouchFeedPeriodMs);
    while (s_touch_feed_run) {
        uint8_t count = 0;
        // 待机 LP 已写 A5 深睡：勿再 I2C 读（CST816S 不应答 → panel_io 刷 E 级日志）
        if (tp != nullptr && !PowerPolicy::GetInstance().IsTouchAsleep()) {
            if (esp_lcd_touch_read_data(tp) == ESP_OK) {
                if (esp_lcd_touch_get_data(tp, pts, &count, 1) != ESP_OK) {
                    count = 0;
                }
            }
        }
        const int x = count > 0 ? static_cast<int>(pts[0].x) : 0;
        const int y = count > 0 ? static_cast<int>(pts[0].y) : 0;
        TouchFeedHandleSample(count, x, y);
        vTaskDelay(pdMS_TO_TICKS(kTouchFeedPeriodMs));
    }
    s_touch_feed_task = nullptr;
    vTaskDelete(nullptr);
}

static void StartTouchFeed(esp_lcd_touch_handle_t tp) {
    if (tp == nullptr || s_touch_feed_task != nullptr) {
        return;
    }
    s_touch_feed_run = true;
    BaseType_t ok =
        FirmwareCreateTask(TouchFeedTask, "touch_feed", 4096, tp, kTouchFeedPriority, &s_touch_feed_task, 0, 0);
    if (ok != pdPASS) {
        s_touch_feed_run = false;
        s_touch_feed_task = nullptr;
        ESP_LOGE(TAG, "touch_feed create failed");
    }
}

static void StopTouchFeed() {
    if (s_touch_feed_task == nullptr) {
        return;
    }
    s_touch_feed_run = false;
    for (int i = 0; i < 50 && s_touch_feed_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// LVGL indev：按边沿交付，而不是只读「此刻快照」。
// 否则 LVGL 忙时整段 down→up 被折叠成一次 up，出现「第一次只震不点、第二次才切页」。
static esp_err_t CustomTouchRead(esp_lcd_touch_handle_t tp, esp_lcd_touch_point_data_t* points, uint8_t* count,
                                 uint8_t max_count, void* /*user_ctx*/) {
    (void)tp;
    if (points == nullptr || count == nullptr || max_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (InputSamplingPaused()) {
        *count = 0;
        return ESP_OK;
    }

    uint8_t out_count = 0;
    uint16_t out_x = 0;
    uint16_t out_y = 0;
    const char* why = "idle";
    uint8_t pend_d = 0, pend_u = 0, live = 0;
    bool lvgl_d_before = false;

    portENTER_CRITICAL(&s_touch_snap_mux);
    auto& s = s_touch_snap;
    pend_d = s.pending_down;
    pend_u = s.pending_up;
    live = s.live_count;
    lvgl_d_before = s.lvgl_down;
    if (s.lvgl_down) {
        if (s.pending_up > 0) {
            s.pending_up--;
            s.lvgl_down = false;
            out_count = 0;
            why = "deliver_up(pend)";
        } else if (s.live_count > 0 && !s.suppress_lvgl) {
            out_count = 1;
            out_x = s.live_x;
            out_y = s.live_y;
            why = "hold";
        } else {
            // 已松开但边沿被吞：补一次 up，避免粘住 PRESSED
            s.lvgl_down = false;
            out_count = 0;
            why = "deliver_up(force)";
        }
    } else if (s.pending_down > 0) {
        out_x = s.down_q[s.down_head].x;
        out_y = s.down_q[s.down_head].y;
        s.down_head = static_cast<uint8_t>((s.down_head + 1) % kTouchEdgeQueueDepth);
        s.pending_down--;
        s.lvgl_down = true;
        out_count = 1;
        why = "deliver_down(pend)";
    } else if (s.live_count > 0 && !s.suppress_lvgl) {
        s.lvgl_down = true;
        out_count = 1;
        out_x = s.live_x;
        out_y = s.live_y;
        why = "deliver_down(live)";
    } else {
        out_count = 0;
        why = "idle";
    }
    const uint8_t pend_d_after = s.pending_down;
    const uint8_t pend_u_after = s.pending_up;
    const bool lvgl_d_after = s.lvgl_down;
    // down 已交付但 up 仍锁存：必须再踢一次 indev，否则 LVGL 可能不再读，CLICKED 永不到
    const bool kick_again = (out_count == 1 && pend_u_after > 0) || (out_count == 0 && pend_d_after > 0);
    portEXIT_CRITICAL(&s_touch_snap_mux);

    // 只打边沿交付，避免 hold/idle 刷屏；对照 feed latch 时间戳即可定位
#if LV_ADAPTER_TOUCH_TRACE
    if (why[0] == 'd') {  // deliver_*
        ESP_LOGI(TAG,
                 "indev %s out=%u xy=%u,%u before(pend_d=%u pend_u=%u live=%u lvgl_d=%d) "
                 "after(pend_d=%u pend_u=%u lvgl_d=%d)%s",
                 why, out_count, out_x, out_y, pend_d, pend_u, live, lvgl_d_before ? 1 : 0, pend_d_after, pend_u_after,
                 lvgl_d_after ? 1 : 0, kick_again ? " → kick again" : "");
    }
#else
    (void)why;
    (void)pend_d;
    (void)pend_u;
    (void)live;
    (void)lvgl_d_before;
    (void)pend_d_after;
    (void)pend_u_after;
    (void)lvgl_d_after;
#endif

    if (kick_again) {
        if (auto* disp = LvAdapterSsd1677::Instance()) {
            disp->KickTouchInput();
        }
    }

    if (out_count == 0) {
        *count = 0;
        return ESP_OK;
    }
    *count = 1;
    points[0] = {};
    points[0].x = out_x;
    points[0].y = out_y;
    return ESP_OK;
}

static bool MapLogicalToUi(int x, int y, int* ux, int* uy, bool clamp_border = false) {
    (void)clamp_border;
    // 397 没有面板内容留白；适配器已将触点转换到当前逻辑方向。
    if (x < 0 || x >= DisplayUiLogW() || y < 0 || y >= DisplayUiLogH()) {
        return false;
    }
    *ux = x;
    *uy = y;
    return true;
}

static void ReadTouchWithQuickPanel(lv_indev_t* indev, lv_indev_data_t* data) {
    static bool tracking = false;
    static bool consumed = false;
    static bool close_panel = false;
    static bool ignore_until_release = false;
    static lv_point_t origin = {};
    static lv_obj_t* start_screen = nullptr;
    s_adapter_touch_read(indev, data);
    const bool pressed = data->state == LV_INDEV_STATE_PRESSED;
    lv_point_t start;
    lv_point_t last;
    portENTER_CRITICAL(&s_touch_snap_mux);
    start = s_touch_start;
    last = {s_touch_snap.live_x, s_touch_snap.live_y};
    portEXIT_CRITICAL(&s_touch_snap_mux);

    struct {
        int x, y, start_x, start_y;
    } p = {pressed ? data->point.x : last.x, pressed ? data->point.y : last.y, start.x, start.y};

    const bool blocked = StandbyPresentation::IsActive() || UiInputQuickPanelBlocked();
    int swipe_x = 0, swipe_y = 0;
    int swipe_start_x = 0, swipe_start_y = 0;
    const bool swipe_valid = !blocked &&
        MapLogicalToUi(p.x, p.y, &swipe_x, &swipe_y, true) &&
        MapLogicalToUi(p.start_x, p.start_y, &swipe_start_x, &swipe_start_y, true);
    lv_point_t swipe_point = {swipe_x, swipe_y};
    if (ScreenHandleHorizontalSwipe(indev, pressed, swipe_point,
                                    {swipe_start_x, swipe_start_y}, swipe_valid)) {
        tracking = false;
        consumed = false;
        ignore_until_release = pressed;
        data->state = LV_INDEV_STATE_RELEASED;
        data->continue_reading = TouchUiHasPendingEdges();
        return;
    }
    if (blocked || (tracking && start_screen != lv_screen_active())) {
        tracking = false;
        consumed = false;
        ignore_until_release = pressed;
    }
    int ux = 0;
    int uy = 0;
    if ((pressed || tracking) && !MapLogicalToUi(p.x, p.y, &ux, &uy)) {
        // 从留白起滑时等待手指进入内容区，不将整次触摸标为无效。
        if (pressed && !blocked && !tracking && !ignore_until_release && MapLogicalToUi(p.x, p.y, &ux, &uy, true)) {
            data->state = LV_INDEV_STATE_RELEASED;
            data->continue_reading = TouchUiHasPendingEdges();
            return;
        }
        data->state = LV_INDEV_STATE_RELEASED;
        tracking = false;
        consumed = false;
        ignore_until_release = pressed;
        return;
    }
    if (pressed && !blocked && !ignore_until_release && !tracking) {
        tracking = true;
        int start_x = 0, start_y = 0;
        if (!MapLogicalToUi(p.start_x, p.start_y, &start_x, &start_y, true)) {
            data->state = LV_INDEV_STATE_RELEASED;
            tracking = false;
            ignore_until_release = true;
            return;
        }
        origin = {static_cast<int32_t>(start_x), static_cast<int32_t>(start_y)};
        start_screen = lv_screen_active();
        close_panel = ScreenFrontlightIsVisible();
        if ((close_panel && ScreenFrontlightOwnsDrag(origin.x, origin.y)) ||
            (!close_panel && ScreenPageOwnsVerticalDrag(origin))) {
            tracking = false;
            ignore_until_release = true;
        }
    }
    if (tracking && !consumed) {
        const int dy = uy - origin.y;
        const int distance = close_panel ? -dy : dy;
        if (distance >= UiSy(60) && distance > 2 * std::abs(ux - origin.x)) {
            consumed = true;
            lv_indev_wait_release(indev);
        }
    }
    if (!pressed) {
        data->state = LV_INDEV_STATE_RELEASED;
        if (consumed && !blocked) {
            // 松手后再创建遮罩，避免当前手势继续操作新面板。
            if (close_panel) {
                if (lv_async_call([](void*) { ScreenFrontlightHide(); }, nullptr) != LV_RESULT_OK) {
                    ESP_LOGW(TAG, "quick panel close request failed");
                }
            } else {
                if (lv_async_call(
                        [](void* screen) {
                            if (screen == lv_screen_active()) {
                                ScreenFrontlightShow();
                            }
                        },
                        start_screen) != LV_RESULT_OK) {
                    ESP_LOGW(TAG, "quick panel open request failed");
                }
            }
        }
        tracking = false;
        consumed = false;
        ignore_until_release = false;
        return;
    }
    if (consumed) {
        data->state = LV_INDEV_STATE_RELEASED;
        data->continue_reading = TouchUiHasPendingEdges();
        return;
    }
    data->state = LV_INDEV_STATE_PRESSED;
    data->point = swipe_point;
    // 短按回放：同一次 indev 周期内立刻再读到 RELEASED，才能生成 CLICKED
    if (TouchUiHasPendingEdges()) {
        data->continue_reading = true;
    }
}

/*
 * LVGL I1 / HTILED：1=白、0=黑，与面板 VRAM（0xFF=白）一致，写屏不取反。
 * 全刷：fast LUT → 0x26=0x24=新图 → 0x22=FULL → 0x20 → BUSY（旧 0xC7 / 优化 0xC4）
 * 局刷：partial LUT → 边框0x80 → 整帧只写 0x24 → 0x22=PARTIAL → 0x20（旧 0xCF / 优化 0xCC）
 * 阅读抗锯齿：局刷/偶发 FULL 铺 BW 底 → Overlay 掩码 → 灰 LUT + 0xCC
 * SSD1677_EPD_OPT_PATH=0 走旧流程；=1 启用优化档（含 0x37/0x3F）。
 * 每 15 次局刷强制一次全刷；开机/关机走全刷。
 *
 * 触摸：PARTIAL 启动后立刻 flush_ready；下次写屏用 TryFinish（不等 BUSY），忙则 coalesce。
 * BUSY 下降沿唤醒补刷任务；取得 adapter 锁后提交合并帧。
 * 盖板虚拟键：touch_feed 并行采点/震动/发事件（跳转内部 lv_async）。
 * 屏内：无锁对照 HapticAttachClick 区域表早震；feed 锁存 down/up 供 indev。
 */
static constexpr uint8_t kLvglWhite = 0xFF;
static constexpr int kPartialDirtyPadPx = 24;
static_assert(BOARD_DISPLAY_PARTIALS_PER_FULL > 0 && BOARD_DISPLAY_PARTIALS_PER_FULL <= INT32_MAX - 2,
              "Maintenance refresh count must fit a positive int32_t with two reserved partials");
static_assert(BOARD_DISPLAY_FULL_REFRESH_MIN_INTERVAL_MS >= 0,
              "Maintenance full-refresh interval must not be negative");
static constexpr uint32_t kPartialsPerFull = BOARD_DISPLAY_PARTIALS_PER_FULL;

struct ReaderRippleRequest {
    lv_obj_t* screen = nullptr;
    int direction = 0, orient = 0, left = 0, top = 0, width = 0, height = 0;
};

static ReaderRippleRequest s_reader_ripple;

void EpdRequestReaderRipple(lv_obj_t* screen, int direction, int left, int top, int width, int height) {
    s_reader_ripple = {screen, direction, DisplayUiGetOrient(), left, top, width, height};
}

void EpdCancelReaderRipple() {
    s_reader_ripple.direction = 0;
}

struct EpdFlushCtx {
    uint8_t* ripple_fb = nullptr;
    bool ripple_final_pending = false;
    lv_display_t* disp = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    SemaphoreHandle_t done_sem = nullptr;
    uint8_t* last_fb = nullptr;
    uint8_t* work_fb = nullptr;
    uint8_t* gray_scratch = nullptr;  // Overlay 单平面 scratch（LSB/MSB 分两次打包）
    size_t fb_size = 0;
    int panel_w = 0;
    int panel_h = 0;
    esp_err_t last_error = ESP_OK;  // 最近刷新结果，UI锁保护
    bool has_last = false;
    uint32_t partial_refresh_count = 0;
    int64_t last_full_refresh_us = 0;  // 最近一次成功全刷的完成时刻，由显示锁保护
    bool defer_boot_paint = true;      // SetupUI 完成前只攒帧不上屏
    bool boot_fb_ready = false;
    bool defer_standby_paint = false;  // 进待机 settle：只攒帧，Park 再全刷
    volatile bool suppress_lvgl_notify = false;
    volatile bool freeze_updates = false;   // 关机画已落墨 / 待机 park：丢弃后续 LVGL flush
    bool standby_frozen = false;            // 待机冻结 flush（无 Deep Sleep）
    bool force_next_full = false;           // 下一帧强制全刷（退待机后画回底层）
    bool force_full_c7 = false;             // 强制全刷走 STANDBY 枚举（进/出待机）；否则 FULL（二者均为 0xC7）
    bool force_dual_plane_partial = false;  // 下一帧 0x26=旧 0x24=新 局刷（AA 开/关 seed）
    bool refresh_inflight = false;          // 局刷已启动、BUSY 尚未结束：flush 已提前 flush_ready，LVGL 可继续读触摸。
                                            // 下次写屏前 Drain（不等 Sync）。
    bool coalesce_pending = false;          // BUSY 时合并脏帧，flush 立刻返回好让 indev 跑
    TaskHandle_t coalesce_task = nullptr;
};

// 仅周期清残影受最短间隔约束，首帧、待机和失败恢复仍可强制全刷。
static bool IsMaintenanceRefreshDue(const EpdFlushCtx* ctx, uint32_t reserved_partials = 0) {
    const bool count_due = ctx->partial_refresh_count >= kPartialsPerFull ||
                           reserved_partials >= kPartialsPerFull - ctx->partial_refresh_count;
    return count_due &&
           (esp_timer_get_time() - ctx->last_full_refresh_us) / 1000 >= BOARD_DISPLAY_FULL_REFRESH_MIN_INTERVAL_MS;
}

static EpdFlushCtx* s_refresh_ctx = nullptr;
static std::atomic<uint32_t> s_submitted{0};
static std::atomic<uint32_t> s_completed{0};
static void (*s_after_submit)() = nullptr;
static EpdEffect s_reader_effect = EpdEffect::None;

uint8_t* EpdRenderBuffer() {
    return s_refresh_ctx ? s_refresh_ctx->last_fb : nullptr;
}

static bool TryFinishInflightRefresh(EpdFlushCtx* ctx) {
    if (ctx == nullptr || !ctx->refresh_inflight) {
        return true;
    }
    if (xSemaphoreTake(ctx->done_sem, 0) != pdTRUE) {
        return false;
    }
    ctx->refresh_inflight = false;
    ctx->suppress_lvgl_notify = false;
    return true;
}

EpdRefreshStats EpdGetStats() {
    // BUSY 完成只投递信号；读取状态时收尾，避免无后续脏帧时导航一直等待。
    TryFinishInflightRefresh(s_refresh_ctx);
    const bool busy = s_refresh_ctx && (s_refresh_ctx->refresh_inflight || s_refresh_ctx->coalesce_pending ||
                                        s_refresh_ctx->ripple_final_pending);
    if (!busy) {
        s_completed.store(s_submitted.load());
    }
    EpdRefreshStats stats{};
    stats.submitted = s_submitted.load();
    stats.completed = s_completed.load();
    stats.busy = busy;
    stats.last_error = s_refresh_ctx ? s_refresh_ctx->last_error : ESP_ERR_INVALID_STATE;
    return stats;
}

bool EpdTryBeginUiRender() {
    return !EpdGetStats().busy;
}

// SPI 后端的渲染与合帧共用 LVGL 锁，不需要独立的渲染解锁。
void EpdEndUiRender() {}

void EpdCancelPageTurn() {
    EpdCancelReaderRipple();
}

void EpdSetReaderEffect(EpdEffect effect) {
    s_reader_effect = effect == EpdEffect::Ripple ? effect : EpdEffect::None;
}

void EpdRequestPageTurn(int direction, const EpdPageTurnArea& area) {
    if (s_reader_effect == EpdEffect::Ripple) {
        EpdRequestReaderRipple(lv_screen_active(), direction, area.x, area.y, area.width, area.height);
    }
}

void EpdRequestScreenChange(bool /*full_gc*/) {
    // SSD1677 直接提交新页面；导航不额外触发全刷或推白过渡。
}

void EpdRequestBlackEqualization() {
    if (auto* display = LvAdapterSsd1677::Instance()) {
        display->RequestDualPlanePartial();
    }
}

void EpdAfterNextSubmit(void (*callback)()) {
    s_after_submit = callback;
}

void EpdSetUiScrolling(bool scrolling) {
    if (!scrolling) {
        EpdRequestUiScrollSettle();
    }
}

void EpdRequestUiScrollSettle() {
    if (auto* display = LvAdapterSsd1677::Instance()) {
        display->RequestFullOnNextCommit();
    }
}

static void EpdAreaRounder(lv_area_t* area, void* user_data) {
    (void)user_data;
    // 扩 invalidate：让 LVGL 把旧字/AA 边缘也重绘成白，局刷 CURR 才有黑→白差分
    area->x1 -= kPartialDirtyPadPx;
    area->y1 -= kPartialDirtyPadPx / 2;
    area->x2 += kPartialDirtyPadPx;
    area->y2 += kPartialDirtyPadPx / 2;
    if (area->x1 < 0) {
        area->x1 = 0;
    }
    if (area->y1 < 0) {
        area->y1 = 0;
    }
    const int log_w = DisplayUiLogW();
    const int log_h = DisplayUiLogH();
    if (log_w > 0 && area->x2 >= log_w) {
        area->x2 = log_w - 1;
    }
    if (log_h > 0 && area->y2 >= log_h) {
        area->y2 = log_h - 1;
    }
    area->x1 = (area->x1 >> 3) << 3;
    area->x2 = ((area->x2 >> 3) << 3) + 7;
    if (log_w > 0 && area->x2 >= log_w) {
        area->x2 = log_w - 1;
    }
}

static bool EpdRefreshDoneCb(const esp_lcd_panel_handle_t handle, const void* edata, void* user_data) {
    (void)handle;
    (void)edata;
    auto* ctx = static_cast<EpdFlushCtx*>(user_data);
    if (!ctx) {
        return false;
    }

    if (ctx->suppress_lvgl_notify) {
        BaseType_t hp_task = pdFALSE;
        if (ctx->done_sem) {
            xSemaphoreGiveFromISR(ctx->done_sem, &hp_task);
        }
        if (ctx->coalesce_task) {
            vTaskNotifyGiveFromISR(ctx->coalesce_task, &hp_task);
        }
        if (hp_task == pdTRUE) {
            portYIELD_FROM_ISR();
        }
        return false;
    }

    return esp_lv_adapter_display_notify_frame_done_from_isr(ctx->disp);
}

static void AlignArea(int x_start, int y_start, int x_end, int y_end, int* x_draw, int* y_draw, int* w_draw,
                      int* h_draw) {
    const int len_x = x_end - x_start;
    const int len_y = y_end - y_start;
    *x_draw = x_start - (x_start % 8);
    *y_draw = y_start;
    *w_draw = ((len_x + 7) / 8) * 8;
    *h_draw = len_y;
}

static void OverlayDirty(uint8_t* dst, const uint8_t* color_map, int panel_w, int panel_h, int x_draw, int y_draw,
                         int w_draw, int h_draw) {
    const int log_w = DisplayUiLogW();
    const int src_stride = (log_w + 7) / 8;
    const int dst_stride = panel_w / 8;
    if (s_wallpaper_notice_clip) {
        const int right = std::min<int>(x_draw + w_draw, s_wallpaper_notice_clip->x2 + 1);
        const int bottom = std::min<int>(y_draw + h_draw, s_wallpaper_notice_clip->y2 + 1);
        x_draw = std::max<int>(x_draw, s_wallpaper_notice_clip->x1);
        y_draw = std::max<int>(y_draw, s_wallpaper_notice_clip->y1);
        w_draw = right - x_draw;
        h_draw = bottom - y_draw;
    }
    int origin_x, origin_y;
    DisplayUiLogicalToPanel(0, 0, &origin_x, &origin_y);
    if (!s_wallpaper_notice_clip && DisplayUiGetOrient() == kDisplayUiPortrait && origin_x == 0 &&
        origin_y == panel_h - 1) {
        EpdOverlayPortrait(dst, color_map, panel_w, panel_h, x_draw, y_draw, w_draw, h_draw);
        return;
    }
    if (!s_wallpaper_notice_clip && DisplayUiGetOrient() == kDisplayUiLandLeft && src_stride == dst_stride &&
        origin_x == 0 && origin_y == 0) {
        const int row_bytes = w_draw / 8;
        for (int row = 0; row < h_draw; row++) {
            const int y = y_draw + row;
            memcpy(dst + y * dst_stride + x_draw / 8, color_map + y * src_stride + x_draw / 8,
                   static_cast<size_t>(row_bytes));
        }
        return;
    }
    for (int ly = y_draw; ly < y_draw + h_draw; ly++) {
        for (int lx = x_draw; lx < x_draw + w_draw; lx++) {
            const bool white = (color_map[ly * src_stride + (lx >> 3)] & static_cast<uint8_t>(0x80 >> (lx & 7))) != 0;
            int px = 0;
            int py = 0;
            DisplayUiLogicalToPanel(lx, ly, &px, &py);
            if (px < 0 || py < 0 || px >= panel_w || py >= panel_h) {
                continue;
            }
            uint8_t* byte = &dst[py * dst_stride + (px >> 3)];
            const uint8_t mask = static_cast<uint8_t>(0x80 >> (px & 7));
            if (white) {
                *byte |= mask;
            } else {
                *byte = static_cast<uint8_t>(*byte & ~mask);
            }
        }
    }
}

static esp_err_t WaitRefreshDone(EpdFlushCtx* ctx, const char* tag) {
    if (xSemaphoreTake(ctx->done_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "%s timeout", tag);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

// 局刷：prepare → 整帧只写 0x24=新图（不写 0x26）→ 0xCF。依赖面板里 0x26 仍是上一帧。
static esp_err_t DrawPartialFrame(EpdFlushCtx* ctx, esp_lcd_panel_handle_t panel, const uint8_t* curr_fb) {
    if (ctx == nullptr || panel == nullptr || curr_fb == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    // curr_fb 在 PSRAM：draw_bitmap 内再拷到内部 DMA bounce，勿再经 tx_* 中转
    ESP_RETURN_ON_ERROR(epaper_panel_prepare_for_partial(panel), TAG, "partial prepare");
    epaper_panel_set_bitmap_color(panel, SSD1677_EPAPER_BITMAP_CURRENT);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(panel, 0, 0, ctx->panel_w, ctx->panel_h, curr_fb), TAG,
                        "partial curr 0x24");
    epaper_panel_set_refresh_mode(panel, SSD1677_EPAPER_REFRESH_PARTIAL);
    return epaper_panel_refresh_screen(panel);
}

// 写 0x26=prev、0x24=curr（不激活）。
static esp_err_t WriteBothPlanes(EpdFlushCtx* ctx, const uint8_t* prev_fb, const uint8_t* curr_fb,
                                 const char* wait_tag) {
    if (ctx == nullptr || ctx->panel == nullptr || prev_fb == nullptr || curr_fb == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    epaper_panel_set_bitmap_color(ctx->panel, SSD1677_EPAPER_BITMAP_PREVIOUS);
    esp_err_t err = esp_lcd_panel_draw_bitmap(ctx->panel, 0, 0, ctx->panel_w, ctx->panel_h, prev_fb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s 0x26: %s", wait_tag, esp_err_to_name(err));
        return err;
    }
    epaper_panel_set_bitmap_color(ctx->panel, SSD1677_EPAPER_BITMAP_CURRENT);
    err = esp_lcd_panel_draw_bitmap(ctx->panel, 0, 0, ctx->panel_w, ctx->panel_h, curr_fb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s 0x24: %s", wait_tag, esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

// AA 用差分局刷底：显式 0x26=旧、0x24=新 → partial LUT + 0xCF，同步等 BUSY。
static esp_err_t PartialBwBasePrevCurr(EpdFlushCtx* ctx, const uint8_t* prev_fb, const uint8_t* curr_fb,
                                       const char* wait_tag) {
    ESP_RETURN_ON_ERROR(epaper_panel_prepare_for_partial(ctx->panel), TAG, "aa partial prepare");
    esp_err_t err = WriteBothPlanes(ctx, prev_fb, curr_fb, wait_tag);
    if (err != ESP_OK) {
        return err;
    }
    epaper_panel_set_refresh_mode(ctx->panel, SSD1677_EPAPER_REFRESH_PARTIAL);
    err = epaper_panel_refresh_screen(ctx->panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s partial act: %s", wait_tag, esp_err_to_name(err));
        return err;
    }
    return WaitRefreshDone(ctx, wait_tag);
}

// 全刷：0x26/0x24 同写新图；mode 选 FULL/STANDBY（均为 0xC7）。
static esp_err_t FullRefreshBothSame(EpdFlushCtx* ctx, const uint8_t* curr_fb, const char* wait_tag,
                                     esp_lcd_ssd1677_refresh_mode_t mode = SSD1677_EPAPER_REFRESH_FULL) {
    esp_err_t err = WriteBothPlanes(ctx, curr_fb, curr_fb, wait_tag);
    if (err != ESP_OK) {
        return err;
    }
    epaper_panel_set_refresh_mode(ctx->panel, mode);
    err = epaper_panel_refresh_screen(ctx->panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s refresh: %s", wait_tag, esp_err_to_name(err));
        return err;
    }
    ctx->last_full_refresh_us = esp_timer_get_time();
    return ESP_OK;
}

// Overlay 灰步：LSB/MSB 分两次打包同一 scratch → 0xCC → BW 写回双平面。
static esp_err_t GrayOverlayAfterBwBase(EpdFlushCtx* ctx, const char* wait_tag) {
    if (ctx->gray_scratch == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    epd_gray_aa_pack_overlay_plane(ctx->gray_scratch, 0);
    epaper_panel_set_bitmap_color(ctx->panel, SSD1677_EPAPER_BITMAP_CURRENT);
    esp_err_t err = esp_lcd_panel_draw_bitmap(ctx->panel, 0, 0, ctx->panel_w, ctx->panel_h, ctx->gray_scratch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s 0x24 lsb: %s", wait_tag, esp_err_to_name(err));
        return err;
    }
    epd_gray_aa_pack_overlay_plane(ctx->gray_scratch, 1);
    epaper_panel_set_bitmap_color(ctx->panel, SSD1677_EPAPER_BITMAP_PREVIOUS);
    err = esp_lcd_panel_draw_bitmap(ctx->panel, 0, 0, ctx->panel_w, ctx->panel_h, ctx->gray_scratch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s 0x26 msb: %s", wait_tag, esp_err_to_name(err));
        return err;
    }

    epaper_panel_set_refresh_mode(ctx->panel, SSD1677_EPAPER_REFRESH_GRAY);
    err = epaper_panel_refresh_screen(ctx->panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s gray refresh: %s", wait_tag, esp_err_to_name(err));
        return err;
    }

    // cleanup：面板 RAM 回到 BW，下一帧 0x26 才有正确「旧值」
    WriteBothPlanes(ctx, ctx->work_fb, ctx->work_fb, wait_tag);
    return ESP_OK;
}

// 阅读抗锯齿：
// - force_full_base / 无 last：C4 铺 BW 底（进阅读首帧、周期清残影）
// - 否则：0x26=last 0x24=work 局刷换底 → Overlay 灰 0xCC
static esp_err_t GrayRefreshFromWorkFb(EpdFlushCtx* ctx, const char* wait_tag, bool force_full_base) {
    if (ctx == nullptr || ctx->panel == nullptr || ctx->work_fb == nullptr || ctx->gray_scratch == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const bool use_full_base = force_full_base || !ctx->has_last || ctx->last_fb == nullptr;
    esp_err_t err;
    if (use_full_base) {
        ESP_LOGD(TAG, "%s BW base FULL 0xC7", wait_tag);
        err = FullRefreshBothSame(ctx, ctx->work_fb, wait_tag, SSD1677_EPAPER_REFRESH_FULL);
    } else {
        ESP_LOGD(TAG, "%s BW base PARTIAL 0x26=old 0x24=new", wait_tag);
        err = PartialBwBasePrevCurr(ctx, ctx->last_fb, ctx->work_fb, wait_tag);
    }
    if (err != ESP_OK) {
        return err;
    }

    err = GrayOverlayAfterBwBase(ctx, wait_tag);
    if (err != ESP_OK) {
        return err;
    }

    // Overlay 末已 WriteBothPlanes(work,work) 重铺 0x26/0x24，退出 AA 后可直接局刷，勿再逼 C4
    if (!use_full_base) {
        ctx->partial_refresh_count++;
    } else {
        ctx->partial_refresh_count = 0;
    }
    ESP_LOGD(TAG, "%s GRAY overlay+0xCC done (base=%s)", wait_tag, use_full_base ? "FULL" : "PARTIAL");
    return ESP_OK;
}

static bool WantReaderGrayAa(void) {
    // LVGL 任务栈在 PSRAM，禁止 EnsureLoaded/NVS（会 cache_utils assert）。
    // epd_i1_glyph_aa_enabled 仅在正文沉浸态由 BookReaderPrefsSyncGlyphAa 置位。
    return epd_i1_glyph_aa_enabled != 0;
}

static void OnLvglRefrStart(lv_event_t* e) {
    ++s_submitted;
    // Gray AA rebuilds its entire mask each frame. Keep full rendering there
    // until that mask can preserve unchanged glyphs across dirty rectangles.
    static bool gray_full_mode = false;
    auto* disp = static_cast<lv_display_t*>(lv_event_get_target(e));
    if (WantReaderGrayAa()) {
        if (!gray_full_mode) {
            lv_display_set_render_mode(disp, LV_DISPLAY_RENDER_MODE_FULL);
            lv_obj_invalidate(lv_screen_active());
            gray_full_mode = true;
        }
    } else if (gray_full_mode) {
        lv_display_set_render_mode(disp, LV_DISPLAY_RENDER_MODE_DIRECT);
        gray_full_mode = false;
    }
    if (WantReaderGrayAa()) {
        epd_gray_aa_begin_frame(DisplayUiLogicalToPanel);
    }
}

// 排空上一次异步局刷（等 BUSY，不做 Sync）。
static esp_err_t DrainRefreshIfNeeded(EpdFlushCtx* ctx) {
    if (ctx == nullptr || !ctx->refresh_inflight) {
        return ESP_OK;
    }
    const auto result = WaitRefreshDone(ctx, "drain");
    if (result != ESP_OK) {
        ctx->last_error = result;
        ctx->force_next_full = true;
        return result;
    }
    ctx->refresh_inflight = false;
    ctx->suppress_lvgl_notify = false;
    return ESP_OK;
}

// 非阻塞：BUSY 未结束返回 false（调用方 coalesce + flush_ready，勿堵 indev）。

static void NotifyCoalescedFrame(EpdFlushCtx* ctx);

// Called after BUSY completes, while holding the adapter lock.
static bool TryStartReaderRipple(EpdFlushCtx* ctx) {
    const auto request = s_reader_ripple;
    s_reader_ripple.direction = 0;
    if (!request.direction || ctx->ripple_final_pending || !ctx->has_last || request.screen != lv_screen_active() ||
        request.orient != DisplayUiGetOrient() || WantReaderGrayAa() || ctx->force_next_full ||
        ctx->force_dual_plane_partial || IsMaintenanceRefreshDue(ctx, 2) || ctx->freeze_updates ||
        ctx->defer_standby_paint || ctx->defer_boot_paint || TouchUiFingerIsDown() || TouchUiHasPendingEdges() ||
        request.width <= 0 || request.height <= 0) {
        return false;
    }
    if (!ctx->ripple_fb) {
        ctx->ripple_fb = static_cast<uint8_t*>(heap_caps_malloc(ctx->fb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (!ctx->ripple_fb) {
        return false;
    }
    EpdBuildReaderRippleI1(ctx->ripple_fb, ctx->last_fb, ctx->work_fb, ctx->panel_w, ctx->panel_h, request.orient,
                           request.direction, request.left, request.top, request.width, request.height);
    ctx->suppress_lvgl_notify = true;
    xSemaphoreTake(ctx->done_sem, 0);
    const esp_err_t err = DrawPartialFrame(ctx, ctx->panel, ctx->ripple_fb);
    if (err != ESP_OK) {
        // A failed intermediate update has an uncertain baseline: recover with
        // a final-page full refresh rather than diffing against a guessed frame.
        const esp_err_t recovered = FullRefreshBothSame(ctx, ctx->work_fb, "ripple_recover");
        ctx->suppress_lvgl_notify = false;
        ctx->refresh_inflight = false;
        ctx->coalesce_pending = false;
        ctx->force_next_full = recovered != ESP_OK;
        if (recovered != ESP_OK) {
            ctx->coalesce_pending = true;
            NotifyCoalescedFrame(ctx);
        }
        if (recovered == ESP_OK) {
            memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
            ctx->partial_refresh_count = 0;
        }
        ESP_LOGW(TAG, "ripple intermediate failed: %s", esp_err_to_name(err));
        return true;
    }
    memcpy(ctx->last_fb, ctx->ripple_fb, ctx->fb_size);
    ctx->refresh_inflight = true;
    ctx->partial_refresh_count++;
    ctx->ripple_final_pending = true;
    ctx->coalesce_pending = true;
    NotifyCoalescedFrame(ctx);
    ESP_LOGD(TAG, "reader ripple intermediate submitted");
    return true;
}

// BUSY 结束后把 coalesce 的 work_fb 局刷上屏（无新 LVGL flush 时补一刀）。
static void CommitCoalescedFrame(EpdFlushCtx* ctx) {
    if (ctx == nullptr || !ctx->coalesce_pending || ctx->freeze_updates || ctx->defer_standby_paint) {
        return;
    }
    if (!TryFinishInflightRefresh(ctx)) {
        return;
    }
    if (!ctx->has_last || ctx->work_fb == nullptr || ctx->last_fb == nullptr || ctx->panel == nullptr) {
        ctx->coalesce_pending = false;
        return;
    }

    if (TryStartReaderRipple(ctx)) {
        return;
    }
    ctx->ripple_final_pending = false;

    ctx->suppress_lvgl_notify = true;
    xSemaphoreTake(ctx->done_sem, 0);
    esp_err_t err;
    const bool full_refresh = ctx->force_next_full || IsMaintenanceRefreshDue(ctx);
    const bool synchronous = WantReaderGrayAa() || full_refresh || ctx->force_dual_plane_partial;
    if (WantReaderGrayAa()) {
        err = GrayRefreshFromWorkFb(ctx, "coalesce_gray", !ctx->force_dual_plane_partial && full_refresh);
    } else if (full_refresh) {
        err = FullRefreshBothSame(ctx, ctx->work_fb, "coalesce_full");
        ctx->partial_refresh_count = 0;
    } else if (ctx->force_dual_plane_partial) {
        err = PartialBwBasePrevCurr(ctx, ctx->last_fb, ctx->work_fb, "coalesce_dual");
    } else {
        err = DrawPartialFrame(ctx, ctx->panel, ctx->work_fb);
    }
    ctx->last_error = err;
    ctx->force_next_full = false;
    ctx->force_dual_plane_partial = false;
    if (err != ESP_OK) {
        ctx->suppress_lvgl_notify = false;
        ctx->force_next_full = true;
        ctx->coalesce_pending = true;
        NotifyCoalescedFrame(ctx);
        ESP_LOGW(TAG, "coalesce commit: %s", esp_err_to_name(err));
        return;
    }

    ctx->refresh_inflight = !synchronous;
    if (synchronous) {
        ctx->suppress_lvgl_notify = false;
    }
    if (!WantReaderGrayAa() && !full_refresh) {
        ctx->partial_refresh_count++;
    }
    memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
    ctx->coalesce_pending = false;
    ESP_LOGD(TAG, "coalesce commit after busy");
}

static void CoalesceTask(void* arg) {
    auto* ctx = static_cast<EpdFlushCtx*>(arg);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // All framebuffer and LVGL access uses the same recursive adapter mutex.
        // Never take that mutex or call LVGL/SPI from the BUSY ISR.
        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            continue;
        }
        CommitCoalescedFrame(ctx);
        const bool retry =
            ctx->coalesce_pending && !ctx->refresh_inflight && !ctx->freeze_updates && !ctx->defer_standby_paint;
        esp_lv_adapter_unlock();
        // Only a failed panel transaction retries; normal BUSY waits are events.
        if (retry) {
            vTaskDelay(pdMS_TO_TICKS(50));
            xTaskNotifyGive(ctx->coalesce_task);
        }
    }
}

static void NotifyCoalescedFrame(EpdFlushCtx* ctx) {
    if (ctx == nullptr || ctx->freeze_updates || ctx->defer_standby_paint) {
        return;
    }
    if (!ctx->coalesce_pending) {
        return;
    }
    // Notify even if BUSY has already fallen: this closes the arm/ISR race.
    if (ctx->coalesce_task) {
        xTaskNotifyGive(ctx->coalesce_task);
    }
}

// A2I1 逻辑竖屏 → 面板 HTILED（与 adapter ROTATE_270：px=ly, py=ph-1-lx）。
// 超出逻辑屏（panel_h × panel_w，即 480×800）时按 DecodeA2i1ToL8 同款 Fit 等比缩小后居中；
// 未超屏则保持原左上角贴齐行为，不放大。
static void BlitA2i1ToPanelFb(uint8_t* dst_fb, int panel_w, int panel_h, const uint8_t* a2i1_payload, uint16_t log_w,
                              uint16_t log_h, uint16_t src_stride) {
    const uint8_t* src = a2i1_payload + 8; /* skip LVGL palette */
    const int dst_stride = panel_w / 8;
    memset(dst_fb, kLvglWhite, static_cast<size_t>(dst_stride) * static_cast<size_t>(panel_h));
    if (a2i1_payload == nullptr || log_w == 0 || log_h == 0 || src_stride == 0 || panel_w <= 0 || panel_h <= 0) {
        return;
    }

    // ROTATE_270：逻辑宽=panel_h，逻辑高=panel_w（勿与物理 800×480 混淆）。
    const int screen_w = panel_h;
    const int screen_h = panel_w;
    const int src_w = static_cast<int>(log_w);
    const int src_h = static_cast<int>(log_h);

    int dst_w = src_w;
    int dst_h = src_h;
    bool scaled = false;
    if (dst_w > screen_w || dst_h > screen_h) {
        // 与 reader::DecodeA2i1ToL8 相同的等比 Fit（取较小缩放比，不变形）。
        const float sx = static_cast<float>(screen_w) / static_cast<float>(dst_w);
        const float sy = static_cast<float>(screen_h) / static_cast<float>(dst_h);
        const float s = sx < sy ? sx : sy;
        dst_w = std::max(1, static_cast<int>(dst_w * s));
        dst_h = std::max(1, static_cast<int>(dst_h * s));
        scaled = true;
    }

    // 缩放后居中；未缩放保持历史左上角贴齐。
    const int ox = scaled ? (screen_w - dst_w) / 2 : 0;
    const int oy = scaled ? (screen_h - dst_h) / 2 : 0;
    if (scaled) {
        ESP_LOGI(TAG, "shutdown blit fit %dx%d -> %dx%d center=(%d,%d) screen=%dx%d", src_w, src_h, dst_w, dst_h, ox,
                 oy, screen_w, screen_h);
    }

    for (int y = 0; y < dst_h; y++) {
        const int sy = scaled ? static_cast<int>(int64_t(y) * src_h / dst_h) : y;
        const uint8_t* row = src + static_cast<size_t>(sy) * src_stride;
        for (int x = 0; x < dst_w; x++) {
            const int sx = scaled ? static_cast<int>(int64_t(x) * src_w / dst_w) : x;
            const bool white = (row[sx >> 3] & static_cast<uint8_t>(0x80 >> (sx & 7))) != 0;
            const int lx = ox + x;
            const int ly = oy + y;
            const int px = ly;
            const int py = panel_h - 1 - lx;
            if (px < 0 || py < 0 || px >= panel_w || py >= panel_h) {
                continue;
            }
            uint8_t* byte = &dst_fb[py * dst_stride + (px >> 3)];
            const uint8_t mask = static_cast<uint8_t>(0x80 >> (px & 7));
            if (white) {
                *byte |= mask;
            } else {
                *byte = static_cast<uint8_t>(*byte & ~mask);
            }
        }
    }
}

// 关机：关机图一次全刷（0xC7，与进/出待机相同）。
static esp_err_t EpdShutdownFullFb(EpdFlushCtx* ctx, const uint8_t* curr_fb) {
    if (ctx == nullptr || ctx->panel == nullptr || ctx->last_fb == nullptr || curr_fb == nullptr ||
        ctx->work_fb == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (DrainRefreshIfNeeded(ctx) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    ctx->suppress_lvgl_notify = true;
    xSemaphoreTake(ctx->done_sem, 0);

    ESP_LOGD(TAG, "shutdown: FULL 0xC7");
    esp_err_t err = FullRefreshBothSame(ctx, curr_fb, "shutdown", SSD1677_EPAPER_REFRESH_STANDBY);
    ESP_LOGD(TAG, "shutdown: done err=%s", esp_err_to_name(err));
    ctx->suppress_lvgl_notify = false;
    if (err == ESP_OK) {
        memcpy(ctx->last_fb, curr_fb, ctx->fb_size);
        ctx->has_last = true;
    }
    return err;
}

// GCC 在超大翻译单元内于 EpdLvglDrawBitmap 中展开 ESP_LOGI 可能 ICE，日志单独成函数
static void LogAaOffDualPlanePartialSeed() {
    ESP_LOGD(TAG, "aa_off dual-plane PARTIAL 0x26=old 0x24=new");
}

static esp_err_t EpdLvglDrawBitmapImpl(lv_display_t* disp, esp_lcd_panel_handle_t panel, int x_start, int y_start,
                                       int x_end, int y_end, const void* color_map, void* user_ctx) {
    (void)disp;
    auto* ctx = static_cast<EpdFlushCtx*>(user_ctx);
    if (!ctx || !color_map || !ctx->last_fb || !ctx->work_fb) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->freeze_updates) {
        if (ctx->disp) {
            lv_display_flush_ready(ctx->disp);
        }
        return ESP_OK;
    }

    int x_draw = 0, y_draw = 0, w_draw = 0, h_draw = 0;
    AlignArea(x_start, y_start, x_end, y_end, &x_draw, &y_draw, &w_draw, &h_draw);
    const int log_w = DisplayUiLogW();
    const int log_h = DisplayUiLogH();
    if (w_draw <= 0 || h_draw <= 0 || x_draw < 0 || y_draw < 0 || x_draw + w_draw > log_w || y_draw + h_draw > log_h) {
        ESP_LOGW(TAG, "bad area (%d,%d)-(%d,%d) log=%dx%d", x_start, y_start, x_end, y_end, log_w, log_h);
        return ESP_ERR_INVALID_ARG;
    }

    // 勿在 flush_cb 里 Wait BUSY：否则 LVGL 跑不了 indev，touch_feed 边沿无法交付
    if (!TryFinishInflightRefresh(ctx)) {
        if (!ctx->coalesce_pending) {
            if (ctx->boot_fb_ready || ctx->has_last) {
                memcpy(ctx->work_fb, ctx->last_fb, ctx->fb_size);
            } else {
                memset(ctx->work_fb, kLvglWhite, ctx->fb_size);
            }
        }
        OverlayDirty(ctx->work_fb, static_cast<const uint8_t*>(color_map), ctx->panel_w, ctx->panel_h, x_draw, y_draw,
                     w_draw, h_draw);
        if (ctx->defer_standby_paint) {
            memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
            ctx->coalesce_pending = false;
            lv_display_flush_ready(ctx->disp);
            return ESP_OK;
        }
        ctx->coalesce_pending = true;
        NotifyCoalescedFrame(ctx);
        lv_display_flush_ready(ctx->disp);
        return ESP_OK;
    }

    if (ctx->coalesce_pending) {
        OverlayDirty(ctx->work_fb, static_cast<const uint8_t*>(color_map), ctx->panel_w, ctx->panel_h, x_draw, y_draw,
                     w_draw, h_draw);
    } else if (ctx->boot_fb_ready || ctx->has_last) {
        memcpy(ctx->work_fb, ctx->last_fb, ctx->fb_size);
        OverlayDirty(ctx->work_fb, static_cast<const uint8_t*>(color_map), ctx->panel_w, ctx->panel_h, x_draw, y_draw,
                     w_draw, h_draw);
    } else {
        memset(ctx->work_fb, kLvglWhite, ctx->fb_size);
        OverlayDirty(ctx->work_fb, static_cast<const uint8_t*>(color_map), ctx->panel_w, ctx->panel_h, x_draw, y_draw,
                     w_draw, h_draw);
    }

    // DIRECT can flush multiple rectangles per LVGL frame. Compose them all
    // before submitting once, including boot/standby/AA and reader transitions.
    if (!lv_display_flush_is_last(ctx->disp)) {
        ctx->coalesce_pending = true;
        lv_display_flush_ready(ctx->disp);
        return ESP_OK;
    }
    if (ctx->defer_boot_paint) {
        memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
        ctx->boot_fb_ready = true;
        ctx->coalesce_pending = false;
        lv_display_flush_ready(ctx->disp);
        return ESP_OK;
    }

    if (ctx->defer_standby_paint) {
        memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
        ctx->coalesce_pending = false;
        lv_display_flush_ready(ctx->disp);
        return ESP_OK;
    }

    if (!ctx->has_last) {
        EpdCancelReaderRipple();
        ctx->ripple_final_pending = false;
        ctx->suppress_lvgl_notify = true;
        xSemaphoreTake(ctx->done_sem, 0);
        esp_err_t err = ESP_OK;
        if (WantReaderGrayAa()) {
            err = GrayRefreshFromWorkFb(ctx, "first_paint_gray", true);
        } else {
            err = FullRefreshBothSame(ctx, ctx->work_fb, "first_paint");
        }
        ctx->suppress_lvgl_notify = false;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "first paint: %s", esp_err_to_name(err));
            return err;
        }
        memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
        ctx->has_last = true;
        ctx->partial_refresh_count = 0;
        ctx->coalesce_pending = false;
        lv_display_flush_ready(ctx->disp);
        return ESP_OK;
    }

    // 阅读抗锯齿：局刷换 BW 底 + Overlay 灰；仅强制/周期才 C4 底
    if (WantReaderGrayAa()) {
        // AA 开瞬间：优先双平面局刷底，勿被 force_next_full / 周期 C4 抢走
        EpdCancelReaderRipple();
        ctx->ripple_final_pending = false;
        const bool dual_seed = ctx->force_dual_plane_partial;
        ctx->force_dual_plane_partial = false;
        const bool force_full_base = !dual_seed && (ctx->force_next_full || IsMaintenanceRefreshDue(ctx));
        ctx->force_next_full = false;
        ctx->force_full_c7 = false;
        ctx->coalesce_pending = false;
        if (DrainRefreshIfNeeded(ctx) != ESP_OK) {
            return ESP_ERR_TIMEOUT;
        }
        ctx->suppress_lvgl_notify = true;
        xSemaphoreTake(ctx->done_sem, 0);
        esp_err_t err = GrayRefreshFromWorkFb(ctx, dual_seed ? "aa_on_gray" : "reader_gray", force_full_base);
        ctx->suppress_lvgl_notify = false;
        ctx->refresh_inflight = false;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "reader gray: %s", esp_err_to_name(err));
            return err;
        }
        memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
        lv_display_flush_ready(ctx->disp);
        return ESP_OK;
    }

    // AA 关瞬间：双平面局刷 seed 0x26/0x24（无灰 Overlay），再回日常只写 0x24
    if (ctx->force_dual_plane_partial) {
        EpdCancelReaderRipple();
        ctx->ripple_final_pending = false;
        ctx->force_dual_plane_partial = false;
        ctx->force_next_full = false;
        ctx->force_full_c7 = false;
        ctx->coalesce_pending = false;
        if (DrainRefreshIfNeeded(ctx) != ESP_OK) {
            return ESP_ERR_TIMEOUT;
        }
        ctx->suppress_lvgl_notify = true;
        xSemaphoreTake(ctx->done_sem, 0);
        LogAaOffDualPlanePartialSeed();
        esp_err_t err = PartialBwBasePrevCurr(ctx, ctx->last_fb, ctx->work_fb, "aa_off_seed");
        ctx->suppress_lvgl_notify = false;
        ctx->refresh_inflight = false;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "aa_off seed: %s", esp_err_to_name(err));
            return err;
        }
        memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
        ctx->partial_refresh_count++;
        lv_display_flush_ready(ctx->disp);
        return ESP_OK;
    }

    // 退待机后首帧 / 周期或其它强制全刷（FULL 与 STANDBY 均为 0xC7）
    if (ctx->force_next_full || IsMaintenanceRefreshDue(ctx)) {
        EpdCancelReaderRipple();
        ctx->ripple_final_pending = false;
        const bool from_force = ctx->force_next_full;
        const bool use_c7 = from_force && ctx->force_full_c7;
        ctx->force_next_full = false;
        ctx->force_full_c7 = false;
        ctx->coalesce_pending = false;
        ctx->suppress_lvgl_notify = true;
        xSemaphoreTake(ctx->done_sem, 0);
        const auto mode = use_c7 ? SSD1677_EPAPER_REFRESH_STANDBY : SSD1677_EPAPER_REFRESH_FULL;
        ESP_LOGD(TAG, "%s FULL 0x22=%02X", from_force ? "forced" : "periodic",
                 use_c7 ? SSD1677_PARAM_DISP_UPDATE_FULL_STANDBY : SSD1677_PARAM_DISP_UPDATE_FULL);
        esp_err_t err = FullRefreshBothSame(ctx, ctx->work_fb, from_force ? "forced_full" : "periodic_full", mode);
        ctx->suppress_lvgl_notify = false;
        ctx->refresh_inflight = false;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "full: %s", esp_err_to_name(err));
            return err;
        }
        memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
        ctx->partial_refresh_count = 0;
        lv_display_flush_ready(ctx->disp);
        return ESP_OK;
    }

    if (TryStartReaderRipple(ctx)) {
        lv_display_flush_ready(ctx->disp);
        return ESP_OK;
    }
    ctx->ripple_final_pending = false;
    if (ctx->coalesce_pending) {
        ctx->coalesce_pending = false;
    } else {
        // VRAM writes still use the full frame; only changed/unchanged matters.
        if (std::memcmp(ctx->last_fb, ctx->work_fb, ctx->fb_size) == 0) {
            lv_display_flush_ready(ctx->disp);
            return ESP_OK;
        }
    }

    ctx->suppress_lvgl_notify = true;
    xSemaphoreTake(ctx->done_sem, 0);

    // 局刷：整帧 0x24=新（不写 0x26）
    ESP_LOGD(TAG, "PARTIAL full-frame 0x24 #%u", ctx->partial_refresh_count + 1);
    {
        const esp_err_t err = DrawPartialFrame(ctx, panel, ctx->work_fb);
        if (err != ESP_OK) {
            ctx->suppress_lvgl_notify = false;
            ctx->force_next_full = true;
            ctx->coalesce_pending = true;
            NotifyCoalescedFrame(ctx);
            ESP_LOGE(TAG, "partial: %s", esp_err_to_name(err));
            return err;
        }
    }

    ctx->refresh_inflight = true;
    ctx->partial_refresh_count++;

    memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
    lv_display_flush_ready(ctx->disp);
    return ESP_OK;
}

static esp_err_t EpdLvglDrawBitmap(lv_display_t* disp, esp_lcd_panel_handle_t panel, int x_start, int y_start,
                                   int x_end, int y_end, const void* color_map, void* user_ctx) {
    const auto result = EpdLvglDrawBitmapImpl(disp, panel, x_start, y_start, x_end, y_end, color_map, user_ctx);
    auto* ctx = static_cast<EpdFlushCtx*>(user_ctx);
    if (ctx) {
        ctx->last_error = result;
        if (result != ESP_OK) {
            ctx->force_next_full = true;
        }
    }
    return result;
}

// SetupUI 末尾：首页 FULL（0x24=0x26）；之后局刷只写整帧 0x24。
static void EpdCommitBootBaseline(EpdFlushCtx* ctx) {
    if (ctx == nullptr || !ctx->defer_boot_paint) {
        return;
    }
    ctx->defer_boot_paint = false;
    if (!ctx->boot_fb_ready || ctx->last_fb == nullptr || ctx->panel == nullptr) {
        ESP_LOGW(TAG, "boot paint skip (no fb)");
        return;
    }
    ctx->suppress_lvgl_notify = true;
    xSemaphoreTake(ctx->done_sem, 0);
    esp_err_t err = FullRefreshBothSame(ctx, ctx->last_fb, "boot");
    if (err == ESP_OK) {
        ctx->has_last = true;
        ctx->partial_refresh_count = 0;
    } else {
        ESP_LOGE(TAG, "boot paint: %s", esp_err_to_name(err));
    }
    ctx->suppress_lvgl_notify = false;
}

// 非叠图待机壁纸：复用 last_fb。先整帧填白，再按逻辑坐标把已缩放的 L8 居中写入。
// 1bpp 与 OverlayDirty 相同：1=白，像素 0 在 bit7。不另申请缓冲。
static bool BlitStandbyL8FullScreen(EpdFlushCtx* ctx, const RasterImage& img) {
    if (ctx == nullptr || ctx->last_fb == nullptr || img.empty() ||
        img.pixels.size() < size_t(img.width) * img.height) {
        return false;
    }
    memset(ctx->last_fb, kLvglWhite, ctx->fb_size);
    const int max_w = DisplayUiLogW();
    const int max_h = DisplayUiLogH();
    int draw_w = 0;
    int draw_h = 0;
    if (!FitImageSize(img.width, img.height, max_w, max_h, &draw_w, &draw_h)) {
        return false;
    }
    const int left = (max_w - draw_w) / 2;
    const int top = (max_h - draw_h) / 2;
    const bool same_size = draw_w == img.width && draw_h == img.height;
    const int stride = ctx->panel_w / 8;
    for (int ly = 0; ly < draw_h; ++ly) {
        const int sy = same_size ? ly : static_cast<int>(int64_t(ly) * img.height / draw_h);
        const uint8_t* row = img.pixels.data() + static_cast<size_t>(sy) * img.width;
        for (int lx = 0; lx < draw_w; ++lx) {
            const int sx = same_size ? lx : static_cast<int>(int64_t(lx) * img.width / draw_w);
            int px = 0;
            int py = 0;
            DisplayUiLogicalToPanel(left + lx, top + ly, &px, &py);
            if (px < 0 || py < 0 || px >= ctx->panel_w || py >= ctx->panel_h) {
                continue;
            }
            uint8_t* byte = &ctx->last_fb[static_cast<size_t>(py) * stride + (px >> 3)];
            const uint8_t mask = static_cast<uint8_t>(0x80 >> (px & 7));
            if (DarkenImagePixel(row[sx]) >= 128) {
                *byte |= mask;
            } else {
                *byte = static_cast<uint8_t>(*byte & ~mask);
            }
        }
        if ((ly & 63) == 0) {
            taskYIELD();
        }
    }
    ESP_LOGI(TAG, "standby wallpaper framebuffer: logical=%dx%d image=%ux%u draw=%dx%d at=(%d,%d)", max_w, max_h,
             static_cast<unsigned>(img.width), static_cast<unsigned>(img.height), draw_w, draw_h, left, top);
    return true;
}

static void ShowWallpaperLoading(EpdFlushCtx* ctx) {
    if (!ctx || DrainRefreshIfNeeded(ctx) != ESP_OK || !WallpaperFrame_Capture()) {
        ESP_LOGW(TAG, "wallpaper loading notice unavailable");
        return;
    }
    auto* notice = ScreenCreateToast(lv_layer_top(), LangString_WALLPAPER_ENTER_LOADING);
    lv_obj_update_layout(notice);
    lv_area_t clip;
    lv_obj_get_coords(notice, &clip);
    const bool deferred = ctx->defer_standby_paint;
    const bool frozen = ctx->freeze_updates;
    ctx->defer_standby_paint = true;
    ctx->freeze_updates = false;
    s_wallpaper_notice_clip = &clip;
    lv_obj_invalidate(notice);
    lv_refr_now(ctx->disp);
    s_wallpaper_notice_clip = nullptr;
    ctx->defer_standby_paint = deferred;
    ctx->freeze_updates = frozen;
    lv_obj_delete(notice);
    ctx->suppress_lvgl_notify = true;
    xSemaphoreTake(ctx->done_sem, 0);
    esp_err_t result = FullRefreshBothSame(ctx, ctx->last_fb, "wallpaper_loading");
    if (result == ESP_OK) {
        result = WaitRefreshDone(ctx, "wallpaper_loading");
    }
    ctx->suppress_lvgl_notify = false;
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "wallpaper loading notice refresh failed: code=%d", static_cast<int>(result));
    }
}

LvAdapterSsd1677::LvAdapterSsd1677(const esp_lcd_panel_handle_t panel, const esp_lcd_panel_io_handle_t panel_io,
                                   const esp_lcd_touch_handle_t touch_handle, const int width, const int height) {
    instance_ = this;
    width_ = width;
    height_ = height;
    if (touch_handle != nullptr) {
        s_touch_tp = touch_handle;
    }

    esp_timer_create_args_t notification_timer_args = {
        .callback =
            [](void* arg) {
                auto* display = static_cast<LvAdapterSsd1677*>(arg);
                DisplayLockGuard lock(display);
                if (display->notification_label_) {
                    lv_obj_add_flag(display->notification_label_, LV_OBJ_FLAG_HIDDEN);
                }
                if (display->status_label_ && !DisplayUiWaveVisible()) {
                    lv_obj_remove_flag(display->status_label_, LV_OBJ_FLAG_HIDDEN);
                }
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "epd_notif_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&notification_timer_args, &notification_timer_));

    epd_flush_ctx_ = new EpdFlushCtx();
    s_refresh_ctx = epd_flush_ctx_;
    epd_flush_ctx_->panel = panel;
    epd_flush_ctx_->panel_w = SSD1677_PANEL_WIDTH;
    epd_flush_ctx_->panel_h = SSD1677_PANEL_HEIGHT;
    epd_flush_ctx_->fb_size = SSD1677_PANEL_BUFFER_SIZE;
    epd_flush_ctx_->done_sem = xSemaphoreCreateBinary();

    // last/work 在 PSRAM（仅 CPU 读写）。发屏由面板侧拷到内部 DMA bounce，
    // 避免 PSRAM 直 DMA 的 cache 不一致与鬼影。内部只常驻 1×48KB bounce。
    const uint32_t psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    epd_flush_ctx_->last_fb = static_cast<uint8_t*>(heap_caps_malloc(epd_flush_ctx_->fb_size, psram));
    epd_flush_ctx_->work_fb = static_cast<uint8_t*>(heap_caps_malloc(epd_flush_ctx_->fb_size, psram));
    epd_flush_ctx_->gray_scratch = static_cast<uint8_t*>(heap_caps_malloc(epd_flush_ctx_->fb_size, psram));

    if (epd_flush_ctx_->last_fb) {
        memset(epd_flush_ctx_->last_fb, kLvglWhite, epd_flush_ctx_->fb_size);
    }

    if (!epd_flush_ctx_->done_sem || !epd_flush_ctx_->last_fb || !epd_flush_ctx_->work_fb ||
        !epd_flush_ctx_->gray_scratch) {
        ESP_LOGE(TAG, "epd flush ctx alloc failed");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    epd_gray_aa_ensure(SSD1677_PANEL_WIDTH, SSD1677_PANEL_HEIGHT);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_stack_size = 16 * 1024;
    adapter_cfg.task_max_delay_ms = 1000;
    // 等待至少一个系统节拍，避免短周期 LVGL 定时器折算为零而空转。
    adapter_cfg.task_min_delay_ms = portTICK_PERIOD_MS;
    // 与 470 显示任务一致：UI 回调访问 NVS 时必须使用内部 RAM 栈。
    adapter_cfg.stack_in_psram = false;
    adapter_cfg.task_priority = 1;
    adapter_cfg.task_core_id = 1;

    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_cfg));
    ESP_ERROR_CHECK(esp_lv_adapter_set_default_display_idf_callback_registration_enabled(false));

    // ROTATE_0：adapter 原样交出 I1；逻辑→面板旋转由 DisplayUi / OverlayDirty 做
    esp_lv_adapter_display_config_t disp_cfg = ESP_LV_ADAPTER_DISPLAY_SPI_MONO_DEFAULT_CONFIG(
        panel, panel_io, static_cast<uint16_t>(width), static_cast<uint16_t>(height), ESP_LV_ADAPTER_ROTATE_0,
        ESP_LV_ADAPTER_MONO_LAYOUT_HTILED);
    disp_cfg.profile.use_psram = true;

    lv_display_t* disp = esp_lv_adapter_register_display(&disp_cfg);
    ESP_ERROR_CHECK(disp ? ESP_OK : ESP_FAIL);
    epd_flush_ctx_->disp = disp;
    UiThemeApply(disp);
    DisplayUiBind(disp, SSD1677_PANEL_WIDTH, SSD1677_PANEL_HEIGHT);
    lv_display_add_event_cb(
        disp,
        [](lv_event_t*) {
            auto* ctx = s_refresh_ctx;
            if (ctx && ctx->work_fb) {
                if (DrainRefreshIfNeeded(ctx) != ESP_OK) {
                    ESP_LOGE(TAG, "margin preview refresh drain failed");
                    return;
                }
                memset(ctx->work_fb, kLvglWhite, ctx->fb_size);
                ctx->coalesce_pending = true;
                lv_obj_invalidate(lv_screen_active());
            }
        },
        LV_EVENT_REFRESH, nullptr);
    DisplayUiSetOrient(kDisplayUiPortrait);
    // Keep the full I1 framebuffer, but draw only LVGL's invalid rectangles.
    // Unlike PARTIAL, DIRECT preserves the full-frame stride expected by OverlayDirty.
    lv_display_set_render_mode(disp, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_add_event_cb(disp, OnLvglRefrStart, LV_EVENT_REFR_START, nullptr);
    lv_display_add_event_cb(
        disp,
        [](lv_event_t*) {
            StandbyPresentation::NotifyFlushCompleted();
            if (s_after_submit) {
                const auto callback = s_after_submit;
                s_after_submit = nullptr;
                callback();
            }
        },
        LV_EVENT_REFR_READY, nullptr);
    lv_timer_create(
        [](lv_timer_t*) {
            auto* display = LvAdapterSsd1677::Instance();
            if (!display->system_ready_) {
                if (!display->system_ready_pending_.exchange(false, std::memory_order_acq_rel)) {
                    return;
                }
                // 就绪通知是构造的最后一步，构造返回后才允许页面访问板级单例。
                Board::GetInstance();
                display->system_ready_ = true;
                display->SetupUI();
            }
            EpdGetStats();
            // 按 470 v2.0.75 的 Tick 顺序领取输入和后台结果；此回调持有 UI 锁。
            VkKey_DrainTouch();
            PagerKey_Drain();
            UiInputPoll();
            ScreenPollPendingWork();
            if (!StandbyPresentation::IsActive()) {
                s_wallpaper_loading_shown = false;
            } else if (!s_wallpaper_loading_shown && StandbyWallpaper_IsLoadingSlow()) {
                s_wallpaper_loading_shown = true;
                ShowWallpaperLoading(s_refresh_ctx);
            }
        },
        portTICK_PERIOD_MS, nullptr);

    epaper_panel_callbacks_t epd_cbs = {
        .on_epaper_refresh_done = EpdRefreshDoneCb,
    };
    ESP_ERROR_CHECK(epaper_panel_register_event_callbacks(panel, &epd_cbs, epd_flush_ctx_));
    ESP_ERROR_CHECK(esp_lv_adapter_set_area_rounder_cb(disp, EpdAreaRounder, nullptr));

    esp_lv_adapter_draw_bitmap_callbacks_t draw_cbs = {
        .custom_draw_bitmap = EpdLvglDrawBitmap,
    };
    ESP_ERROR_CHECK(esp_lv_adapter_set_draw_bitmap_callbacks(disp, &draw_cbs, epd_flush_ctx_));
    ESP_ERROR_CHECK(FirmwareCreateTask(CoalesceTask, "epd_commit", 6144, epd_flush_ctx_, 2,
                                            &epd_flush_ctx_->coalesce_task, 1, 0) == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);

    if (touch_handle != nullptr) {
        esp_lv_adapter_touch_config_t touch_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, touch_handle);
        touch_cfg.callbacks.custom_touch_read = CustomTouchRead;
        touch_indev_ = esp_lv_adapter_register_touch(&touch_cfg);
        ESP_ERROR_CHECK(touch_indev_ ? ESP_OK : ESP_FAIL);
        s_adapter_touch_read = lv_indev_get_read_cb(touch_indev_);
        lv_indev_set_read_cb(touch_indev_, ReadTouchWithQuickPanel);
        StartTouchFeed(touch_handle);
    }

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    const mmap_assets_config_t mmap_cfg = {
        .partition_label = "resources",
        .max_files = MMAP_RESOURCES_FILES,
        .checksum = MMAP_RESOURCES_CHECKSUM,
        .flags = {.mmap_enable = true},
    };
    ESP_ERROR_CHECK(mmap_assets_new(&mmap_cfg, &resources_assets_));

    esp_lv_fs_handle_t fs_handle = nullptr;
    const fs_cfg_t fs_cfg = {
        .fs_letter = 'A',
        .fs_nums = MMAP_RESOURCES_FILES,
        .fs_assets = resources_assets_,
    };
    ESP_ERROR_CHECK(esp_lv_adapter_fs_mount(&fs_cfg, &fs_handle));
}

void LvAdapterSsd1677::SetupUI() {
    DisplayUiScreenFactory pending = nullptr;
    lv_obj_t* startup = DisplayUiCreateStartup(touch_indev_ != nullptr, false, pending);
    if (!startup) {
        return;
    }
    lv_screen_load(startup);
    // 攒帧后黑→白→首页上屏
    if (lv_screen_active() != nullptr) {
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(nullptr);
    }
    EpdCommitBootBaseline(epd_flush_ctx_);
}

void LvAdapterSsd1677::KickTouchInput() {
    if (touch_indev_ != nullptr) {
        esp_lv_adapter_touch_notify_interrupt(touch_indev_);
    }
}

bool LvAdapterSsd1677::TryGetResource(const char* name, const uint8_t** mem, size_t* size) const {
    if (name == nullptr || name[0] == '\0' || mem == nullptr || size == nullptr) {
        return false;
    }
    *mem = nullptr;
    *size = 0;
    if (resources_assets_ == nullptr) {
        return false;
    }
    const int n = mmap_assets_get_stored_files(resources_assets_);
    for (int i = 0; i < n; ++i) {
        const char* nm = mmap_assets_get_name(resources_assets_, i);
        if (nm == nullptr || std::strcmp(nm, name) != 0) {
            continue;
        }
        const uint8_t* p = mmap_assets_get_mem(resources_assets_, i);
        const int sz = mmap_assets_get_size(resources_assets_, i);
        if (p == nullptr || sz <= 0) {
            return false;
        }
        *mem = p;
        *size = static_cast<size_t>(sz);
        return true;
    }
    return false;
}

bool TouchUiFingerIsDown(void) {
    portENTER_CRITICAL(&s_touch_snap_mux);
    // live 仍按着，且没有未交付的抬起（抬起已锁存说明手指已离开）
    const bool down = s_touch_snap.live_count > 0 && s_touch_snap.pending_up == 0;
    portEXIT_CRITICAL(&s_touch_snap_mux);
    return down;
}

bool TouchUiHasPendingEdges(void) {
    portENTER_CRITICAL(&s_touch_snap_mux);
    const bool pending = s_touch_snap.pending_down > 0 || s_touch_snap.pending_up > 0;
    portEXIT_CRITICAL(&s_touch_snap_mux);
    return pending;
}

void LvAdapterSsd1677::RegisterTouchVirtualKeys(const TouchVirtualKey* keys, size_t count, TouchVirtualKeyEventCb cb,
                                                void* user_data) {
    s_vk_count = 0;
    s_vk_cb = cb;
    s_vk_user = user_data;
    s_vk_instant_was_down = false;
    s_vk_hold = {};
    if (keys == nullptr || count == 0) {
        return;
    }
    if (count > kMaxTouchVirtualKeys) {
        count = kMaxTouchVirtualKeys;
    }
    for (size_t i = 0; i < count; ++i) {
        s_vk_keys[i] = keys[i];
    }
    s_vk_count = count;
    ESP_LOGD(TAG, "registered %u touch virtual keys", static_cast<unsigned>(s_vk_count));
}

void LvAdapterSsd1677::RestoreStatusWidgetsLocked() {
    // 切页只换控件指针，图标/静音/时钟缓存跨页保留，首帧与页面内容同一次局刷画出。
    // 局刷已改为启动后立刻 flush_ready（BUSY 延后到下次 Drain），状态栏合并刷新仍有助于少刷。
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
    if (status_label_ != nullptr) {
        // 无缓存时勿回退「待命」：首页 Idle 应以时钟为准，由 ApplyIdleStatusBar 写入
        const char* body = status_body_cache_[0] != '\0' ? status_body_cache_ : "--:--";
        ApplyStatusTextLocked(body);
        if (!DisplayUiWaveVisible()) {
            lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
            if (notification_label_ != nullptr) {
                lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void LvAdapterSsd1677::BindStatusWidgets(lv_obj_t* network, lv_obj_t* mute, lv_obj_t* battery, lv_obj_t* status,
                                         lv_obj_t* notification, lv_obj_t* low_battery_popup, lv_obj_t* battery_pct) {
    network_label_ = network;
    mute_label_ = mute;
    battery_label_ = battery;
    battery_pct_label_ = battery_pct;
    status_label_ = status;
    notification_label_ = notification;
    low_battery_popup_ = low_battery_popup;

    // 勿清 battery_icon_/network_icon_/muted_/status_body_cache_，也勿调 Board/UpdateStatusBar
    // （SetupUI 持锁重入会死锁）。有缓存则立刻灌进新控件，与本页首帧合并刷新。
    RestoreStatusWidgetsLocked();

    if (pending_notification_[0] != '\0' && notification_label_ != nullptr) {
        lv_label_set_text(notification_label_, pending_notification_);
        lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
        if (status_label_ != nullptr) {
            lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (notification_timer_ != nullptr) {
            esp_timer_stop(notification_timer_);
            if (pending_notification_ms_ > 0) {
                esp_timer_start_once(notification_timer_, static_cast<uint64_t>(pending_notification_ms_) * 1000ULL);
            }
        }
        pending_notification_[0] = '\0';
        pending_notification_ms_ = 0;
    }
}

void LvAdapterSsd1677::QueueStatusNotification(const char* notification, int duration_ms) {
    if (notification == nullptr || notification[0] == '\0') {
        pending_notification_[0] = '\0';
        pending_notification_ms_ = 0;
        return;
    }
    std::snprintf(pending_notification_, sizeof(pending_notification_), "%s", notification);
    pending_notification_ms_ = duration_ms;
    // 若状态栏已绑定，直接显示
    if (notification_label_ != nullptr) {
        ShowNotification(pending_notification_, duration_ms);
        pending_notification_[0] = '\0';
        pending_notification_ms_ = 0;
    }
}

LvAdapterSsd1677::~LvAdapterSsd1677() {
    if (instance_ == this) {
        instance_ = nullptr;
    }
    StopTouchFeed();
    if (notification_timer_) {
        esp_timer_stop(notification_timer_);
        esp_timer_delete(notification_timer_);
        notification_timer_ = nullptr;
    }
    if (epd_flush_ctx_) {
        ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));
        ESP_ERROR_CHECK(DrainRefreshIfNeeded(epd_flush_ctx_));
        epd_flush_ctx_->freeze_updates = true;
        epd_flush_ctx_->coalesce_pending = false;
        epaper_panel_callbacks_t cbs = {};
        epaper_panel_register_event_callbacks(epd_flush_ctx_->panel, &cbs, nullptr);
        if (epd_flush_ctx_->coalesce_task) {
            vTaskDelete(epd_flush_ctx_->coalesce_task);
            epd_flush_ctx_->coalesce_task = nullptr;
        }
        if (epd_flush_ctx_->last_fb) {
            heap_caps_free(epd_flush_ctx_->last_fb);
        }
        if (epd_flush_ctx_->work_fb) {
            heap_caps_free(epd_flush_ctx_->work_fb);
        }
        if (epd_flush_ctx_->ripple_fb) {
            heap_caps_free(epd_flush_ctx_->ripple_fb);
        }
        if (epd_flush_ctx_->gray_scratch) {
            heap_caps_free(epd_flush_ctx_->gray_scratch);
        }
        if (epd_flush_ctx_->done_sem) {
            vSemaphoreDelete(epd_flush_ctx_->done_sem);
        }
        s_refresh_ctx = nullptr;
        s_after_submit = nullptr;
        delete epd_flush_ctx_;
        epd_flush_ctx_ = nullptr;
        esp_lv_adapter_unlock();
    }
}

void LvAdapterSsd1677::SetEmotion(const char* emotion) {
    // 非对话页无表情控件，跳过加锁，少打断其它屏局刷。
    if (!ConversationPresentation::IsActive()) {
        return;
    }
    DisplayLockGuard lock(this);
    if (!lock) {
        return;
    }
    ConversationPresentation::SetEmotion(emotion);
}

void LvAdapterSsd1677::SetChatMessage(const char* role, const char* content) {
    // 只转发；AddMessage 内自行短持锁做 UI。解析/落盘在锁外，避免流式每包堵住翻页。
    if (!ConversationPresentation::IsActive()) {
        return;
    }
    ConversationPresentation::AddMessage(role, content);
}

void LvAdapterSsd1677::SetStatusTitlePrefix(const char* prefix) {
    DisplayLockGuard lock(this);
    if (!lock) {
        return;
    }
    if (prefix == nullptr || prefix[0] == '\0') {
        status_title_prefix_[0] = '\0';
    } else {
        std::snprintf(status_title_prefix_, sizeof(status_title_prefix_), "%s", prefix);
    }
}

void LvAdapterSsd1677::BeginStandbyEnterPaint() {
    DisplayLockGuard lock(this);
    if (!lock) {
        return;
    }
    auto* ctx = epd_flush_ctx_;
    if (ctx == nullptr || ctx->panel == nullptr) {
        return;
    }
    if (DrainRefreshIfNeeded(ctx) != ESP_OK) {
        return;
    }
    ctx->defer_standby_paint = true;
    ctx->coalesce_pending = false;
    ESP_LOGD(TAG, "standby enter: defer paint until FULL");
}

bool LvAdapterSsd1677::ParkEpdForStandby() {
    DisplayLockGuard lock(this);
    if (!lock) {
        return false;
    }
    auto* ctx = epd_flush_ctx_;
    if (ctx == nullptr || ctx->panel == nullptr || ctx->standby_frozen) {
        return ctx && ctx->standby_frozen;
    }

    if (DrainRefreshIfNeeded(ctx) != ESP_OK) {
        return false;
    }
    ctx->defer_standby_paint = false;

    // 叠图仍从进入前的界面合成。普通壁纸先把 last_fb 填白再居中写入，避免露出设置页。
    const RasterImage* standby_image = StandbyWallpaper_DecodedImage();
    if (standby_image != nullptr && ctx->last_fb != nullptr) {
        const bool drawn = StandbyWallpaper_IsOverlay()
                               ? WallpaperFrame_Draw(*standby_image, StandbyWallpaper_DecodedAlpha())
                               : BlitStandbyL8FullScreen(ctx, *standby_image);
        if (drawn) {
            ctx->coalesce_pending = false;
        } else {
            ESP_LOGW(TAG, "standby wallpaper composite failed; refresh previous frame");
        }
    }

    if (!standby_image && s_wallpaper_loading_shown) {
        WallpaperFrame_Restore();
    }

    // settle 攒帧完毕：全刷落待机画面后再冻结
    const uint8_t* fb = ctx->last_fb;
    if (ctx->coalesce_pending && ctx->work_fb != nullptr) {
        fb = ctx->work_fb;
    }
    if (fb != nullptr && ctx->has_last && ctx->last_fb != nullptr) {
        ctx->suppress_lvgl_notify = true;
        xSemaphoreTake(ctx->done_sem, 0);
        ESP_LOGD(TAG, "standby enter: FULL 0xC7 (content before freeze)");
        const esp_err_t err = FullRefreshBothSame(ctx, fb, "standby_enter", SSD1677_EPAPER_REFRESH_STANDBY);
        ctx->suppress_lvgl_notify = false;
        if (err == ESP_OK) {
            if (fb != ctx->last_fb) {
                memcpy(ctx->last_fb, fb, ctx->fb_size);
            }
            ctx->partial_refresh_count = 0;
            ctx->force_next_full = false;
            ctx->force_full_c7 = false;
        } else {
            ESP_LOGE(TAG, "standby enter FULL: code=%d", int(err));
            return false;
        }
    }
    if (!fb || !ctx->has_last) {
        return false;
    }
    ctx->coalesce_pending = false;
    ctx->refresh_inflight = false;

    // 冻结后续 LVGL flush；不做 0x10 Deep Sleep
    ctx->freeze_updates = true;
    ctx->standby_frozen = true;
    ESP_LOGD(TAG, "standby: flush frozen (no deep sleep)");
    return true;
}

void LvAdapterSsd1677::PauseStandbyBackgroundWork() {
    if (s_standby_background_paused) {
        return;
    }
    s_standby_background_paused = true;
    const bool had_touch_feed = (s_touch_feed_task != nullptr);
    if (had_touch_feed) {
        StopTouchFeed();
    }
    // 勿 lv_timer_enable(false)：lv_async_call 也是 lv_timer， dismiss 会永远排不上
    ESP_LOGD(TAG, "standby: paused touch_feed(%d)", had_touch_feed ? 1 : 0);
}

void LvAdapterSsd1677::ResumeStandbyBackgroundWork() {
    if (!s_standby_background_paused) {
        return;
    }
    s_standby_background_paused = false;
    if (s_touch_tp != nullptr && s_touch_feed_task == nullptr) {
        StartTouchFeed(s_touch_tp);
    }
    ESP_LOGD(TAG, "standby: resumed touch_feed");
}

void LvAdapterSsd1677::WakeEpdFromStandby() {
    DisplayLockGuard lock(this);
    if (!lock) {
        return;
    }
    auto* ctx = epd_flush_ctx_;
    if (ctx == nullptr || ctx->panel == nullptr) {
        return;
    }
    const bool was_defer = ctx->defer_standby_paint;
    const bool was_frozen = ctx->standby_frozen;
    ctx->defer_standby_paint = false;
    ctx->coalesce_pending = false;
    if (!was_defer && !was_frozen) {
        return;
    }
    if (was_frozen) {
        ctx->standby_frozen = false;
        ctx->freeze_updates = false;
    }
    // settle 前退出或正常 dismiss：等卸 Overlay 后首帧全刷底层（0xC7）
    ctx->force_next_full = true;
    ctx->force_full_c7 = true;
    ESP_LOGD(TAG, "standby: exit defer/freeze (next frame FULL 0xC7)");
}

void LvAdapterSsd1677::RequestNextFullRefresh() {
    if (epd_flush_ctx_ == nullptr) {
        return;
    }
    epd_flush_ctx_->force_next_full = true;
}

void LvAdapterSsd1677::RequestDualPlanePartial() {
    if (epd_flush_ctx_ == nullptr) {
        return;
    }
    epd_flush_ctx_->force_dual_plane_partial = true;
    // 与 C4 互斥：开/关 AA 要的是局刷双平面，不是全刷
    epd_flush_ctx_->force_next_full = false;
    epd_flush_ctx_->force_full_c7 = false;
}

void LvAdapterSsd1677::SleepEpdForPowerOff() {
    if (epd_flush_ctx_ == nullptr || epd_flush_ctx_->panel == nullptr) {
        return;
    }
    if (DrainRefreshIfNeeded(epd_flush_ctx_) != ESP_OK) {
        return;
    }
    const esp_err_t err = epaper_panel_deep_sleep(epd_flush_ctx_->panel);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "EPD deep sleep: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGD(TAG, "EPD deep sleep before PWR_KEY");
}

// 关机 JPG/PNG：逻辑竖屏居中写入 1bpp。坐标与 BlitA2i1ToPanelFb 相同，不跟当前 UI 方向。
bool BlitShutdownL8ToPanelFb(uint8_t* dst_fb, int panel_w, int panel_h, const RasterImage& img) {
    if (dst_fb == nullptr || panel_w <= 0 || panel_h <= 0 || img.empty() ||
        img.pixels.size() < static_cast<size_t>(img.width) * img.height) {
        return false;
    }
    const int dst_stride = panel_w / 8;
    memset(dst_fb, kLvglWhite, static_cast<size_t>(dst_stride) * static_cast<size_t>(panel_h));
    const int screen_w = panel_h;
    const int screen_h = panel_w;
    int draw_w = 0;
    int draw_h = 0;
    if (!FitImageSize(img.width, img.height, screen_w, screen_h, &draw_w, &draw_h)) {
        return false;
    }
    const int left = (screen_w - draw_w) / 2;
    const int top = (screen_h - draw_h) / 2;
    const bool same_size = draw_w == img.width && draw_h == img.height;
    for (int ly = 0; ly < draw_h; ++ly) {
        const int sy = same_size ? ly : static_cast<int>(int64_t(ly) * img.height / draw_h);
        const uint8_t* row = img.pixels.data() + static_cast<size_t>(sy) * img.width;
        for (int lx = 0; lx < draw_w; ++lx) {
            const int sx = same_size ? lx : static_cast<int>(int64_t(lx) * img.width / draw_w);
            const int px = top + ly;
            const int py = panel_h - 1 - (left + lx);
            if (px < 0 || py < 0 || px >= panel_w || py >= panel_h) {
                continue;
            }
            uint8_t* byte = &dst_fb[static_cast<size_t>(py) * dst_stride + (px >> 3)];
            const uint8_t mask = static_cast<uint8_t>(0x80 >> (px & 7));
            if (DarkenImagePixel(row[sx]) >= 128) {
                *byte |= mask;
            } else {
                *byte = static_cast<uint8_t>(*byte & ~mask);
            }
        }
        if ((ly & 63) == 0) {
            taskYIELD();
        }
    }
    ESP_LOGI(TAG, "shutdown wallpaper framebuffer: image=%ux%u draw=%dx%d at=(%d,%d)", static_cast<unsigned>(img.width),
             static_cast<unsigned>(img.height), draw_w, draw_h, left, top);
    return true;
}

struct ShutdownDecodeJob {
    char path[192] = {};
    RasterImage* image = nullptr;
    std::vector<uint8_t>* alpha = nullptr;
    lv_point_t size{};
    bool overlay = false;
    bool ok = false;
    SemaphoreHandle_t done = nullptr;
};

void ShutdownDecodeTask(void* arg) {
    auto* job = static_cast<ShutdownDecodeJob*>(arg);
    job->ok = job->image != nullptr &&
              (job->overlay ? DecodeImageFileOverlay(job->path, job->size.x, job->size.y, *job->image, *job->alpha)
                            : DecodeImageFileToL8(job->path, DISPLAY_CONTENT_W, DISPLAY_CONTENT_H, *job->image)) &&
              !job->image->empty();
    if (job->done != nullptr) {
        xSemaphoreGive(job->done);
    }
    vTaskDeleteWithCaps(nullptr);
}

// 非 A2I1 的关机壁纸走现有 L8 解码。A2I1 返回 false，由原加载函数处理。
static bool TryDecodeShutdownRaster(RasterImage& out, std::vector<uint8_t>& alpha, bool overlay) {
    if (!SdCardManager::GetInstance().IsMounted()) {
        const esp_err_t restored = power_hw_sd_restore();
        ESP_LOGI(TAG, "shutdown img: SD restore %s mounted=%d", esp_err_to_name(restored),
                 SdCardManager::GetInstance().IsMounted() ? 1 : 0);
    }
    if (!SdCardManager::GetInstance().IsMounted()) {
        return false;
    }
    char path[sizeof(ShutdownDecodeJob::path)] = {};
    if (!Wallpaper_TryResolveActiveWallpaperPath(path, sizeof(path))) {
        return false;
    }
    FILE* file = fopen(path, "rb");
    if (file == nullptr) {
        ESP_LOGW(TAG, "shutdown image open failed: errno=%d", errno);
        return false;
    }
    uint8_t magic[4] = {};
    const size_t magic_n = fread(magic, 1, sizeof(magic), file);
    fclose(file);
    if (magic_n >= 4 && magic[0] == 'A' && magic[1] == '2' && magic[2] == 'I' && magic[3] == '1') {
        return false;
    }

    ShutdownDecodeJob job;
    std::snprintf(job.path, sizeof(job.path), "%s", path);
    job.image = &out;
    job.alpha = &alpha;
    job.overlay = overlay;
    job.size = WallpaperFrame_Size();
    job.done = xSemaphoreCreateBinary();
    if (job.done == nullptr) {
        return false;
    }
    // 图片解码保留独立 PSRAM 栈，避免与关机呈现的调用栈叠加。
    if (FirmwareCreateTask(ShutdownDecodeTask, "off_wp_dec", 12 * 1024, &job, 5, nullptr, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vSemaphoreDelete(job.done);
        ESP_LOGW(TAG, "shutdown img: decode task create failed");
        return false;
    }
    const int64_t started = esp_timer_get_time();
    bool notice_shown = false;
    while (xSemaphoreTake(job.done, pdMS_TO_TICKS(100)) != pdTRUE) {
        if (!notice_shown && esp_timer_get_time() - started >= 1000000) {
            notice_shown = true;
            auto* display = LvAdapterSsd1677::Instance();
            DisplayLockGuard lock(display);
            if (lock) {
                ShowWallpaperLoading(s_refresh_ctx);
            }
        }
    }
    vSemaphoreDelete(job.done);
    if (!job.ok) {
        ESP_LOGW(TAG, "shutdown image decode failed; use built-in");
        out.Reset();
        return false;
    }
    ESP_LOGI(TAG, "shutdown image decoded (%ux%u)", static_cast<unsigned>(out.width),
             static_cast<unsigned>(out.height));
    return true;
}

void LvAdapterSsd1677::CancelPowerOff() {
    DisplayLockGuard lock(this);
    if (!lock) {
        return;
    }
    WakeEpdFromStandby();
    if (epd_flush_ctx_) {
        epd_flush_ctx_->freeze_updates = false;
    }
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev)) {
        lv_indev_enable(indev, true);
    }
    RequestFullOnNextCommit();
}

bool LvAdapterSsd1677::ShowPoweredOffScreen() {
    // 若待机曾冻结 flush，先恢复再画关机图
    WakeEpdFromStandby();
    RasterImage shutdown_l8;
    PrepareWallpaperOverlay();
    {
        DisplayLockGuard lock(this);
        if (!lock || !epd_flush_ctx_) {
            return false;
        }
        epd_flush_ctx_->freeze_updates = true;
    }
    const bool overlay = Wallpaper_IsOverlay(false) && WallpaperFrame_HasBase();
    std::vector<uint8_t> shutdown_alpha;
    const bool have_shutdown_l8 = TryDecodeShutdownRaster(shutdown_l8, shutdown_alpha, overlay);
    DisplayLockGuard lock(this);
    if (!lock) {
        return false;
    }
    BindStatusWidgets(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);

    // 直接往 EPD 帧缓冲画关机图，再关机全刷（0xC7）。
    // 加载链：NVS 的 JPG/PNG 或 A2I1 →（失败/坏图）内置 mmap →（再失败）「已关机」文字
    bool have_img = false;
    if (overlay && epd_flush_ctx_ && epd_flush_ctx_->work_fb) {
        const bool drawn = have_shutdown_l8 && WallpaperFrame_Draw(shutdown_l8, shutdown_alpha);
        if (!drawn) {
            ESP_LOGW(TAG, "shutdown overlay unavailable; restore previous frame");
            WallpaperFrame_Restore();
        }
        memcpy(epd_flush_ctx_->work_fb, epd_flush_ctx_->last_fb, epd_flush_ctx_->fb_size);
        have_img = EpdShutdownFullFb(epd_flush_ctx_, epd_flush_ctx_->work_fb) == ESP_OK;
    }
    if (!overlay && have_shutdown_l8 && epd_flush_ctx_ != nullptr && epd_flush_ctx_->work_fb != nullptr &&
        BlitShutdownL8ToPanelFb(epd_flush_ctx_->work_fb, epd_flush_ctx_->panel_w, epd_flush_ctx_->panel_h,
                                shutdown_l8)) {
        const esp_err_t err = EpdShutdownFullFb(epd_flush_ctx_, epd_flush_ctx_->work_fb);
        if (err == ESP_OK) {
            have_img = true;
        } else {
            ESP_LOGE(TAG, "shutdown full refresh failed: %s", esp_err_to_name(err));
        }
    }
    shutdown_l8.Reset();
    std::vector<uint8_t>().swap(shutdown_alpha);
    WallpaperFrame_Clear();
    uint8_t* sd_buf = nullptr;
    size_t sd_len = 0;
    if (!have_img && epd_flush_ctx_ != nullptr && epd_flush_ctx_->work_fb != nullptr) {
        const uint8_t* mem = nullptr;
        size_t plen = 0;
        bool from_sd = TryLoadShutdownA2i1FromSd(&sd_buf, &sd_len);

        auto try_parse = [&](const uint8_t* raw, size_t raw_len, bool strip_mmap_prefix) -> bool {
            if (raw == nullptr || raw_len == 0) {
                return false;
            }
            const uint8_t* p = raw;
            size_t n = raw_len;
            if (strip_mmap_prefix && n >= 6 && p[0] == 0x5A && p[1] == 0x5A && p[2] == 'A' && p[3] == '2' &&
                p[4] == 'I' && p[5] == '1') {
                p += 2;
                n -= 2;
            }
            uint16_t w = 0, h = 0, stride = 0;
            const uint8_t* payload = nullptr;
            if (!ParseA2i1(p, n, &w, &h, &stride, &payload) || payload == nullptr) {
                return false;
            }
            BlitA2i1ToPanelFb(epd_flush_ctx_->work_fb, epd_flush_ctx_->panel_w, epd_flush_ctx_->panel_h, payload, w, h,
                              stride);
            const esp_err_t err = EpdShutdownFullFb(epd_flush_ctx_, epd_flush_ctx_->work_fb);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "shutdown full refresh failed: %s", esp_err_to_name(err));
                return false;
            }
            return true;
        };

        if (from_sd) {
            if (try_parse(sd_buf, sd_len, false)) {
                have_img = true;
            } else {
                ESP_LOGW(TAG, "shutdown img: SD A2I1 unusable → built-in");
                heap_caps_free(sd_buf);
                sd_buf = nullptr;
                sd_len = 0;
                from_sd = false;
            }
        }

        if (!have_img && resources_assets_ != nullptr) {
            mem = mmap_assets_get_mem(resources_assets_, MMAP_RESOURCES_BG_SHUTDOWN_A2I1);
            const int sz = mmap_assets_get_size(resources_assets_, MMAP_RESOURCES_BG_SHUTDOWN_A2I1);
            if (mem != nullptr && sz > 0) {
                plen = static_cast<size_t>(sz);
                ESP_LOGI(TAG, "shutdown img: use built-in bg_shutdown.a2i1 (%u bytes)", static_cast<unsigned>(plen));
                have_img = try_parse(mem, plen, true);
                if (!have_img) {
                    ESP_LOGW(TAG, "shutdown img: built-in A2I1 parse/blit fail");
                }
            } else {
                ESP_LOGW(TAG, "shutdown img: built-in missing mem=%p sz=%d", mem, sz);
            }
        } else if (!have_img) {
            ESP_LOGW(TAG, "shutdown img: no resources_assets_");
        }
    }
    if (sd_buf != nullptr) {
        heap_caps_free(sd_buf);
        sd_buf = nullptr;
    }

    if (have_img) {
        // 冻结后续 flush，禁止 LVGL 再把白屏刷上去。
        if (epd_flush_ctx_ != nullptr) {
            epd_flush_ctx_->freeze_updates = true;
        }
    } else {
        if (epd_flush_ctx_) {
            epd_flush_ctx_->freeze_updates = false;
        }
        lv_obj_t* scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(scr, UiColorBackground(), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(scr, 0, 0);
        lv_obj_set_style_border_width(scr, 0, 0);
        lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* lbl = lv_label_create(scr);
        lv_label_set_text(lbl, LangString_POWERED_OFF);
        lv_obj_set_style_text_font(lbl, UiFont(), 0);
        lv_obj_set_style_text_color(lbl, UiColorForeground(), 0);
        lv_obj_center(lbl);
        lv_obj_t* old_scr = lv_screen_active();
        lv_screen_load(scr);
        if (old_scr != nullptr && old_scr != scr) {
            lv_obj_delete(old_scr);
        }
        RequestNextFullRefresh();
        lv_obj_invalidate(scr);
        lv_refr_now(nullptr);
        if (epd_flush_ctx_ != nullptr) {
            epd_flush_ctx_->freeze_updates = true;
        }
        have_img = epd_flush_ctx_ && EpdShutdownFullFb(epd_flush_ctx_, epd_flush_ctx_->last_fb) == ESP_OK;
        ESP_LOGI(TAG, "shutdown fallback result=%d", have_img);
    }
    if (!have_img) {
        ESP_LOGE(TAG, "shutdown screen did not complete");
    }
    return have_img;
}

void LvAdapterSsd1677::ApplyStatusTextLocked(const char* status) {
    const char* body = (status != nullptr) ? status : "";
    if (body != status_body_cache_) {
        std::snprintf(status_body_cache_, sizeof(status_body_cache_), "%s", body);
    }
    if (status_label_ == nullptr) {
        return;
    }
    // LVGL invalidates and recomputes label geometry even for identical text.
    // On e-paper this can repeatedly redraw the reading page behind the header.
    if (status_title_prefix_[0] != '\0') {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s·%s", status_title_prefix_, status_body_cache_);
        if (std::strcmp(lv_label_get_text(status_label_), buf) != 0) {
            lv_label_set_text(status_label_, buf);
        }
    } else {
        if (std::strcmp(lv_label_get_text(status_label_), status_body_cache_) != 0) {
            lv_label_set_text(status_label_, status_body_cache_);
        }
    }
}

void LvAdapterSsd1677::SetStatus(const char* status) {
    char activation_status[64];
    const auto activation_code = ActivationPolling::GetInstance().Code();
    const bool has_activation = !activation_code.empty();
    if (has_activation) {
        std::snprintf(activation_status, sizeof(activation_status), LangString_ACTIVATION_CODE_FMT,
                      activation_code.c_str());
        status = activation_status;
    }
    const bool is_clock_text =
        status != nullptr && std::strlen(status) == 5 &&
        ((status[2] == ':' && status[0] >= '0' && status[0] <= '9' && status[1] >= '0' && status[1] <= '9' &&
          status[3] >= '0' && status[3] <= '9' && status[4] >= '0' && status[4] <= '9') ||
         std::strcmp(status, "--:--") == 0);
    // 百问页：屏蔽待命/聆听/回答，保留连接网络/登录等系统提示（跟 DeviceState）
    // 连接完成切回 Idle 时，允许立即显示时钟，清掉残留的「连接中」。
    if (ConversationPresentation::IsActive() && !has_activation && !is_clock_text) {
        const DeviceState state = SystemState::GetInstance().Device();
        if (state == kDeviceStateIdle || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
            return;
        }
    }
    DisplayLockGuard lock(this);
    if (!lock) {
        return;
    }
    if (status_label_ == nullptr) {
        return;
    }
    ApplyStatusTextLocked(status);
    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    if (notification_label_) {
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    }
    last_status_update_time_ = std::chrono::system_clock::now();
}

void LvAdapterSsd1677::ShowNotification(const char* notification, int duration_ms) {
    DisplayLockGuard lock(this);
    if (!lock) {
        return;
    }
    if (notification_label_ == nullptr) {
        return;
    }
    lv_label_set_text(notification_label_, notification);
    lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    if (status_label_) {
        lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    }
    // 音量键通知后同步顶栏静音图标
    if (auto* codec = Board::GetInstance().GetAudioCodec()) {
        const bool want_mute = (codec->output_volume() == 0);
        if (want_mute != muted_ && mute_label_ != nullptr) {
            muted_ = want_mute;
            lv_label_set_text(mute_label_, muted_ ? FONT_AWESOME_VOLUME_XMARK : "");
        }
    }
    if (notification_timer_) {
        esp_timer_stop(notification_timer_);
        ESP_ERROR_CHECK(esp_timer_start_once(notification_timer_, duration_ms * 1000ULL));
    }
}

void LvAdapterSsd1677::UpdateStatusBar(bool update_all) {
    // 全屏页（老化/关机图等）已 ClearStatusBindings：勿再抢 DisplayLock，减轻与 LVGL 竞态
    if (mute_label_ == nullptr && network_label_ == nullptr && battery_label_ == nullptr && status_label_ == nullptr &&
        notification_label_ == nullptr && battery_pct_label_ == nullptr) {
        return;
    }

    auto& board = Board::GetInstance();
    auto* codec = board.GetAudioCodec();

    // I2C / 业务读取放锁外；所有 label 改动合并进一次 DisplayLock，减少无效局刷。
    const bool want_mute = (codec != nullptr && codec->output_volume() == 0);

    char time_str[64] = {};
    bool set_time = false;
    const auto activation_code = ActivationPolling::GetInstance().Code();
    if (!activation_code.empty()) {
        std::snprintf(time_str, sizeof(time_str), LangString_ACTIVATION_CODE_FMT, activation_code.c_str());
        set_time = true;
    }
    const bool assistant_page = ConversationPresentation::IsActive();
    const auto device_state = SystemState::GetInstance().Device();
    // 百问联网/启动中：顶栏保留「连接中…」等，勿用时钟盖住；Idle/聆听/播报仍刷时钟（对话文案已在 SetStatus
    // 过滤）
    const bool assistant_net_hint =
        assistant_page && (device_state == kDeviceStateConnecting || device_state == kDeviceStateStarting ||
                           device_state == kDeviceStateWifiConfiguring || device_state == kDeviceStateActivating);
    const bool want_clock =
        AllowsIdleStatusClock() && activation_code.empty() && !assistant_net_hint &&
        (assistant_page || (!SystemState::GetInstance().IsVoiceActive() && device_state == kDeviceStateIdle));
    if (want_clock &&
        (update_all || last_status_update_time_ + std::chrono::seconds(10) < std::chrono::system_clock::now())) {
        time_t now = time(nullptr);
        struct tm* tm = localtime(&now);
        if (tm && tm->tm_year >= 2025 - 1900) {
            strftime(time_str, sizeof(time_str), "%H:%M", tm);
            set_time = true;
        }
    }

    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    const char* new_battery_icon = nullptr;
    const bool battery_ok = board.GetBatteryLevel(battery_level, charging, discharging);
    if (battery_ok) {
        if (charging) {
            new_battery_icon = FONT_AWESOME_BATTERY_BOLT;
        } else {
            static const char* levels[] = {
                FONT_AWESOME_BATTERY_EMPTY,          FONT_AWESOME_BATTERY_QUARTER, FONT_AWESOME_BATTERY_HALF,
                FONT_AWESOME_BATTERY_THREE_QUARTERS, FONT_AWESOME_BATTERY_FULL,    FONT_AWESOME_BATTERY_FULL,
            };
            new_battery_icon = levels[battery_level / 20];
        }
    }

    const char* new_network_icon = nullptr;
    static int seconds_counter = 0;
    const bool wifi_busy = WifiStation::GetInstance().IsLinkInProgress();
    // 常态约 10s；WiFi 扫描/连接中每秒刷，配合弱→中→强梯度
    if (update_all || wifi_busy || (++seconds_counter % 10 == 0)) {
        static const std::vector<DeviceState> allowed_states = {
            kDeviceStateIdle,      kDeviceStateStarting, kDeviceStateWifiConfiguring, kDeviceStateConnecting,
            kDeviceStateListening, kDeviceStateSpeaking, kDeviceStateActivating,
        };
        if (update_all || wifi_busy ||
            std::find(allowed_states.begin(), allowed_states.end(), device_state) != allowed_states.end()) {
            new_network_icon = UiNetworkIcon(board.GetNetworkStatus());
        }
    }

    {
        DisplayLockGuard lock(this);
        if (!lock) {
            return;
        }
        if (mute_label_ == nullptr) {
            return;
        }

        if (want_mute != muted_) {
            muted_ = want_mute;
            lv_label_set_text(mute_label_, muted_ ? FONT_AWESOME_VOLUME_XMARK : "");
        }

        if (set_time && status_label_ != nullptr) {
            ApplyStatusTextLocked(time_str);
            // 百问或 WorkBuddy 按住说话时，波形占用状态栏中央，只更新文案，勿揭开时钟盖住动画
            // 通知展示中同样勿用时钟盖住（如开机 IO 扩展异常）
            if (!DisplayUiWaveVisible()) {
                const bool notif_on =
                    notification_label_ != nullptr && !lv_obj_has_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
                if (!notif_on) {
                    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
                    if (notification_label_ != nullptr) {
                        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
                    }
                }
            }
            last_status_update_time_ = std::chrono::system_clock::now();
        }

        if (battery_ok && new_battery_icon != nullptr) {
            if (battery_icon_ != new_battery_icon) {
                battery_icon_ = new_battery_icon;
                if (battery_label_ != nullptr) {
                    lv_label_set_text(battery_label_, battery_icon_);
                }
            }
            if (battery_level != battery_level_cache_) {
                battery_level_cache_ = battery_level;
                snprintf(battery_pct_cache_, sizeof(battery_pct_cache_), "%d%%", battery_level);
                if (battery_pct_label_ != nullptr) {
                    lv_label_set_text(battery_pct_label_, battery_pct_cache_);
                }
            }
            if (low_battery_popup_ != nullptr) {
                if (std::strcmp(new_battery_icon, FONT_AWESOME_BATTERY_EMPTY) == 0 && discharging) {
                    if (lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) {
                        lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                    }
                } else if (!lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) {
                    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }

        if (new_network_icon != nullptr && network_icon_ != new_network_icon) {
            network_icon_ = new_network_icon;
            if (network_label_ != nullptr) {
                lv_label_set_text(network_label_, network_icon_);
            }
        }
    }
}

void LvAdapterSsd1677::SetPowerSaveMode(bool on) {
    if (on) {
        SetChatMessage("system", "");
        SetEmotion("sleepy");
    } else {
        SetChatMessage("system", "");
        SetEmotion("neutral");
    }
}

void LvAdapterSsd1677::SetTheme(Theme* theme) {
    Display::SetTheme(theme);
}

bool LvAdapterSsd1677::Lock(int timeout_ms) {
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

void LvAdapterSsd1677::Unlock() {
    esp_lv_adapter_unlock();
}

bool LvAdapterSsd1677::IsReady() const {
    return system_ready_ && esp_lv_adapter_is_initialized();
}

void LvAdapterSsd1677::SetSystemReady() {
    system_ready_pending_.store(true, std::memory_order_release);
}

void LvAdapterSsd1677::RequestFullOnNextCommit() {
    RequestNextFullRefresh();
}

void LvAdapterSsd1677::BindStatusBar(lv_obj_t* status, lv_obj_t* notification, lv_obj_t* network, lv_obj_t* battery,
                                     lv_obj_t* battery_pct, lv_obj_t* mute) {
    BindStatusWidgets(network, mute, battery, status, notification, nullptr, battery_pct);
}

EpdStatusBar LvAdapterSsd1677::GetBoundStatusBar() const {
    EpdStatusBar bar;
    bar.network_label = network_label_;
    bar.mute_label = mute_label_;
    bar.battery_label = battery_label_;
    bar.status_label = status_label_;
    bar.notification_label = notification_label_;
    bar.battery_pct_label = battery_pct_label_;
    return bar;
}

bool EpdWait(uint32_t sequence, uint32_t timeout_ms) {
    const int64_t deadline = esp_timer_get_time() + int64_t(timeout_ms) * 1000;
    if (!sequence) {
        return false;
    }
    while (int32_t(s_completed.load(std::memory_order_acquire) - sequence) < 0) {
        if (esp_lv_adapter_lock(0) == ESP_OK) {
            EpdGetStats();
            esp_lv_adapter_unlock();
        }
        if (esp_timer_get_time() >= deadline) {
            return false;
        }
        vTaskDelay(1);
    }
    if (esp_lv_adapter_lock(1000) != ESP_OK) {
        return false;
    }
    const auto result = EpdGetStats().last_error;
    esp_lv_adapter_unlock();
    return result == ESP_OK;
}

void LvAdapterSsd1677::FullRefresh() {
    DisplayLockGuard lock(this);
    if (!lock) {
        return;
    }
    RequestNextFullRefresh();
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(nullptr);
}

void LvAdapterSsd1677::SetStreamPaintMode(bool) {}

void LvAdapterSsd1677::PrepareWallpaperOverlay() {
    DisplayLockGuard lock(this);
    if (!lock || WallpaperFrame_HasBase() || (!Wallpaper_IsOverlay(true) && !Wallpaper_IsOverlay(false))) {
        return;
    }
    auto* ctx = epd_flush_ctx_;
    if (!ctx || DrainRefreshIfNeeded(ctx) != ESP_OK) {
        ESP_LOGW(TAG, "overlay base refresh unavailable; use fullscreen wallpaper");
        return;
    }
    const bool deferred = ctx->defer_standby_paint;
    ctx->defer_standby_paint = true;
    lv_anim_refr_now();
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(ctx->disp);
    ctx->defer_standby_paint = deferred;
    if (!WallpaperFrame_Capture()) {
        ESP_LOGW(TAG, "overlay base allocation failed; use fullscreen wallpaper");
    }
}

void LvAdapterSsd1677::RequestRestoreFrontlightAfterCommit() {}

bool LvAdapterSsd1677::ShowLowBatteryShutdownNotice(bool low_voltage) {
    uint32_t sequence;
    {
        DisplayLockGuard lock(this);
        if (!lock || !IsReady()) {
            return false;
        }
        auto* label = lv_label_create(lv_layer_top());
        lv_label_set_text(
            label, low_voltage ? LangString_BATTERY_LOW_VOLTAGE_SHUTTING_DOWN : LangString_BATTERY_SHUTTING_DOWN);
        lv_obj_set_width(label, lv_pct(95));
        lv_obj_set_style_text_font(label, UiFont(), 0);
        lv_obj_set_style_bg_color(label, UiColorBackground(), 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, ScreenStatusBarHeight());
        if (epd_flush_ctx_) {
            epd_flush_ctx_->freeze_updates = false;
        }
        RequestNextFullRefresh();
        lv_refr_now(nullptr);
        sequence = s_submitted.load();
    }
    if (!EpdWait(sequence)) {
        ESP_LOGE(TAG, "low battery notice refresh timeout");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(3000));
    return true;
}

EpdFrameBuffer EpdGetFrameBuffer() {
    auto* ctx = s_refresh_ctx;
    return ctx ? EpdFrameBuffer{ctx->last_fb, ctx->panel_w, ctx->panel_h, (ctx->panel_w + 7) / 8, 1} : EpdFrameBuffer{};
}

EpdRefreshProfile EpdGetRefreshProfile() {
    return EpdRefreshProfile::Clear;
}

bool EpdSetRefreshProfile(EpdRefreshProfile profile) {
    return EpdProfileSupported(profile) && profile == EpdRefreshProfile::Clear;
}

bool EpdRefreshInit() {
    return s_refresh_ctx && s_refresh_ctx->panel;
}

uint32_t EpdSubmit(bool full, bool, bool) {
    auto* display = LvAdapterSsd1677::Instance();
    if (!display || !display->LockUi(1000)) {
        return 0;
    }
    if (full) {
        display->RequestNextFullRefresh();
    }
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(nullptr);
    const auto sequence = s_submitted.load();
    display->UnlockUi();
    return sequence;
}

void EpdSetEffect(EpdEffect effect) {
    EpdSetReaderEffect(effect);
}

void EpdSetTestActive(bool) {}

bool EpdTestRequest(EpdPattern, int, bool, int) {
    return false;
}

void EpdTestStop() {}

bool EpdSetVcomMv(int) {
    return false;
}

int EpdGetVcomMv() {
    return 0;
}

EpdBlackDepth EpdGetBlackDepth() {
    return EpdBlackDepth::Default;
}

bool EpdSetBlackDepth(EpdBlackDepth) {
    return false;
}
