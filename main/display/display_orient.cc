#include "display_orient.h"

#include "config.h"

#include <cstdlib>

#include <esp_log.h>
#include <lvgl.h>

namespace {

constexpr const char* TAG = "DispOrient";

int s_orient = kDisplayUiPortrait;
int s_panel_w = 1216; // ED047 物理宽（横置）
int s_panel_h = 684;  // ED047 物理高
lv_display_t* s_disp = nullptr;

int ClampOrient(int orient) {
    if (orient < 0 || orient >= kDisplayUiOrientCount) {
        return kDisplayUiPortrait;
    }
    return orient;
}

void LogicalToPanelAt(int orient, int lx, int ly, int* px, int* py) {
    const int pw = s_panel_w;
    const int ph = s_panel_h;
    switch (ClampOrient(orient)) {
        case kDisplayUiLandLeft:
            *px = lx;
            *py = ly;
            break;
        case kDisplayUiLandRight:
            *px = pw - 1 - lx;
            *py = ph - 1 - ly;
            break;
        case kDisplayUiPortrait:
        default:
            *px = ly;
            *py = ph - 1 - lx;
            break;
    }
}

void TouchToLogicalAt(int orient, int tx, int ty, int* lx, int* ly) {
    const int pw = s_panel_w;
    const int ph = s_panel_h;
    switch (ClampOrient(orient)) {
        case kDisplayUiLandLeft:
            *lx = ty;
            *ly = ph - 1 - tx;
            break;
        case kDisplayUiLandRight:
            *lx = pw - 1 - ty;
            *ly = tx;
            break;
        case kDisplayUiPortrait:
        default:
            *lx = tx;
            *ly = ty;
            break;
    }
}

void FailCheck(const char* what) {
    ESP_LOGE(TAG, "self-check failed: %s", what);
    abort();
}

void SelfCheck() {
    int px = 0, py = 0, lx = 0, ly = 0;
    LogicalToPanelAt(kDisplayUiPortrait, 0, 0, &px, &py);
    if (px != 0 || py != s_panel_h - 1) {
        FailCheck("portrait (0,0)");
    }
    LogicalToPanelAt(kDisplayUiPortrait, s_panel_h - 1, 0, &px, &py);
    if (px != 0 || py != 0) {
        FailCheck("portrait (h-1,0)");
    }
    LogicalToPanelAt(kDisplayUiLandLeft, 0, 0, &px, &py);
    if (px != 0 || py != 0) {
        FailCheck("land-left (0,0)");
    }
    LogicalToPanelAt(kDisplayUiLandRight, 0, 0, &px, &py);
    if (px != s_panel_w - 1 || py != s_panel_h - 1) {
        FailCheck("land-right (0,0)");
    }
    TouchToLogicalAt(kDisplayUiPortrait, 12, 34, &lx, &ly);
    if (lx != 12 || ly != 34) {
        FailCheck("touch portrait");
    }
    TouchToLogicalAt(kDisplayUiLandLeft, 0, 0, &lx, &ly);
    if (lx != 0 || ly != s_panel_h - 1) {
        FailCheck("touch land-left");
    }
    LogicalToPanelAt(kDisplayUiLandLeft, lx, ly, &px, &py);
    if (px != 0 || py != s_panel_h - 1) {
        FailCheck("touch/panel land-left");
    }
}

}  // namespace

void DisplayUiBind(lv_display_t* disp, int panel_w, int panel_h) {
    s_disp = disp;
    if (panel_w > 0 && panel_h > 0) {
        s_panel_w = panel_w;
        s_panel_h = panel_h;
    }
    s_orient = kDisplayUiPortrait;
    SelfCheck();
}

bool DisplayUiSetOrient(int orient) {
    orient = ClampOrient(orient);
    // S31 LVGL 缓冲按竖屏 content 分配；横屏需改 flush/缓冲，暂锁竖屏
    if (orient != kDisplayUiPortrait) {
        ESP_LOGW(TAG, "landscape deferred on S31; keep portrait");
        orient = kDisplayUiPortrait;
    }
    // UI 逻辑分辨率必须是 content 区（已扣 L/R/T/B inset），禁止改回面板全尺寸
    const int log_w = DISPLAY_CONTENT_W;
    const int log_h = DISPLAY_CONTENT_H;
    const bool changed = (s_orient != orient);
    s_orient = orient;
    if (s_disp != nullptr) {
        if (lv_display_get_horizontal_resolution(s_disp) != log_w ||
            lv_display_get_vertical_resolution(s_disp) != log_h) {
            lv_display_set_resolution(s_disp, log_w, log_h);
        }
    }
    if (changed) {
        ESP_LOGI(TAG, "orient=%d log=%dx%d (content inset L%d R%d T%d B%d)", orient, log_w, log_h,
                 DISPLAY_CONTENT_LEFT_INSET, DISPLAY_CONTENT_RIGHT_INSET, DISPLAY_CONTENT_TOP_INSET,
                 DISPLAY_CONTENT_BOTTOM_INSET);
    }
    return changed;
}

int DisplayUiGetOrient(void) {
    return s_orient;
}

bool DisplayUiIsLandscape(void) {
    return s_orient != kDisplayUiPortrait;
}

int DisplayUiLogW(void) {
    return (s_orient == kDisplayUiPortrait) ? DISPLAY_CONTENT_W : DISPLAY_CONTENT_H;
}

int DisplayUiLogH(void) {
    return (s_orient == kDisplayUiPortrait) ? DISPLAY_CONTENT_H : DISPLAY_CONTENT_W;
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
