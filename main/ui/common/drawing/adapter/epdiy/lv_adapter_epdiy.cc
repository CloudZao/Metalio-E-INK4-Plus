#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <vector>
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
#include <zlib.h>
#include <font_awesome.h>
#include "infra/tasks/task_create.h"
#include "business/conversation/conversation_presentation.h"
#include "business/conversation/activation_polling.h"
#include "sysmgmt/system_state.h"
#include "boards/common/input/input.h"
#include "ui/common/runtime/input/input_dispatch.h"
#include "ui/common/drawing/rendering/network_status.h"
#include "boards/common/display/epdiy/epdiy.h"
#include "ui/common/drawing/font/epd_font24/epd_font24.h"  // EPD_LOGICAL_W/H、epd_logical_to_physical
#include "lv_adapter_epdiy.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "boards/common/audio/audio_codec.h"
#include "boards/common/board/board.h"
#include "boards/common/display/frontlight.h"
#include "boards/common/input/buttons/io_expander/expander_button_input.h"
#include "boards/common/input/touch/ft6336u/touch_input.h"
#include "boards/common/storage/sd_access.h"
#include "infra/storage/sd_paths.h"
#include "infra/image/image_tone.h"
#include "config.h"
#include "lv_adapter_epdiy_touch.h"
#include "epd_panel.h"
#include "sysmgmt/device_state.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "ui/common/drawing/adapter/epd_refresh.h"
#include "ui/common/drawing/display_orient.h"
#include "ui/common/drawing/font/fontpack_lvgl.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/drawing/rendering/epd_content_policy.h"
#include "ui/common/drawing/rendering/epd_prepare.h"
#include "ui/common/drawing/rendering/wallpaper_frame.h"
#include "ui/common/runtime/input/boot_key_handler.h"
#include "sysmgmt/update/update_presentation.h"
#include "ui/common/widgets/screen_common.h"
#include "infra/image/image_geometry.h"
#include "infra/image/image_decode.h"
#include "business/wallpaper/wallpaper_active.h"
#include "ui/common/runtime/input/pager_key_handler.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/runtime/screen_common_nav.h"
#include "ui/common/runtime/display_ui.h"
#include "sysmgmt/power/standby_presentation.h"
#include "sysmgmt/power/standby_wallpaper.h"

/**
 * @file lv_adapter_epdiy.cc
 * @brief LVGL9 L8 灰度渲染 → EPDiy；开机加载主页，或优先恢复老化 / 阅读
 */


static bool BlitL8ToEpdFullScreen(const RasterImage& img);

#define TAG "lv_epdiy"

static std::atomic<bool> s_force_full{false};
static std::atomic<bool> s_stream_paint{false};
static std::atomic<bool> s_freeze_updates{false};
static std::atomic<bool> s_defer_standby_commit{false};     // 进待机先攒 FB，Park 再推白上屏
static bool s_standby_loading_notice_shown = false;         // 每次进入待机最多显示一次加载提示
static bool s_standby_frame_prepared = false;               // 重试复用已合成的目标帧
static const lv_area_t* s_wallpaper_notice_clip = nullptr;  // 同步刷新提示时仅写入提示矩形
static void ShowWallpaperLoading();
static std::atomic<bool> s_restore_frontlight_after_commit{false};  // 退出待机：底层上墨后再开前光
static SemaphoreHandle_t s_lvgl_mux = nullptr;

static mmap_assets_handle_t s_resources = nullptr;
static lv_display_t* s_disp = nullptr;
static uint8_t* s_draw_buffer = nullptr;
static size_t s_draw_buffer_bytes = 0;
static LvAdapterEpdiy* s_instance = nullptr;

// StartNetwork/激活占着 app_main：须另起 Tick 才能把顶栏刷到墨水屏（对齐 397 LVGL 任务）
static void DisplayTickTask(void* /*arg*/) {
    while (true) {
        auto* d = LvAdapterEpdiy::Instance();
        if (d != nullptr) {
            d->Tick();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static bool StartDisplayTickTaskOnce() {
    static bool started = false;
    if (started) {
        return true;
    }
    // 栈字节；对齐 397 LVGL 16KB（局刷深 + 部分页勿再往栈上塞大缓冲）
    if (FirmwareCreateTask(DisplayTickTask, "lv_epdiy_tick", 16 * 1024, nullptr, 2, nullptr, 1, 0) != pdPASS) {
        ESP_LOGE(TAG, "operation=setup_ui stage=start_tick result=task_unavailable");
        return false;
    }
    started = true;
    return true;
}

// LANDSCAPE 下直接写 4bpp FB（等同 epd_draw_pixel，避免每像素函数调用）
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
    EpdSubmit(force_full, s_stream_paint.load(), s_restore_frontlight_after_commit.exchange(false));
}

static uint32_t lv_tick_cb(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

class LvAdapterEpdiyUiRenderGuard {
public:
    explicit LvAdapterEpdiyUiRenderGuard(bool wait) {
        const int64_t deadline = esp_timer_get_time() + 10000000;
        while (!EpdTryBeginUiRender()) {
            if (!wait) {
                return;
            }
            if (esp_timer_get_time() >= deadline) {
                ESP_LOGE(TAG, "display rendering admission timeout");
                return;
            }
            vTaskDelay(1);
        }
        acquired_ = true;
    }

    ~LvAdapterEpdiyUiRenderGuard() {
        if (acquired_) {
            EpdEndUiRender();
        }
    }

    explicit operator bool() const {
        return acquired_;
    }

    LvAdapterEpdiyUiRenderGuard(const LvAdapterEpdiyUiRenderGuard&) = delete;
    LvAdapterEpdiyUiRenderGuard& operator=(const LvAdapterEpdiyUiRenderGuard&) = delete;

private:
    bool acquired_ = false;  // 只归还当前作用域成功取得的一层权限
};

static bool RenderDisplay(lv_display_t* display, bool wait) {
    // 待机可能已关闭底层页面的字体及 SD 资源，不能只在 flush 时丢弃像素。
    if (s_freeze_updates.load(std::memory_order_relaxed)) {
        return false;
    }
    if (display == nullptr) {
        display = lv_display_get_default();
    }
    lv_timer_t* timer = display != nullptr ? lv_display_get_refr_timer(display) : nullptr;
    if (timer == nullptr) {
        return false;
    }
    LvAdapterEpdiyUiRenderGuard guard(wait);
    if (!guard) {
        return false;
    }
    lv_display_refr_timer(timer);
    return true;
}

static bool RefreshDisplayNow(lv_display_t* display, bool wait = false) {
    // 强制刷新保持原有动画推进顺序，动画业务回调不持有绘制闸门。
    lv_anim_refr_now();
    return RenderDisplay(display, wait);
}

static void DisplayRefreshTimer(lv_timer_t* timer) {
    // 仅推迟实际绘制；输入、异步结果回收及其他定时器继续执行。
    RenderDisplay(static_cast<lv_display_t*>(lv_timer_get_user_data(timer)), false);
}

// Display resolution already excludes the panel's reserved border. Apply the
// same drawable bounds to every screen and overlay, including direct loads.
static void ConstrainDrawableRoot(lv_obj_t* root, int width, int height) {
    if (root == nullptr) {
        return;
    }
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
                           ? static_cast<lv_obj_t*>(lv_event_get_param(event))
                           : lv_display_get_screen_active(disp);
    const auto code = lv_event_get_code(event);
    if (code != LV_EVENT_REFRESH) {
        ConstrainDrawableRoot(screen, width, height);
        ConstrainDrawableRoot(lv_display_get_layer_top(disp), width, height);
        ConstrainDrawableRoot(lv_display_get_layer_sys(disp), width, height);
    }
    if (code == LV_EVENT_RESOLUTION_CHANGED || code == LV_EVENT_REFRESH) {
        // 分辨率或平移变化后清除旧位置，下一帧按新坐标重绘。
        if (uint8_t* fb = EpdRenderBuffer()) {
            memset(fb, 0xFF, static_cast<size_t>(epd_width() / 2) * epd_height());
        }
        if (screen != nullptr) {
            lv_obj_invalidate(screen);
        }
    }
}

// LVGL owns the complete L8 result, including clipping, masks and layer blending.
// PARTIAL buffers start at area.x1/y1; only convert pixels inside this dirty area.
static void blit_l8_area_to_epdiy(const uint8_t* pixels, const lv_area_t* area, uint8_t* fb, int disp_w, int disp_h) {
    if (!pixels || !area || !fb || disp_w <= 0 || disp_h <= 0) {
        return;
    }
    int x1 = std::max<int>(area->x1, 0), y1 = std::max<int>(area->y1, 0);
    int x2 = std::min<int>(area->x2, disp_w - 1), y2 = std::min<int>(area->y2, disp_h - 1);
    if (s_wallpaper_notice_clip != nullptr) {
        x1 = std::max<int>(x1, s_wallpaper_notice_clip->x1);
        y1 = std::max<int>(y1, s_wallpaper_notice_clip->y1);
        x2 = std::min<int>(x2, s_wallpaper_notice_clip->x2);
        y2 = std::min<int>(y2, s_wallpaper_notice_clip->y2);
    }
    if (x1 > x2 || y1 > y2) {
        return;
    }
    const int stride = lv_draw_buf_width_to_stride(lv_area_get_width(area), LV_COLOR_FORMAT_L8);
    int px0, py0, px1, py1, px2, py2;
    DisplayUiLogicalToPanel(x1, y1, &px0, &py0);
    DisplayUiLogicalToPanel(x1 + 1, y1, &px1, &py1);
    DisplayUiLogicalToPanel(x1, y1 + 1, &px2, &py2);
    EpdBlitL8(fb, pixels, stride, area->x1, area->y1, x1, y1, x2, y2, epd_width(), epd_height(), px0, py0, px1 - px0,
              py1 - py0, px2 - px0, py2 - py0);
}

static void blit_i1_area_to_epdiy(const uint8_t* px_map, const lv_area_t* area, uint8_t* fb, int disp_w, int disp_h) {
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
    const uint32_t pal_bytes = LV_COLOR_INDEXED_PALETTE_SIZE(LV_COLOR_FORMAT_I1) * (uint32_t)sizeof(lv_color32_t);
    // PARTIAL reshape：stride 对应该脏区宽度，不是全屏 disp_w
    const uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)(area->x2 - area->x1 + 1), LV_COLOR_FORMAT_I1);
    const uint8_t* bits = px_map + pal_bytes;
    const int fb_w = epd_width();
    const int fb_h = epd_height();
    const int64_t t0 = esp_timer_get_time();

    // 底边/非整屏局刷：打一次关键日志，便于对照「横线 / 底黑条」
    if (y1 > 0 || y2 < disp_h - 1 || x1 > 0 || x2 < disp_w - 1) {
        ESP_LOGD(TAG, "blit partial area=%d,%d-%d,%d aw=%d ah=%d stride=%u disp=%dx%d", x1, y1, x2, y2, area_w, area_h,
                 (unsigned)stride, disp_w, disp_h);
    }

    int px0, py0, px1, py1, px2, py2;
    DisplayUiLogicalToPanel(x1, y1, &px0, &py0);
    DisplayUiLogicalToPanel(x1 + 1, y1, &px1, &py1);
    DisplayUiLogicalToPanel(x1, y1 + 1, &px2, &py2);
    EpdBlitI1(fb, bits, stride, area->x1, area->y1, x1, y1, x2, y2, fb_w, fb_h, px0, py0, px1 - px0, py1 - py0,
              px2 - px0, py2 - py0);
    ESP_LOGD(TAG, "blit area %d,%d-%d,%d cost=%lldms", x1, y1, x2, y2, (long long)((esp_timer_get_time() - t0) / 1000));
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
    if (lv_display_get_color_format(disp) == LV_COLOR_FORMAT_I1) {
        blit_i1_area_to_epdiy(px_map, area, fb, disp_w, disp_h);
    } else {
        blit_l8_area_to_epdiy(px_map, area, fb, disp_w, disp_h);
    }

    if (lv_display_flush_is_last(disp)) {
        const int64_t t0 = esp_timer_get_time();
        const bool force = s_force_full.exchange(false);
        ESP_LOGD(TAG, "flush last area=%d,%d-%d,%d force=%d defer=%d", area->x1, area->y1, area->x2, area->y2,
                 force ? 1 : 0, s_defer_standby_commit.load() ? 1 : 0);
        // 待机或关机：只收集完整画面，由定格路径统一提交推白
        if (!s_defer_standby_commit.load(std::memory_order_relaxed)) {
            epd_commit_update(force);
        }
        StandbyPresentation::NotifyFlushCompleted();
        ESP_LOGD(TAG, "commit cost=%lldms", (long long)((esp_timer_get_time() - t0) / 1000));
    }
    lv_display_flush_ready(disp);
}

static bool ShowTopNotice(const char* text, uint32_t hold_ms = 0) {
    lv_obj_t* notice = lv_label_create(lv_layer_top());
    lv_obj_clear_flag(notice, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_style_all(notice);
    lv_label_set_text(notice, text);
    lv_label_set_long_mode(notice, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(notice, LV_HOR_RES - 32, 0);
    lv_obj_set_style_text_font(notice, UiFont(), 0);
    lv_obj_set_style_text_color(notice, UiColorBackground(), 0);
    lv_obj_set_style_bg_color(notice, UiColorForeground(), 0);
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
    const bool rendered = RefreshDisplayNow(s_disp, true);
    s_wallpaper_notice_clip = nullptr;
    s_freeze_updates.store(was_frozen);
    if (!rendered) {
        lv_obj_delete(notice);
        return false;
    }
    const bool shown = EpdWait(EpdSubmit(false));
    if (shown && hold_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(hold_ms));
    }
    lv_obj_delete(notice);
    return shown;
}

static void ShowWallpaperLoading() {
    // 保存提示写入前的画面，解码失败时直接恢复，避免重绘已停放的底层页面。
    if (!WallpaperFrame_Capture()) {
        return;
    }
    if (!ShowTopNotice(LangString_WALLPAPER_ENTER_LOADING)) {
        ESP_LOGW(TAG, "wallpaper loading notice failed");
    }
}

// L8 背景在完整面板区域等比居中，不使用普通 UI 内缩。
static bool BlitL8ToEpdFullScreen(const RasterImage& img) {
    uint8_t* fb = EpdRenderBuffer();
    if (fb == nullptr || img.empty()) {
        return false;
    }
    memset(fb, 0xFF, static_cast<size_t>(epd_width() / 2) * epd_height());
    const int max_w = DISPLAY_WIDTH;
    const int max_h = DISPLAY_HEIGHT;
    int draw_w = 0, draw_h = 0;
    if (!FitImageSize(img.width, img.height, max_w, max_h, &draw_w, &draw_h)) {
        return false;
    }
    const int left = (max_w - draw_w) / 2;
    const int top = (max_h - draw_h) / 2;
    ESP_LOGI(TAG, "wallpaper framebuffer: panel=%dx%d image=%ux%u draw=%dx%d at=(%d,%d)", max_w, max_h,
             static_cast<unsigned>(img.width), static_cast<unsigned>(img.height), draw_w, draw_h, left, top);
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
            FbPutNative(fb, px, py, DarkenImagePixel(row[lx * img.width / draw_w]), fb_w);
        }
        if ((ly & 63) == 0) {
            taskYIELD();
        }
    }
    return true;
}

static bool TryDecodeShutdownFromSd(RasterImage& out) {
    // 待机保持 SD 访问暂停，仅允许当前任务读取关机壁纸。
    SdAccessPermit permit;
    char path[SD_FILE_PATH_CAPACITY] = {};
    if (!Wallpaper_TryResolveActiveWallpaperPath(path, sizeof(path))) {
        ESP_LOGI(TAG, "shutdown img: no NVS shutdown file");
        return false;
    }
    if (!DecodeImageFileToL8(path, DISPLAY_WIDTH, DISPLAY_HEIGHT, out) || out.empty()) {
        ESP_LOGW(TAG, "shutdown img: SD decode failed");
        return false;
    }
    ESP_LOGI(TAG, "shutdown img: SD %dx%d", out.width, out.height);
    return true;
}

static bool TryDecodeShutdownBuiltin(RasterImage& out) {
    // 中英文默认图随应用固件更新，不依赖 resources 分区升级。
    extern const uint8_t zh_start[] asm("_binary_bg_shutdown_zh_a2i1_z_start");
    extern const uint8_t zh_end[] asm("_binary_bg_shutdown_zh_a2i1_z_end");
    extern const uint8_t en_start[] asm("_binary_bg_shutdown_en_a2i1_z_start");
    extern const uint8_t en_end[] asm("_binary_bg_shutdown_en_a2i1_z_end");
    const bool is_zh = std::strcmp(LangCode, "zh-CN") == 0;
    const uint8_t* start = is_zh ? zh_start : en_start;
    const uint8_t* end = is_zh ? zh_end : en_end;
    uint32_t raw_size = 0;
    if (end - start > 4) {
        std::memcpy(&raw_size, start, sizeof(raw_size));
    }
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> raw(nullptr, heap_caps_free);
    if (raw_size && raw_size <= DISPLAY_WIDTH * DISPLAY_HEIGHT) {
        raw.reset(static_cast<uint8_t*>(heap_caps_malloc(raw_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    }
    uLongf decoded_size = raw_size;
    if (raw && uncompress(raw.get(), &decoded_size, start + 4, end - start - 4) == Z_OK && decoded_size == raw_size &&
        DecodeImageToL8(raw.get(), raw_size, 0, 0, out) && !out.empty()) {
        ESP_LOGI(TAG, "shutdown img: embedded %s %dx%d", LangCode, out.width, out.height);
        return true;
    }
    ESP_LOGW(TAG, "shutdown img: embedded %s decode fail", LangCode);
    raw.reset();
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
    if (n >= 6 && p[0] == 0x5A && p[1] == 0x5A && p[2] == 'A' && p[3] == '2' && p[4] == 'I' && p[5] == '1') {
        p += 2;
        n -= 2;
    }
    if (!DecodeImageToL8(p, n, 0, 0, out) || out.empty()) {
        ESP_LOGW(TAG, "shutdown img: built-in decode fail");
        return false;
    }
    ESP_LOGI(TAG, "shutdown img: built-in %dx%d (%u bytes)", out.width, out.height, static_cast<unsigned>(sz));
    return true;
}

static bool ShowPoweredOffTextFallback() {
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
    lv_obj_invalidate(scr);
    return RefreshDisplayNow(nullptr, true);
}

struct LvAdapterEpdiyShutdownWallpaperWork {
    RasterImage img;                // 当前关机流程持有的解码像素
    bool have_img = false;          // 解码成功后由 done 发布
    bool overlay = false;           // 本次关机使用原界面叠图
    lv_point_t size{};              // 竖屏壁纸解码范围
    std::vector<uint8_t> alpha;     // 图片自带的透明度
    std::atomic<bool> done{false};  // 解码任务完成标记
};

static void DecodeShutdownWallpaperTask(void* arg) {
    auto* work = static_cast<LvAdapterEpdiyShutdownWallpaperWork*>(arg);
    if (work->overlay) {
        SdAccessPermit permit;
        char path[SD_FILE_PATH_CAPACITY] = {};
        if (Wallpaper_TryResolveActiveWallpaperPath(path, sizeof(path))) {
            work->have_img = DecodeImageFileOverlay(path, work->size.x, work->size.y, work->img, work->alpha);
        }
        if (!work->have_img) {
            ESP_LOGW(TAG, "power-off overlay unavailable; retaining base frame");
        }
    } else {
        work->have_img = TryDecodeShutdownFromSd(work->img);
    }
    if (!work->have_img && !work->overlay) {
        work->have_img = TryDecodeShutdownBuiltin(work->img);
    }
    work->done.store(true, std::memory_order_release);
    vTaskDeleteWithCaps(nullptr);
}

LvAdapterEpdiy* LvAdapterEpdiy::Instance() {
    return s_instance;
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

bool LvAdapterEpdiy::Init() {
    s_instance = this;
    if (lvgl_ready_) {
        return true;
    }
    if (!EpdPanel_IsReady()) {
        ESP_LOGE(TAG, "epd not ready");
        return false;
    }

    if (s_lvgl_mux == nullptr) {
        s_lvgl_mux = xSemaphoreCreateRecursiveMutex();
    }

    if (!fontpack_lv_ensure_ready()) {
        ESP_LOGE(TAG, "fontpack init failed (flash main/assets/fonts/font.fontpack to font_data)");
        return false;
    }

    width_ = DISPLAY_CONTENT_W;
    height_ = DISPLAY_CONTENT_H;

    if (!EpdRefreshInit()) {
        ESP_LOGE(TAG, "refresh worker/buffers unavailable");
        return false;
    }
    lv_init();
    if (!EpdContentBlendInit()) {
        return false;
    }
    lv_tick_set_cb(lv_tick_cb);
    s_disp = lv_display_create(width_, height_);
    UiThemeApply(s_disp);
    lv_display_add_event_cb(s_disp, drawable_bounds_cb, LV_EVENT_SCREEN_LOAD_START, nullptr);
    lv_display_add_event_cb(s_disp, drawable_bounds_cb, LV_EVENT_RESOLUTION_CHANGED, nullptr);
    lv_display_add_event_cb(s_disp, drawable_bounds_cb, LV_EVENT_REFRESH, nullptr);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_I1);
    DisplayUiBind(s_disp, epd_width(), epd_height());
    width_ = DisplayUiContentW();
    height_ = DisplayUiContentH();

    // A bounded stripe works in either orientation. LVGL renders larger dirty
    // areas in chunks; no full-screen shadow/coverage buffer is needed.
    constexpr uint32_t kDrawRows = 64;
    const uint32_t max_stride =
        lv_draw_buf_width_to_stride(std::max(DISPLAY_WIDTH, DISPLAY_HEIGHT), LV_COLOR_FORMAT_L8);
    const size_t binary_bytes =
        LV_COLOR_INDEXED_PALETTE_SIZE(LV_COLOR_FORMAT_I1) * sizeof(lv_color32_t) +
        std::max(size_t(lv_draw_buf_width_to_stride(DISPLAY_WIDTH, LV_COLOR_FORMAT_I1)) * DISPLAY_HEIGHT,
                 size_t(lv_draw_buf_width_to_stride(DISPLAY_HEIGHT, LV_COLOR_FORMAT_I1)) * DISPLAY_WIDTH);
    // Retain a full I1 frame for fast ordinary pages; gray reuses this allocation.
    const size_t buf_size = std::max<size_t>(binary_bytes, size_t(max_stride) * size_t(kDrawRows));
    uint8_t* buf1 = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf1 == nullptr) {
        buf1 = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (buf1 == nullptr) {
        ESP_LOGE(TAG, "no draw buffer %u", (unsigned)buf_size);
        return false;
    }
    s_draw_buffer = buf1;
    s_draw_buffer_bytes = buf_size;
    lv_display_set_buffers(s_disp, buf1, nullptr, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(
        s_disp,
        [](lv_event_t* event) {
            auto* display = static_cast<lv_display_t*>(lv_event_get_target(event));
            auto* root = lv_display_get_screen_active(display);
            if (!root) {
                return;
            }
            lv_obj_update_layout(root);
            lv_obj_update_layout(lv_display_get_layer_top(display));
            lv_obj_update_layout(lv_display_get_layer_sys(display));
            const auto format = EpdVisibleContentNeedsGray(display) ? LV_COLOR_FORMAT_L8 : LV_COLOR_FORMAT_I1;
            if (format == lv_display_get_color_format(display)) {
                return;
            }
            lv_display_set_color_format(display, format);
            lv_display_set_buffers(display, s_draw_buffer, nullptr, s_draw_buffer_bytes,
                                   LV_DISPLAY_RENDER_MODE_PARTIAL);
            lv_obj_invalidate(root);
            lv_obj_invalidate(lv_display_get_layer_top(display));
            lv_obj_invalidate(lv_display_get_layer_sys(display));
            ESP_LOGI(TAG, "content render format=%s", format == LV_COLOR_FORMAT_I1 ? "I1" : "L8");
        },
        LV_EVENT_REFR_START, nullptr);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_timer_set_cb(lv_display_get_refr_timer(s_disp), DisplayRefreshTimer);
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

    ESP_LOGI(TAG, "SetupUI: initialize screen %dx%d offset=%d,%d", width_, height_, DisplayUiOffsetX(),
             DisplayUiOffsetY());
    lv_obj_t* startup = DisplayUiCreateStartup(true, true, boot_resume_factory_);
    if (!startup) {
        return false;
    }
    if (boot_resume_factory_) {
        FreezeFlush(true);
    }
    lv_screen_load(startup);
    if (!boot_resume_factory_ && !RefreshDisplayNow(s_disp, true)) {
        return false;
    }

    {
        esp_timer_create_args_t notification_timer_args = {
            .callback =
                [](void* arg) {
                    auto* display = static_cast<LvAdapterEpdiy*>(arg);
                    DisplayLockGuard lock(display);
                    if (!lock) {
                        return;
                    }
                    if (display->notification_label_ != nullptr) {
                        lv_obj_add_flag(display->notification_label_, LV_OBJ_FLAG_HIDDEN);
                    }
                    if (display->status_label_ != nullptr && !DisplayUiWaveVisible()) {
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
    if (!StartDisplayTickTaskOnce()) {
        lvgl_ready_ = false;
        return false;
    }
    ESP_LOGI(TAG, "LVGL home ready");
    return true;
}

void LvAdapterEpdiy::Tick() {
    if (!lvgl_ready_) {
        return;
    }
    // Board 构造期间仍可能申请 UI 锁；在锁外等待构造完成，避免正文电量查询递归取单例。
    const bool finish_boot = system_ready_pending_.load(std::memory_order_acquire);
    if (finish_boot) {
        Board::GetInstance();
    }
    if (Lock(50)) {
        // 只消费本轮已在锁外等待过的通知；抢锁失败或稍后到达的通知留给下一轮。
        if (finish_boot && system_ready_pending_.exchange(false, std::memory_order_acq_rel)) {
            if (boot_resume_factory_) {
                auto factory = boot_resume_factory_;
                boot_resume_factory_ = nullptr;
                ScreenLoadReplace(factory());
                FreezeFlush(false);
                RequestFullOnNextCommit();
            }
            SetStatus("");
        }
        const int64_t round_start = esp_timer_get_time();
        if (!powering_off_) {
            VkKey_DrainTouch();
            PagerKey_Drain();
            UiInputPoll();
        }
        ScreenPollPendingWork();
        if (StandbyPresentation::IsActive() && !s_standby_loading_notice_shown && StandbyWallpaper_IsLoadingSlow()) {
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
                ESP_LOGD(TAG, "UI round slow total=%lldms timers=%lldms suppressed=%u",
                         static_cast<long long>(round_us / 1000), static_cast<long long>(dt / 1000), suppressed);
                last_log_us = now;
                suppressed = 0;
            } else {
                ++suppressed;
            }
        }
    }
}

void LvAdapterEpdiy::SetSystemReady() {
    system_ready_pending_.store(true, std::memory_order_release);
}

void LvAdapterEpdiy::FullRefresh() {
    if (!lvgl_ready_ || s_disp == nullptr) {
        return;
    }
    s_stream_paint.store(false);
    s_force_full.store(true);
    if (Lock(1000)) {
        lv_obj_t* scr = lv_screen_active();
        if (scr != nullptr) {
            lv_obj_invalidate(scr);
        }
        RefreshDisplayNow(s_disp);
        Unlock();
    }
}

void LvAdapterEpdiy::SetStreamPaintMode(bool on) {
    const bool was_on = s_stream_paint.exchange(on);
    if (was_on && !on) {
        EpdRequestScreenChange();
    }
}

void LvAdapterEpdiy::SetStatusTitlePrefix(const char* prefix) {
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

void LvAdapterEpdiy::SetStatus(const char* status) {
    // 百问页：屏蔽聆听/说话文案（PTT 波形占顶栏）；Idle 须放行时钟（ApplyIdleStatusBar）
    if (ConversationPresentation::IsActive()) {
        const DeviceState state = SystemState::GetInstance().Device();
        if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
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
    if (!lock) {
        return;
    }
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
            ESP_ERROR_CHECK(esp_timer_start_once(notification_timer_, static_cast<uint64_t>(duration_ms) * 1000ULL));
        }
    }
}

bool LvAdapterEpdiy::ShowLowBatteryShutdownNotice(bool low_voltage) {
    DisplayLockGuard lock(this);
    if (!lock || !lvgl_ready_) {
        return false;
    }
    return ShowTopNotice(low_voltage ? LangString_BATTERY_LOW_VOLTAGE_SHUTTING_DOWN : LangString_BATTERY_SHUTTING_DOWN,
                         3000);
}

void LvAdapterEpdiy::SetChatMessage(const char* role, const char* content) {
    if (!ConversationPresentation::IsActive()) {
        return;
    }
    ConversationPresentation::AddMessage(role, content);
}

void LvAdapterEpdiy::SetEmotion(const char* emotion) {
    if (!ConversationPresentation::IsActive()) {
        return;
    }
    DisplayLockGuard lock(this);
    if (!lock) {
        return;
    }
    ConversationPresentation::SetEmotion(emotion);
}

void LvAdapterEpdiy::UpdateStatusBar(bool update_all) {
    // Background boot/network callbacks must not lazily initialize I2S while
    // standby owns and may power down the peripherals.
    if (StandbyPresentation::IsActive()) {
        return;
    }
    auto& board = Board::GetInstance();

    char time_str[16] = {};
    bool set_time = false;
    const char* wifi_busy = UiNetworkBusyText(board.GetNetworkStatus());
    const bool assistant_page = ConversationPresentation::IsActive();
    const auto device_state = SystemState::GetInstance().Device();
    // 百问联网/启动中：顶栏保留「连接中…」等，勿用时钟盖住
    const bool assistant_net_hint =
        assistant_page && (device_state == kDeviceStateConnecting || device_state == kDeviceStateStarting ||
                           device_state == kDeviceStateWifiConfiguring || device_state == kDeviceStateActivating);
    const bool want_clock =
        wifi_busy == nullptr && AllowsIdleStatusClock() && !assistant_net_hint &&
        (assistant_page || (!ActivationPolling::GetInstance().HasCode() &&
                            !SystemState::GetInstance().IsVoiceActive() && device_state == kDeviceStateIdle));
    if (want_clock &&
        (update_all || last_status_update_time_ + std::chrono::seconds(10) < std::chrono::system_clock::now())) {
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
    if (!lock) {
        return;
    }
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
        if (!DisplayUiWaveVisible()) {
            if (lv_obj_has_flag(status_label_, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
            }
            if (notification_label_ != nullptr && !lv_obj_has_flag(notification_label_, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else if (set_time && status_label_ != nullptr) {
        ApplyStatusTextLocked(time_str);
        if (!DisplayUiWaveVisible()) {
            const bool notif_on =
                notification_label_ != nullptr && !lv_obj_has_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
            if (!notif_on) {
                if (lv_obj_has_flag(status_label_, LV_OBJ_FLAG_HIDDEN)) {
                    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
                }
                if (notification_label_ != nullptr && !lv_obj_has_flag(notification_label_, LV_OBJ_FLAG_HIDDEN)) {
                    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
        last_status_update_time_ = std::chrono::system_clock::now();
    }

    if (network_label_ != nullptr) {
        const char* neu = UiNetworkIcon(board.GetNetworkStatus());
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

void LvAdapterEpdiy::BindStatusBar(lv_obj_t* status_label, lv_obj_t* notification_label, lv_obj_t* network_label,
                                   lv_obj_t* battery_label, lv_obj_t* battery_pct_label, lv_obj_t* mute_label) {
    status_label_ = status_label;
    notification_label_ = notification_label;
    network_label_ = network_label;
    battery_label_ = battery_label;
    battery_pct_label_ = battery_pct_label;
    mute_label_ = mute_label;
    RestoreStatusWidgetsLocked();
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

void LvAdapterEpdiy::RequestFullOnNextCommit() {
    s_force_full.store(true);
}

void LvAdapterEpdiy::BeginStandbyEnterPaint() {
    EpdSetTestActive(false);
    EpdCancelPageTurn();
    s_restore_frontlight_after_commit.store(false);
    s_stream_paint.store(false);
    s_defer_standby_commit.store(true, std::memory_order_relaxed);
    s_force_full.store(false);
    s_standby_loading_notice_shown = false;
    s_standby_frame_prepared = false;
    FreezeFlush(Wallpaper_HasStandbySelection());
    ESP_LOGI(TAG, "standby enter: defer panel commit until Park");
}

void LvAdapterEpdiy::PrepareWallpaperOverlay() {
    if (WallpaperFrame_HasBase() || (!Wallpaper_IsOverlay(true) && !Wallpaper_IsOverlay(false))) {
        return;
    }
    const bool deferred = s_defer_standby_commit.exchange(true);
    // 在底层字体、页面资源停放前完成最后一帧，底图中不包含壁纸或加载提示。
    lv_anim_refr_now();
    {
        LvAdapterEpdiyUiRenderGuard guard(true);
        if (guard && RenderDisplay(s_disp, false)) {
            if (!WallpaperFrame_Capture()) {
                ESP_LOGW(TAG, "overlay base allocation failed; use fullscreen wallpaper");
            }
        }
    }
    s_defer_standby_commit.store(deferred);
}

bool LvAdapterEpdiy::ParkEpdForStandby() {
    DisplayLockGuard lock(this);
    if (!lock) {
        return false;
    }
    if (!s_standby_frame_prepared && !StandbyWallpaper_IsOverlay() && Wallpaper_HasStandbySelection() &&
        StandbyWallpaper_DecodedImage() == nullptr) {
        // 无提示时帧缓冲仍是原页面；有提示时用底图恢复，不访问已关闭的 SD 资源。
        if (WallpaperFrame_HasBase() && !WallpaperFrame_Restore()) {
            return false;
        }
    }
    // 经典页已完成 flush；壁纸直接写入同一缓冲，均只提交一次。
    FreezeFlush(true);
    s_defer_standby_commit.store(false, std::memory_order_relaxed);
    s_stream_paint.store(false);
    s_force_full.store(false);
    if (!s_standby_frame_prepared) {
        if (StandbyWallpaper_IsOverlay()) {
            const auto* wallpaper = StandbyWallpaper_DecodedImage();
            if (!wallpaper || !WallpaperFrame_Draw(*wallpaper, StandbyWallpaper_DecodedAlpha())) {
                if (!WallpaperFrame_Restore()) {
                    return false;
                }
            }
        } else if (const auto* wallpaper = StandbyWallpaper_DecodedImage()) {
            if (!BlitL8ToEpdFullScreen(*wallpaper)) {
                return false;
            }
        }
        s_standby_frame_prepared = true;
        StandbyWallpaper_ReleasePixels();
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
    WallpaperFrame_Clear();
    // 待机壁纸直接覆盖物理全屏；唤醒重绘前清除普通 UI 内缩范围之外的旧像素。
    if (uint8_t* fb = EpdRenderBuffer()) {
        memset(fb, 0xFF, static_cast<size_t>(epd_width() / 2) * epd_height());
    }
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
    touch_input_set_feed_paused(true);
}

void LvAdapterEpdiy::ResumeStandbyBackgroundWork() {
    touch_input_set_feed_paused(false);
}

bool LvAdapterEpdiy::ShowPoweredOffScreen() {
    DisplayLockGuard lock(this);
    if (!lock) {
        return false;
    }
    powering_off_ = true;
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev != nullptr; indev = lv_indev_get_next(indev)) {
        lv_indev_enable(indev, false);
    }
    if (!DisplayUiPreparePowerOff()) {
        return false;
    }
    if (Wallpaper_IsOverlay(false)) {
        PrepareWallpaperOverlay();
    }
    // 加载期间保留原帧，提示局部叠加；解码完成后再生成完整关机画面。
    FreezeFlush(true);
    s_defer_standby_commit.store(true, std::memory_order_relaxed);
    s_stream_paint.store(false);

    s_force_full.store(false);
    s_restore_frontlight_after_commit.store(false);
    EpdSetTestActive(false);
    EpdCancelPageTurn();
    LvAdapterEpdiyShutdownWallpaperWork work;
    work.overlay = Wallpaper_IsOverlay(false) && WallpaperFrame_HasBase();
    work.size = WallpaperFrame_Size();
    const int64_t started = esp_timer_get_time();
    if (FirmwareCreateTask(DecodeShutdownWallpaperTask, "off_wp_load", 12 * 1024, &work, 5, nullptr, 0,
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
        ESP_LOGW(TAG, "shutdown wallpaper task create failed; use fallback frame");
        if (!work.overlay) {
            work.have_img = TryDecodeShutdownBuiltin(work.img);
        }
    }
    bool have_img = work.have_img;

    BindStatusWidgets(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    if (work.overlay) {
        have_img = have_img && WallpaperFrame_Draw(work.img, work.alpha);
        if (!have_img) {
            have_img = WallpaperFrame_Restore();
        }
    } else {
        DisplayUiSetFullScreen(true);
        DisplayUiSetOrient(kDisplayUiPortrait);
        if (have_img) {
            have_img = BlitL8ToEpdFullScreen(work.img);
        }
    }

    if (!have_img) {
        FreezeFlush(false);
        if (!ShowPoweredOffTextFallback()) {
            return false;
        }
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

void LvAdapterEpdiy::CancelPowerOff() {
    DisplayLockGuard lock(this);
    if (!lock) {
        return;
    }
    powering_off_ = false;
    FreezeFlush(false);
    s_defer_standby_commit.store(false);
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev)) {
        lv_indev_enable(indev, true);
    }
    RequestFullOnNextCommit();
}

void LvAdapterEpdiy::FreezeFlush(bool on) {
    s_freeze_updates.store(on, std::memory_order_relaxed);
}

void LvAdapterEpdiy::ApplyStatusTextLocked(const char* status) {
    const char* body = (status != nullptr) ? status : "";
    if (std::strcmp(status_body_cache_, body) != 0) {
        std::snprintf(status_body_cache_, sizeof(status_body_cache_), "%s", body);
    }
    if (status_label_ == nullptr) {
        return;
    }
    char buf[sizeof(status_title_prefix_) + sizeof(status_body_cache_) + sizeof("·")];
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
        if (!DisplayUiWaveVisible()) {
            lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
