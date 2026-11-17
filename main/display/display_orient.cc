#include "display_orient.h"

#include "config.h"
#include "settings.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>

#include <esp_log.h>
#include <lvgl.h>

static constexpr const char* TAG = "DispOrient";

static std::atomic<int> s_orient = kDisplayUiPortrait;
static bool s_full_screen = false;
static int s_panel_w = DISPLAY_HEIGHT; // ED047 物理宽（横置）
static int s_panel_h = DISPLAY_WIDTH;  // ED047 物理高
static lv_display_t* s_disp = nullptr;

static int ClampOrient(int orient) {
    if (orient < 0 || orient >= kDisplayUiOrientCount) {
        return kDisplayUiPortrait;
    }
    return orient;
}

static void LogicalToPanelAt(int orient, int lx, int ly, int* px, int* py) {
    const int pw = s_panel_w;
    const int ph = s_panel_h;
    const int left = s_full_screen ? 0 : DISPLAY_CONTENT_LEFT_INSET;
    const int top = s_full_screen ? 0 : DISPLAY_CONTENT_TOP_INSET;
    lx += s_full_screen ? 0 : DisplayUiOffsetX();
    ly += s_full_screen ? 0 : DisplayUiOffsetY();
    switch (ClampOrient(orient)) {
        case kDisplayUiLandLeft:
            *px = lx + top;
            *py = ly + left;
            break;
        case kDisplayUiLandRight:
            *px = pw - 1 - lx - top;
            *py = ph - 1 - ly - left;
            break;
        case kDisplayUiPortraitDown:
            *px = pw - 1 - ly - top;
            *py = lx + left;
            break;
        case kDisplayUiPortrait:
        default:
            *px = ly + top;
            *py = ph - 1 - lx - left;
            break;
    }
}

static void TouchToLogicalAt(int orient, int tx, int ty, int* lx, int* ly) {
    const int pw = s_panel_w;
    const int ph = s_panel_h;
    const int left = s_full_screen ? 0 : DISPLAY_CONTENT_LEFT_INSET;
    const int top = s_full_screen ? 0 : DISPLAY_CONTENT_TOP_INSET;
    switch (ClampOrient(orient)) {
        case kDisplayUiLandLeft:
            *lx = ty - top;
            *ly = ph - 1 - tx - left;
            break;
        case kDisplayUiLandRight:
            *lx = pw - 1 - ty - top;
            *ly = tx - left;
            break;
        case kDisplayUiPortraitDown:
            *lx = ph - 1 - tx - left;
            *ly = pw - 1 - ty - top;
            break;
        case kDisplayUiPortrait:
        default:
            *lx = tx - left;
            *ly = ty - top;
            break;
    }
    *lx -= s_full_screen ? 0 : DisplayUiOffsetX();
    *ly -= s_full_screen ? 0 : DisplayUiOffsetY();
}

static void FailCheck(const char* what) {
    ESP_LOGE(TAG, "self-check failed: %s", what);
    abort();
}

static void SelfCheck() {
    for (int orient = 0; orient < kDisplayUiOrientCount; ++orient) {
        const int w = (orient == kDisplayUiPortrait || orient == kDisplayUiPortraitDown) ? DisplayUiContentW() : DisplayUiContentH();
        const int h = (orient == kDisplayUiPortrait || orient == kDisplayUiPortraitDown) ? DisplayUiContentH() : DisplayUiContentW();
        for (int x = 0; x < w; x += w - 1) {
            for (int y = 0; y < h; y += h - 1) {
                int px, py, lx, ly;
                LogicalToPanelAt(orient, x, y, &px, &py);
                TouchToLogicalAt(orient, s_panel_h - 1 - py, px, &lx, &ly);
                if (lx != x || ly != y) {
                    FailCheck("orientation round-trip");
                }
            }
        }
    }
}

static int s_default_offsets[kDisplayUiOffsetCount] = {};
static std::atomic<int> s_offsets[kDisplayUiOffsetCount] = {};

int DisplayUiOffset(int side) {
    return side >= 0 && side < kDisplayUiOffsetCount ? s_offsets[side].load() : 0;
}

bool DisplayUiSetOffset(int side, int value) {
    if (side < 0 || side >= kDisplayUiOffsetCount) return false;
    value = std::clamp<int>(value, kDisplayUiOffsetMin, kDisplayUiOffsetMax);
    if (s_offsets[side].exchange(value) == value) return false;
    if (s_disp != nullptr) lv_display_send_event(s_disp, LV_EVENT_REFRESH, nullptr);
    return true;
}

bool DisplayUiResetOffsets(void) {
    bool changed = false;
    for (int side = 0; side < kDisplayUiOffsetCount; ++side) {
        changed |= s_offsets[side].exchange(s_default_offsets[side]) != s_default_offsets[side];
    }
    if (changed && s_disp != nullptr) lv_display_send_event(s_disp, LV_EVENT_REFRESH, nullptr);
    return changed;
}

void DisplayUiSetOffsetDefaults(void) {
    for (int side = 0; side < kDisplayUiOffsetCount; ++side) s_default_offsets[side] = s_offsets[side].load();
}

int DisplayUiOffsetX(void) {
    return s_offsets[3].load() - s_offsets[2].load();
}

int DisplayUiOffsetY(void) {
    return s_offsets[1].load() - s_offsets[0].load();
}

int DisplayUiContentW(void) {
    return s_panel_h - DISPLAY_CONTENT_LEFT_INSET - DISPLAY_CONTENT_RIGHT_INSET;
}

int DisplayUiContentH(void) {
    return s_panel_w - DISPLAY_CONTENT_TOP_INSET - DISPLAY_CONTENT_BOTTOM_INSET;
}

void DisplayUiBind(lv_display_t* disp, int panel_w, int panel_h) {
    s_disp = disp;
    if (panel_w > 0 && panel_h > 0) {
        s_panel_w = panel_w;
        s_panel_h = panel_h;
    }
    s_orient = kDisplayUiPortrait;
    s_full_screen = false;
    Settings settings("screen");
    const char* keys[] = {"offset_top", "offset_bottom", "offset_left", "offset_right"};
    const char* default_keys[] = {"off_def_top", "off_def_bottom", "off_def_left", "off_def_right"};
    for (int i = 0; i < kDisplayUiOffsetCount; ++i) {
        s_default_offsets[i] = std::clamp<int>(settings.GetInt(default_keys[i], 0), kDisplayUiOffsetMin, kDisplayUiOffsetMax);
        s_offsets[i] = std::clamp<int>(settings.GetInt(keys[i], s_default_offsets[i]), kDisplayUiOffsetMin, kDisplayUiOffsetMax);
    }
    SelfCheck();
    DisplayUiSetOrient(s_orient);
}

bool DisplayUiSetOrient(int orient) {
    orient = ClampOrient(orient);
    const bool changed = (s_orient != orient);
    s_orient = orient;
    const int log_w = DisplayUiLogW();
    const int log_h = DisplayUiLogH();
    if (s_disp != nullptr) {
        if (lv_display_get_horizontal_resolution(s_disp) != log_w ||
            lv_display_get_vertical_resolution(s_disp) != log_h) {
            lv_display_set_resolution(s_disp, log_w, log_h);
        }
    }
    if (changed) {
        ESP_LOGI(TAG, "orient=%d log=%dx%d offset=%d,%d", orient, log_w, log_h,
                 DisplayUiOffsetX(), DisplayUiOffsetY());
    }
    return changed;
}

int DisplayUiGetOrient(void) {
    return s_orient;
}

void DisplayUiSetFullScreen(bool full_screen) {
    if (s_full_screen == full_screen) return;
    s_full_screen = full_screen;
    DisplayUiSetOrient(s_orient);
}

bool DisplayUiIsLandscape(void) {
    return s_orient == kDisplayUiLandLeft || s_orient == kDisplayUiLandRight;
}

int DisplayUiLogW(void) {
    const int w = s_full_screen ? s_panel_h : DisplayUiContentW();
    const int h = s_full_screen ? s_panel_w : DisplayUiContentH();
    return DisplayUiIsLandscape() ? h : w;
}

int DisplayUiLogH(void) {
    const int w = s_full_screen ? s_panel_h : DisplayUiContentW();
    const int h = s_full_screen ? s_panel_w : DisplayUiContentH();
    return DisplayUiIsLandscape() ? w : h;
}

void DisplayUiLogicalToPanel(int lx, int ly, int* px, int* py) {
    if (px == nullptr || py == nullptr) {
        return;
    }
    LogicalToPanelAt(s_orient, lx, ly, px, py);
}

void DisplayUiTouchToLogical(int tx, int ty, int* lx, int* ly) {
    if (lx == nullptr || ly == nullptr) {
        return;
    }
    TouchToLogicalAt(s_orient, tx, ty, lx, ly);
}
