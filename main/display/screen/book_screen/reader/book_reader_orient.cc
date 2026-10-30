#include "book_screen/book_nav.h"
#include "book_screen/reader/book_reader_orient.h"

#include "book_screen/book_screen_priv.h"
#include "book_screen/book_geom.h"
#include "book_screen/reader/book_reader_prefs.h"
#include "display_orient.h"
#include "epd_refresh.h"
#include "lv_adapter_display.h"
#include "metalio_accel.h"
#include "standby_screen/standby_screen.h"

#include <cstdlib>
#include <esp_log.h>
#include <esp_timer.h>

namespace {
constexpr int kUnknownOrient = -2;
constexpr int64_t kStableUs = 800000;
constexpr int kGravityThresholdMg = 550;
constexpr int kAxisDominanceMg = 150;
lv_timer_t* s_timer = nullptr;
int s_auto_orient = kBookReaderOrientPortrait;
int s_candidate = kUnknownOrient;
int64_t s_candidate_since_us = 0;

int OrientFromGravity(int x, int y) {
    const int ax = std::abs(x);
    const int ay = std::abs(y);
    // 板上 X 轴对应竖屏，Y 正向对应左横；轴向不明确时保持当前方向。
    if (ay >= kGravityThresholdMg && ay >= ax + kAxisDominanceMg) {
        return y > 0 ? kBookReaderOrientLandLeft : kBookReaderOrientLandRight;
    }
    if (ax >= kGravityThresholdMg && ax >= ay + kAxisDominanceMg) {
        return kBookReaderOrientPortrait;
    }
    return kUnknownOrient;
}

bool ObserveOrient(int orient, int64_t now) {
    if (orient == kUnknownOrient || orient != s_candidate) {
        s_candidate = orient;
        s_candidate_since_us = now;
        return false;
    }
    return now - s_candidate_since_us >= kStableUs;
}

void OnAutoOrientTick(lv_timer_t*) {
    auto& st = Book_State();
    if (!st.reader.read_scr || lv_screen_active() != st.reader.read_scr ||
        st.reader.standby_paused || StandbyScreen::IsActive() ||
        st.reader.opening.load() || st.reader.prefs_reopen_pending ||
        !st.reader.session || !st.reader.session->IsOpen()) {
        MetalioAccel_SetReadingTracking(false);
        s_candidate = kUnknownOrient;
        return;
    }
    MetalioAccel_SetReadingTracking(true);
    int x = 0;
    int y = 0;
    const int orient = MetalioAccel_GetReadingGravity(&x, &y) ? OrientFromGravity(x, y) : kUnknownOrient;
    if (!ObserveOrient(orient, esp_timer_get_time()) || orient == DisplayUiGetOrient()) {
        return;
    }
    if (TouchUiFingerIsDown() || TouchUiHasPendingEdges() || EpdGetStats().busy ||
        st.reader.layout_debounce_timer || ReaderPageWorkPending() ||
        s_reader_paint.seq.load() != s_reader_paint.done.load()) {
        return;
    }
    s_auto_orient = orient;
    ApplyReaderOrient(orient);
}
} // namespace

int ReaderDisplayOrient() {
    const int mode = BookReaderPrefsOrient();
    return mode == kBookReaderOrientAuto ? s_auto_orient : mode;
}

void StopReaderAutoOrient() {
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = nullptr;
    }
    MetalioAccel_SetReadingTracking(false);
    s_candidate = kUnknownOrient;
}

void SyncReaderAutoOrient() {
    StopReaderAutoOrient();
    if (BookReaderPrefsOrient() != kBookReaderOrientAuto) {
        return;
    }
    s_auto_orient = DisplayUiGetOrient();
    s_timer = lv_timer_create(OnAutoOrientTick, 200, nullptr);
    if (!s_timer) {
        ESP_LOGE("ReaderOrient", "orientation timer create failed");
    }
}
