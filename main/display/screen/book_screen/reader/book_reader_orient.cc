#include <algorithm>
#include <cstdlib>
#include <esp_log.h>
#include <esp_timer.h>
#include "book_screen/book_nav.h"
#include "book_screen/reader/book_reader_orient.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/book_geom.h"
#include "book_screen/reader/book_reader_prefs.h"
#include "display_orient.h"
#include "screen_frontlight.h"
#include "power_policy.h"
#include "epd_refresh.h"
#include "lv_adapter_display.h"
#include "metalio_accel.h"
#include "standby_screen/standby_screen.h"


static constexpr int kUnknownOrient = -2;
static constexpr int64_t kStableUs = 800000;
static constexpr int kGravityThresholdMg = 550;
static constexpr int kAxisDominanceMg = 150;
static lv_timer_t* s_timer = nullptr;
static int s_auto_orient = kBookReaderOrientPortrait;
static int s_candidate = kUnknownOrient;
static int64_t s_candidate_since_us = 0;

static int OrientFromGravity(int x, int y) {
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

static bool ObserveOrient(int orient, int64_t now) {
    if (orient == kUnknownOrient || orient != s_candidate) {
        s_candidate = orient;
        s_candidate_since_us = now;
        return false;
    }
    return now - s_candidate_since_us >= kStableUs;
}

static bool s_shake_sample_valid = false;
struct ReaderShakeSample {
    int x, y, z; // 三轴加速度，单位 mg
    int64_t time; // 采样观察时间，单位微秒
};
static ReaderShakeSample s_shake_samples[6] = {};
static unsigned s_shake_next = 0;
static unsigned s_shake_count = 0;
static int s_shake_orient = kUnknownOrient;
static int64_t s_shake_blocked_until = 0;

static void ObserveShake(int x, int y, int z, int64_t now) {
    auto& st = Book_State();
    const int orient = DisplayUiGetOrient();
    if (!s_shake_sample_valid || orient != s_shake_orient) {
        s_shake_count = 0;
        s_shake_next = 0;

    }
    s_shake_orient = orient;
    s_shake_sample_valid = true;
    if (!BookReaderPrefsShakeTurn() ||
        st.reader.read_chrome != BookUiState::ReadChrome::kReading ||
        (st.settings.settings_sheet && !lv_obj_has_flag(st.settings.settings_sheet, LV_OBJ_FLAG_HIDDEN)) ||
        ScreenFrontlightIsVisible() || TouchUiFingerIsDown() || TouchUiHasPendingEdges() ||
        EpdGetStats().busy || st.reader.layout_busy.load() || st.reader.layout_again || st.reader.layout_debounce_timer || ReaderPageWorkPending() ||
        s_reader_paint.seq.load() != s_reader_paint.done.load()) {
        s_shake_count = 0;
        s_shake_next = 0;
        return;
    }
    if (now < s_shake_blocked_until) {
        s_shake_count = 0;
        s_shake_next = 0;
        return;
    }
    s_shake_samples[s_shake_next] = {x, y, z, now};
    s_shake_next = (s_shake_next + 1) % 6;
    s_shake_count = std::min(s_shake_count + 1, 6u);
    const ReaderShakeSample* oldest = &s_shake_samples[(s_shake_next + 6 - s_shake_count) % 6];
    int min_x = x, max_x = x;
    int min_y = y, max_y = y;
    int min_z = z, max_z = z;
    for (unsigned i = 0; i < s_shake_count; ++i) {
        const auto& sample = s_shake_samples[i];
        if (now - sample.time > 250000) continue;
        if (sample.time < oldest->time || now - oldest->time > 250000) oldest = &sample;
        min_x = std::min(min_x, sample.x);
        max_x = std::max(max_x, sample.x);
        min_y = std::min(min_y, sample.y);
        max_y = std::max(max_y, sample.y);
        min_z = std::min(min_z, sample.z);
        max_z = std::max(max_z, sample.z);
    }
    // 250ms 窗口覆盖一次正常晃动，避免只比较相邻样本漏掉缓和动作。
    const bool landscape = orient == kBookReaderOrientLandLeft || orient == kBookReaderOrientLandRight;
    const int horizontal = landscape ? max_x - min_x : max_y - min_y;
    const int vertical = landscape ? max_y - min_y : max_x - min_x;
    const int depth = max_z - min_z;
    const int level = BookReaderPrefsShakeLevel();
    const int threshold = level <= 3 ? 460 - level * 80 : 400 - level * 60;
    // 左右幅度须占主导；允许手持晃动伴随一定的前后倾斜。
    if (horizontal < threshold || horizontal * 10 < vertical * 11 ||
        horizontal * 2 < depth) {
        return;
    }
    // 正值为屏幕右向变化；左右横屏及倒置竖屏同步反转轴向。
    const int signed_horizontal = landscape ? x - oldest->x : y - oldest->y;
    const int screen_horizontal = orient == kBookReaderOrientPortrait || orient == kBookReaderOrientLandRight ?
        -signed_horizontal : signed_horizontal;
    if (std::abs(screen_horizontal) < threshold) return;
    const int direction = screen_horizontal > 0 ? 1 : -1;
    s_shake_blocked_until = now + 600000;
    if (QueueReaderPageTurn(direction)) {
        PowerPolicy::GetInstance().NotifyUserActivity();
    }
}

static void OnAutoOrientTick(lv_timer_t*) {
    auto& st = Book_State();
    if (!st.reader.read_scr || lv_screen_active() != st.reader.read_scr ||
        st.reader.standby_paused || StandbyScreen::IsActive() ||
        st.reader.opening.load() || st.reader.prefs_reopen_pending ||
        !st.reader.session || !st.reader.session->IsOpen()) {
        MetalioAccel_SetReadingTracking(false);
        s_candidate = kUnknownOrient;
        s_shake_sample_valid = false;

        return;
    }
    MetalioAccel_SetReadingTracking(true);
    int x = 0;
    int y = 0;
    int z = 0;
    if (!MetalioAccel_GetReadingGravity(&x, &y, &z)) {
        s_shake_sample_valid = false;
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (BookReaderPrefsOrient() == kBookReaderOrientAuto) {
        const int orient = OrientFromGravity(x, y);
        const bool stable = ObserveOrient(orient, now);
        if (orient != kUnknownOrient && orient != DisplayUiGetOrient()) {
            // 候选方向开始变化即丢弃晃动窗口，切屏过程不参与翻页判定。
            s_shake_sample_valid = false;
            if (stable && !TouchUiFingerIsDown() && !TouchUiHasPendingEdges() &&
                !EpdGetStats().busy && !st.reader.layout_busy.load() && !st.reader.layout_again &&
                !st.reader.layout_debounce_timer && !ReaderPageWorkPending() &&
                s_reader_paint.seq.load() == s_reader_paint.done.load()) {
                s_auto_orient = orient;
                ApplyReaderOrient(orient);
            }
            return;
        }
    }
    ObserveShake(x, y, z, now);
}


int ReaderDisplayOrient() {
    const int mode = BookReaderPrefsOrient();
    return mode == kBookReaderOrientAuto ? s_auto_orient : mode;
}

void StopReaderAutoOrient() {
    s_shake_sample_valid = false;
    s_shake_blocked_until = 0;
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = nullptr;
    }
    MetalioAccel_SetReadingTracking(false);
    s_candidate = kUnknownOrient;
}

void SyncReaderAutoOrient() {
    StopReaderAutoOrient();
    if (BookReaderPrefsOrient() != kBookReaderOrientAuto && !BookReaderPrefsShakeTurn()) {
        return;
    }
    s_auto_orient = DisplayUiGetOrient();
    s_timer = lv_timer_create(OnAutoOrientTick, 50, nullptr);
    if (!s_timer) {
        ESP_LOGE("ReaderOrient", "orientation timer create failed");
    }
}
