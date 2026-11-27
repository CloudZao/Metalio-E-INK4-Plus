#include "ui/apps/settings/settings_test/settings_test_display_screen.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "config.h"
#include "ui/common/drawing/adapter/epd_refresh.h"

#if CONFIG_DISPLAY_BACKEND_EPDIY
#include "boards/common/display/epdiy/epdiy.h"

#else
static constexpr int MODE_GC16 = 0;
#endif
#include <cstring>
#include "sysmgmt/power/policy/power_policy.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/widgets/screen_text.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/runtime/screen_common_nav.h"

static lv_obj_t* s_screen = nullptr;
static lv_obj_t* s_note = nullptr;
static lv_obj_t* s_preview = nullptr;
static int s_pattern = 0;

static constexpr uint8_t kBayer4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

static void OnPreviewDraw(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) {
        return;
    }
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto* layer = lv_event_get_layer(e);
    if (obj == nullptr || layer == nullptr) {
        return;
    }

    lv_area_t a;
    lv_obj_get_content_coords(obj, &a);
    const int w = a.x2 - a.x1 + 1;
    const int h = a.y2 - a.y1 + 1;
    if (w <= 0 || h <= 0) {
        return;
    }

    lv_draw_rect_dsc_t white;
    lv_draw_rect_dsc_init(&white);
    white.bg_color = lv_color_white();
    white.bg_opa = LV_OPA_COVER;
    lv_draw_rect(layer, &white, &a);

    lv_draw_rect_dsc_t black;
    lv_draw_rect_dsc_init(&black);
    black.bg_color = lv_color_black();
    black.bg_opa = LV_OPA_COVER;

    const int bar_h = h / 16;
    for (int level = 0; level < 16; ++level) {
        const int y0 = a.y1 + level * bar_h;
        const int y1 = (level == 15) ? a.y2 : (y0 + bar_h - 1);
        for (int py = y0; py <= y1; ++py) {
            int run_x1 = -1;
            for (int px = a.x1; px <= a.x2 + 1; ++px) {
                bool ink = false;
                if (px <= a.x2) {
                    const int lx = px - a.x1;
                    const int ly = py - y0;
                    if (s_pattern == 0) {
                        ink = kBayer4[ly & 3][lx & 3] < level;
                    } else {
                        const int period = 2 + (level / 4);
                        ink = level >= 15 || (level > 0 && ((lx / period) + (ly / period)) % 2 == 0);
                    }
                }
                if (ink) {
                    if (run_x1 < 0) {
                        run_x1 = px;
                    }
                } else if (run_x1 >= 0) {
                    lv_area_t run{run_x1, py, px - 1, py};
                    lv_draw_rect(layer, &black, &run);
                    run_x1 = -1;
                }
            }
        }
    }
}

static lv_obj_t* Label(lv_obj_t* parent, const char* text, int x, int y, int width, int height = 64) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, UiFont(), 0);
    lv_obj_set_style_text_color(label, UiColorForeground(), 0);
    ScreenFitLabel(label, width, height);
    return label;
}

static void RunGray() {
    EpdSetTestActive(true);
    // 原生灰度路径在 GC16 前执行物理清屏，不使用 DU。
    bool ok = true;
    #if CONFIG_DISPLAY_BACKEND_EPDIY
    ok = EpdTestRequest(EpdPattern::Gray, MODE_GC16, true);
    #else
    s_pattern = 0;
    if (s_preview) {
        lv_obj_invalidate(s_preview);
    }
    if (auto* display = LVAdapterDisplay::Instance()) {
        display->RequestFullOnNextCommit();
    }
    #endif
    if (s_note) {
        lv_label_set_text(s_note, ok ? LangString_SETTINGS_DISPLAY_GRAY_HINT : LangString_SETTINGS_DISPLAY_BUSY);
    }
}

static void Button(lv_obj_t* parent, const char* text, int x, int y, int width, lv_event_cb_t callback) {
    auto* button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, 64);
    lv_obj_set_style_bg_color(button, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(button, UiColorForeground(), 0);
    lv_obj_set_style_border_width(button, 2, 0);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    auto* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, UiFont(), 0);
    lv_obj_update_layout(button);
    ScreenFitLabel(label, lv_obj_get_content_width(button), lv_obj_get_content_height(button));
    lv_obj_center(label);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
}

static bool Key(const char* key) {
    if (!std::strcmp(key, "vk_next")) {
        RunGray();
        return true;
    }
    return false;
}

lv_obj_t* SettingsTestDisplayScreen::Create() {
    auto* screen = lv_obj_create(nullptr);
    s_screen = screen;
    lv_obj_set_style_bg_color(screen, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, UiColorForeground(), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    ScreenSetIsHome(false);
    ScreenCreateStatusBar(screen);

    const bool mono = EpdGetFrameBuffer().bits_per_pixel == 1;
    const int pad = mono ? 16 : 24;
    const int content_w = mono ? LV_HOR_RES - pad * 2 : 620;
    Label(screen, LangString_SETTINGS_TEST_DISPLAY, pad, mono ? 52 : 76, content_w);
    Label(screen, LangString_SETTINGS_DISPLAY_START_HINT, pad, mono ? 90 : 130, content_w, mono ? 96 : 110);
    Button(screen, LangString_SETTINGS_DISPLAY_GRAY, pad, mono ? 194 : 250, mono ? (content_w - 12) / 2 : 300,
           [](lv_event_t*) { RunGray(); });
    Button(screen, LangString_SETTINGS_DISPLAY_DITHER, mono ? pad + (content_w + 12) / 2 : 344, mono ? 194 : 250,
           mono ? (content_w - 12) / 2 : 300, [](lv_event_t*) {
               EpdSetTestActive(true);
               bool ok = true;
               #if CONFIG_DISPLAY_BACKEND_EPDIY
               ok = EpdTestRequest(EpdPattern::Dither, MODE_GC16, true);
               #else
        s_pattern = 1;
        if (s_preview) {
            lv_obj_invalidate(s_preview);
        }
        if (auto* display = LVAdapterDisplay::Instance()) {
            display->RequestFullOnNextCommit();
        }
               #endif
               if (s_note)
                   lv_label_set_text(s_note,
                                     ok ? LangString_SETTINGS_DISPLAY_DITHER_HINT : LangString_SETTINGS_DISPLAY_BUSY);
           });
    s_note = Label(screen, LangString_SETTINGS_DISPLAY_RETEST_HINT, pad, mono ? 268 : 350, content_w, mono ? 60 : 170);

    // 与原生 Pattern() 预览区域一致（逻辑 y=540..1089）。
    auto* frame = lv_obj_create(screen);
    lv_obj_set_pos(frame, mono ? pad : 22, mono ? 336 : 538);
    lv_obj_set_size(frame, mono ? content_w : 624, mono ? 348 : 554);
    if (mono) {
        s_preview = frame;
        lv_obj_add_event_cb(frame, OnPreviewDraw, LV_EVENT_DRAW_MAIN, nullptr);
    }
    lv_obj_set_style_bg_color(frame, UiColorBackground(), 0);
    lv_obj_set_style_border_color(frame, UiColorForeground(), 0);
    lv_obj_set_style_border_width(frame, 2, 0);
    lv_obj_set_style_radius(frame, 0, 0);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    Button(screen, LangString_SETTINGS_TEST_BACK, pad, mono ? LV_VER_RES - 76 : 1110, content_w,
           [](lv_event_t*) { ScreenNavigateBack(); });

    lv_obj_add_event_cb(
        screen,
        [](lv_event_t* e) {
            if (lv_event_get_target(e) != s_screen) {
                return;
            }
            s_screen = s_note = s_preview = nullptr;
            EpdSetTestActive(false);
            PowerPolicy::GetInstance().Release(PowerNeed::StandbyInhibit);
        },
        LV_EVENT_DELETE, nullptr);
    VkKey_AttachScreen(screen, "settings_test_display", {.factory = SettingsTestDisplayScreen::Create, .on_key = Key});
    EpdSetTestActive(true);
    PowerPolicy::GetInstance().Acquire(PowerNeed::StandbyInhibit);
    if (auto* display = LVAdapterDisplay::Instance()) {
        display->RequestFullOnNextCommit();
    }
    return screen;
}
