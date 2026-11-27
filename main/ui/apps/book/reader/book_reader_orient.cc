#include <algorithm>
#include <cstdlib>
#include <esp_log.h>
#include <esp_timer.h>
#include "ui/apps/book/reader/book_reader_orient.h"
#include "boards/common/sensor/sc7a20h/accelerometer.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "ui/common/drawing/adapter/epd_refresh.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/common/drawing/display_orient.h"
#include "ui/apps/book/book_geom.h"
#include "ui/apps/book/book_nav.h"
#include "ui/apps/book/book_screen_priv.h"
#include "business/reader/book_reader_prefs.h"
#include "ui/common/widgets/screen_frontlight.h"
#include "ui/apps/standby/standby_screen.h"

static constexpr int kUnknownOrient = -2;
static constexpr const char* kOrientTag = "ReaderOrient";
static constexpr unsigned kShakeSampleCount = 6; // 50ms 采样覆盖 250ms 晃动窗口
static constexpr int64_t kStableUs = 800000;          // 自动旋转需连续稳定的时间
static constexpr int64_t kShakeWindowUs = 250000;     // 一次晃动的观察窗口
static constexpr int64_t kShakeCooldownUs = 600000;   // 识别成功后的冷却
static constexpr int kShakeLevelSplit = 3;            // 1–3 级与 4–5 级使用不同步进
static constexpr int kShakeBaseLowMg = 460;           // 1–3 级阈值起点，单位 mg
static constexpr int kShakeStepLowMg = 80;            // 1–3 级每提高一档减少的幅度，单位 mg
static constexpr int kShakeBaseHighMg = 400;          // 4–5 级阈值起点，单位 mg
static constexpr int kShakeStepHighMg = 60;           // 4–5 级每提高一档减少的幅度，单位 mg
static constexpr int kShakeVerticalNum = 10;          // 横向幅度相对纵向的比较分子
static constexpr int kShakeVerticalDen = 11;          // 横向幅度至少达到纵向的 11/10
static constexpr int kShakeDepthScale = 2;            // 横向幅度至少达到前后幅度的一半
static lv_timer_t* s_timer = nullptr;
static int s_auto_orient = kBookReaderOrientPortrait;
static int s_candidate = kUnknownOrient;
static int64_t s_candidate_since_us = 0;
static bool s_shake_sample_valid = false;
static int64_t s_shake_blocked_until = 0;

struct ReaderShakeSample {
    int x;         // X 轴加速度，单位 mg
    int y;         // Y 轴加速度，单位 mg
    int z;         // Z 轴加速度，单位 mg
    int64_t time;  // 采样时间，单位微秒
};

static ReaderShakeSample s_shake_samples[kShakeSampleCount] = {};
static unsigned s_shake_next = 0;
static unsigned s_shake_count = 0;
static int s_shake_orient = kUnknownOrient;

static bool ObserveOrient(int orient, int64_t now) {
    if (orient == kUnknownOrient || orient != s_candidate) {
        s_candidate = orient;
        s_candidate_since_us = now;
        return false;
    }
    return now - s_candidate_since_us >= kStableUs;
}

static void ClearShakeWindow() {
    s_shake_count = 0;
    s_shake_next = 0;
}

static int ShakeThresholdMg(int level) {
    if (level <= kShakeLevelSplit) {
        return kShakeBaseLowMg - level * kShakeStepLowMg;
    }
    return kShakeBaseHighMg - level * kShakeStepHighMg;
}

static bool ReaderMotionBusy(const BookUiState& st) {
    return TouchUiFingerIsDown() || TouchUiHasPendingEdges() || EpdGetStats().busy || st.reader.layout_busy.load() ||
           st.reader.layout_again || st.reader.layout_debounce_timer || ReaderPageWorkPending() ||
           s_reader_paint.seq.load() != s_reader_paint.done.load();
}

static void ObserveShake(int x, int y, int z, int64_t now) {
    auto& st = Book_State();
    const int orient = DisplayUiGetOrient();
    if (!s_shake_sample_valid || orient != s_shake_orient) {
        ClearShakeWindow();
    }
    s_shake_orient = orient;
    s_shake_sample_valid = true;
    const bool paused = !BookReaderPrefsShakeTurn() || st.reader.read_chrome != BookUiState::ReadChrome::kReading ||
                        (st.settings.settings_sheet && !lv_obj_has_flag(st.settings.settings_sheet, LV_OBJ_FLAG_HIDDEN)) ||
                        ScreenFrontlightIsVisible() || ReaderMotionBusy(st) || now < s_shake_blocked_until;
    if (paused) {
        ClearShakeWindow();
        return;
    }
    s_shake_samples[s_shake_next] = {x, y, z, now};
    s_shake_next = (s_shake_next + 1) % kShakeSampleCount;
    s_shake_count = std::min(s_shake_count + 1, kShakeSampleCount);
    const ReaderShakeSample* oldest =
        &s_shake_samples[(s_shake_next + kShakeSampleCount - s_shake_count) % kShakeSampleCount];
    int min_x = x;
    int max_x = x;
    int min_y = y;
    int max_y = y;
    int min_z = z;
    int max_z = z;
    for (unsigned i = 0; i < s_shake_count; ++i) {
        const auto& sample = s_shake_samples[i];
        if (now - sample.time > kShakeWindowUs) {
            continue;
        }
        if (sample.time < oldest->time || now - oldest->time > kShakeWindowUs) {
            oldest = &sample;
        }
        min_x = std::min(min_x, sample.x);
        max_x = std::max(max_x, sample.x);
        min_y = std::min(min_y, sample.y);
        max_y = std::max(max_y, sample.y);
        min_z = std::min(min_z, sample.z);
        max_z = std::max(max_z, sample.z);
    }
    // 观察窗口覆盖一次正常晃动，避免只比较相邻样本漏掉缓和动作。
    const bool landscape = orient == kBookReaderOrientLandLeft || orient == kBookReaderOrientLandRight;
    const int horizontal = landscape ? max_x - min_x : max_y - min_y;
    const int vertical = landscape ? max_y - min_y : max_x - min_x;
    const int depth = max_z - min_z;
    const int threshold = ShakeThresholdMg(BookReaderPrefsShakeLevel());
    // 左右幅度须占主导；允许手持晃动伴随一定的前后倾斜。
    if (horizontal < threshold || horizontal * kShakeVerticalNum < vertical * kShakeVerticalDen ||
        horizontal * kShakeDepthScale < depth) {
        return;
    }
    // 正值表示相对当前屏幕向右。竖屏和右横屏的传感器增量取反，左横屏和倒置竖屏保持原符号。
    const int signed_horizontal = landscape ? x - oldest->x : y - oldest->y;
    int screen_horizontal = signed_horizontal;
    if (orient == kBookReaderOrientPortrait || orient == kBookReaderOrientLandRight) {
        screen_horizontal = -signed_horizontal;
    }
    if (std::abs(screen_horizontal) < threshold) {
        return;
    }
    const int direction = screen_horizontal > 0 ? 1 : -1;
    s_shake_blocked_until = now + kShakeCooldownUs;
    if (QueueReaderPageTurn(direction)) {
        PowerPolicy::GetInstance().NotifyUserActivity();
    }
}

static void OnAutoOrientTick(lv_timer_t*) {
    auto& st = Book_State();
    if (!st.reader.read_scr || lv_screen_active() != st.reader.read_scr || st.reader.standby_paused ||
        StandbyPresentation::IsActive() || st.reader.opening.load() || st.reader.prefs_reopen_pending || !st.reader.session ||
        !st.reader.session->IsOpen()) {
        Accelerometer_SetReadingTracking(false, false);
        s_candidate = kUnknownOrient;
        s_shake_sample_valid = false;
        return;
    }
    Accelerometer_SetReadingTracking(true, BookReaderPrefsShakeTurn());
    int x = 0;
    int y = 0;
    int z = 0;
    if (!Accelerometer_GetReadingGravity(&x, &y, &z)) {
        s_shake_sample_valid = false;
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (BookReaderPrefsOrient() == kBookReaderOrientAuto) {
        int orient = kUnknownOrient;
        switch (Accelerometer_GetReadingOrientation()) {
            case AccelerometerOrientationPortrait:
                orient = kBookReaderOrientPortrait;
                break;
            case AccelerometerOrientationLeft:
                orient = kBookReaderOrientLandLeft;
                break;
            case AccelerometerOrientationRight:
                orient = kBookReaderOrientLandRight;
                break;
            default:
                break;
        }
        const bool stable = ObserveOrient(orient, now);
        if (orient != kUnknownOrient && orient != DisplayUiGetOrient()) {
            // 候选方向开始变化即丢弃晃动窗口，切屏过程不参与翻页判定。
            s_shake_sample_valid = false;
            if (stable && !ReaderMotionBusy(st)) {
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

void SyncReaderAutoOrient() {
    StopReaderAutoOrient();
    if (BookReaderPrefsOrient() != kBookReaderOrientAuto && !BookReaderPrefsShakeTurn()) {
        return;
    }
    s_auto_orient = DisplayUiGetOrient();
    s_timer = lv_timer_create(OnAutoOrientTick, BookReaderPrefsShakeTurn() ? 50 : 200, nullptr);
    if (!s_timer) {
        ESP_LOGE(kOrientTag, "orientation timer create failed");
    }
}

void StopReaderAutoOrient() {
    s_shake_sample_valid = false;
    s_shake_blocked_until = 0;
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = nullptr;
    }
    Accelerometer_SetReadingTracking(false, false);
    s_candidate = kUnknownOrient;
}
