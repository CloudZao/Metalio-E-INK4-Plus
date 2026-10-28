#include "screen_frontlight.h"
#include "book_screen/reader/book_auto_turn.h"
#include "book_screen/reader/book_reader_prefs.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/book_nav.h"
#include "standby_screen/standby_screen.h"
#include "lv_adapter_display.h"
#include "power_policy.h"
#include <esp_timer.h>
#include <atomic>

namespace {
lv_timer_t* s_auto_turn_timer = nullptr;
std::atomic<bool> s_auto_turn_enabled{false};
std::atomic<bool> s_auto_turn_pending{false};
bool s_auto_turn_ready = false;
std::atomic<int64_t> s_auto_turn_due_us{0};

bool AutoTurnReadingVisible() {
    const auto& st = Book_State();
    auto visible = [](lv_obj_t* obj) {
        return obj && lv_obj_is_valid(obj) && !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    };
    return st.reader.read_scr && lv_obj_is_valid(st.reader.read_scr) &&
        lv_screen_active() == st.reader.read_scr && !StandbyScreen::IsActive() &&
        st.reader.read_chrome == BookUiState::ReadChrome::kReading &&
        !visible(st.settings.settings_sheet) && !ScreenFrontlightIsVisible() &&
        st.reader.session && st.reader.session->IsOpen() && !st.reader.prefs_reopen_pending;
}

void OnReaderAutoTurnTick(lv_timer_t*) {
    auto& st = Book_State();
    if (!s_auto_turn_enabled) return;
    // Keep one full interval after menus, layout, images, touch or a pending paint.
    if (!AutoTurnReadingVisible() || ReaderWorkersBusy() ||
        st.reader.session->LiveRelayoutPending() || TouchUiFingerIsDown() ||
        TouchUiHasPendingEdges() || s_auto_turn_pending.load() || s_reader_page_delta.load() != 0 ||
        s_reader_paint.seq.load() != s_reader_paint.done.load()) {
        s_auto_turn_ready = false;
        ResetReaderAutoTurnDeadline();
        return;
    }
    if (!st.reader.session->HasNextPage()) {
        StopReaderAutoTurn();
        return;
    }
    if (!s_auto_turn_ready) {
        s_auto_turn_ready = true;
        ResetReaderAutoTurnDeadline();
        return;
    }
    if (esp_timer_get_time() < s_auto_turn_due_us) return;
    ResetReaderAutoTurnDeadline();
    // Keep auto turns separate so a manual event can cancel one before rendering.
    s_auto_turn_pending.store(true);
    RequestRenderReaderPage();
}
}  // namespace

bool ReaderAutoTurnEnabled() { return s_auto_turn_enabled; }

void ResetReaderAutoTurnDeadline() {
    s_auto_turn_due_us = esp_timer_get_time() +
        static_cast<int64_t>(BookReaderPrefsAutoTurnSeconds()) * 1000000;
}

void SetReaderAutoTurnEnabled(bool enabled) {
    if (!enabled) { StopReaderAutoTurn(); return; }
    if (!s_auto_turn_timer) s_auto_turn_timer = lv_timer_create(OnReaderAutoTurnTick, 200, nullptr);
    s_auto_turn_enabled = s_auto_turn_timer != nullptr;
    s_auto_turn_ready = false;
    ResetReaderAutoTurnDeadline();
}

void StopReaderAutoTurn() {
    s_auto_turn_enabled = false;
    s_auto_turn_pending.store(false);
    s_auto_turn_ready = false;
    if (s_auto_turn_timer) {
        lv_timer_del(s_auto_turn_timer);
        s_auto_turn_timer = nullptr;
    }
}

void ReaderAutoTurnPageMoved() {
    if (!s_auto_turn_enabled) return;
    ResetReaderAutoTurnDeadline();
    PowerPolicy::GetInstance().NotifyUserActivity();
}

void ReaderAutoTurnManualInput() {
    if (!s_auto_turn_enabled.load()) return;
    s_auto_turn_pending.store(false);
    ResetReaderAutoTurnDeadline();
}

bool ConsumeReaderAutoTurnRequest() {
    if (!s_auto_turn_pending.exchange(false)) return false;
    if (!s_auto_turn_enabled.load() || !AutoTurnReadingVisible() || ReaderWorkersBusy() ||
        Book_State().reader.session->LiveRelayoutPending() ||
        TouchUiFingerIsDown() || TouchUiHasPendingEdges()) {
        ResetReaderAutoTurnDeadline();
        return false;
    }
    return true;
}
