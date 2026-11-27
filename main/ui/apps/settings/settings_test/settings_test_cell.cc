#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "ui/apps/settings/settings_test/settings_test_cell.h"
#include "business/diagnostics/cell_diagnostic.h"
#include "ui/common/runtime/page_lifecycle.h"
#include "assets/lang_cellular.h"
#include "assets/lang_config.h"
#include "ui/apps/settings/settings_test/settings_test_common.h"

static constexpr const char* kTag = "SettingsTestCell";

static constexpr int kCellCreateMaxRetry = 5;
static constexpr uint32_t kCellCreateRetryMs = 1200;
static int s_pending_cell_slot = -1, s_cell_create_retry = 0;
static CellDiagnosticRequest s_request;
static UiPageLifecycle s_cell_lifecycle;
static void StartCellTest(int slot);
static void StopCellStartTimer();
static bool CellSessionLive() { return SettingsTest_IsLive(); }

static SettingsTestRow* RowForSlot(int slot) {
    return slot == kSettingsTestSimInternal ? &SettingsTest_Ui().sim_int : &SettingsTest_Ui().sim_ext;
}

static void PollCellResult() {
    if (!s_request || !s_request->completed.load(std::memory_order_acquire) || !SettingsTest_IsLive()) {
        return;
    }
    auto result = std::move(s_request);
    auto& ui = SettingsTest_Ui();
    ui.cell_busy = false;
    ui.cell_state = SettingsTestCellState::Idle;
    auto* row = RowForSlot(result->slot);
    SettingsTest_SetRowValue(*row, result->detail);
    SettingsTest_SetRowStatus(*row, result->pass);
    if (ui.cell_auto && result->slot == kSettingsTestSimInternal) {
        StartCellTest(kSettingsTestSimExternal);
    } else {
        ui.cell_auto = false;
    }
}

static void StopCellRetryTimerInternal() {
    if (SettingsTest_Ui().cell_retry_timer != nullptr) {
        lv_timer_delete(SettingsTest_Ui().cell_retry_timer);
        SettingsTest_Ui().cell_retry_timer = nullptr;
    }
    s_pending_cell_slot = -1;
    s_cell_create_retry = 0;
}

static void OnCellRetryTimer(lv_timer_t* /*t*/) {
    SettingsTest_Ui().cell_retry_timer = nullptr;
    if (!CellSessionLive() || s_pending_cell_slot < 0) {
        s_pending_cell_slot = -1;
        s_cell_create_retry = 0;
        return;
    }
    const int slot = s_pending_cell_slot;
    s_pending_cell_slot = -1;
    StartCellTest(slot);
}

static void ScheduleCellRetry(int slot) {
    if (!CellSessionLive()) {
        return;
    }
    if (s_cell_create_retry >= kCellCreateMaxRetry) {
        s_cell_create_retry = 0;
        SettingsTestRow* row = RowForSlot(slot);
        if (row != nullptr) {
            SettingsTest_SetRowValue(*row, LangString_SETTINGS_TEST_CELL_NO_RESOURCE);
            SettingsTest_SetRowStatus(*row, false);
        }
        if (SettingsTest_Ui().cell_auto && slot == kSettingsTestSimInternal) {
            StartCellTest(kSettingsTestSimExternal);
        } else {
            SettingsTest_Ui().cell_auto = false;
        }
        return;
    }

    ++s_cell_create_retry;
    s_pending_cell_slot = slot;
    if (SettingsTest_Ui().cell_retry_timer != nullptr) {
        lv_timer_delete(SettingsTest_Ui().cell_retry_timer);
        SettingsTest_Ui().cell_retry_timer = nullptr;
    }
    SettingsTestRow* row = RowForSlot(slot);
    if (row != nullptr) {
        SettingsTest_SetRowValue(*row, LangString_SETTINGS_TEST_CELL_WAIT_RESOURCE);
    }
    SettingsTest_Ui().cell_retry_timer = lv_timer_create(OnCellRetryTimer, kCellCreateRetryMs, nullptr);
    lv_timer_set_repeat_count(SettingsTest_Ui().cell_retry_timer, 1);
}

static void StartCellTest(int slot) {
    if (SettingsTest_Ui().cell_state != SettingsTestCellState::Idle || SettingsTest_Ui().cell_busy) {
        return;
    }
    SettingsTest_Ui().cell_state = slot == kSettingsTestSimInternal ? SettingsTestCellState::TestingInternal
                                                                    : SettingsTestCellState::TestingExternal;
    SettingsTestRow* row = RowForSlot(slot);
    if (row != nullptr) {
        SettingsTest_SetRowValue(*row, LangString_SETTINGS_TEST_CELL_TESTING);
    }
    s_request = CellDiagnosticStart(slot);
    if (!s_request) {
        SettingsTest_Ui().cell_state = SettingsTestCellState::Idle;
        ScheduleCellRetry(slot);
        return;
    }
    SettingsTest_Ui().cell_busy = true;
    s_cell_create_retry = 0;
    s_pending_cell_slot = -1;
}

static uint32_t CellWaitElapsedMs(uint32_t start_ms) {
    const uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    return now - start_ms;
}

static void OnCellAutoStartPoll(lv_timer_t* t) {
    if (!CellSessionLive()) {
        StopCellStartTimer();
        return;
    }

    const uint32_t start_ms = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lv_timer_get_user_data(t)));
    if (SettingsTest_Ui().wifi_scan_busy && CellWaitElapsedMs(start_ms) < kSettingsTestCellWaitMaxMs) {
        return;
    }

    StopCellStartTimer();
    if (!SettingsTest_IsCellNetwork()) {
        SettingsTest_SetRowValue(SettingsTest_Ui().sim_int, LangString_SETTINGS_TEST_CELL_NEED_4G);
        SettingsTest_SetRowStatus(SettingsTest_Ui().sim_int, false);
        SettingsTest_SetRowValue(SettingsTest_Ui().sim_ext, LangString_SETTINGS_TEST_CELL_NEED_4G);
        SettingsTest_SetRowStatus(SettingsTest_Ui().sim_ext, false);
        return;
    }
    SettingsTest_Ui().cell_auto = true;
    StartCellTest(kSettingsTestSimInternal);
}

static void StopCellStartTimer() {
    if (SettingsTest_Ui().cell_start_timer != nullptr) {
        lv_timer_delete(SettingsTest_Ui().cell_start_timer);
        SettingsTest_Ui().cell_start_timer = nullptr;
    }
}

void SettingsTestCell_StartAutoSequence() {
    s_cell_lifecycle.Bind(SettingsTest_Ui().root_scr, {PollCellResult, [](UiPagePowerEvent event) {
        if (event == UiPagePowerEvent::PauseApplications) {
            SettingsTestCell_StopStartTimer();
            SettingsTestCell_StopRetryTimer();
        } else if (event == UiPagePowerEvent::ResumeApplications) {
            SettingsTestCell_StartAutoSequence();
        }
    }, nullptr, 100});
    SettingsTestCell_StopStartTimer();
    SettingsTestCell_StopRetryTimer();
    const uint32_t start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    SettingsTest_Ui().cell_start_timer = lv_timer_create(OnCellAutoStartPoll, kSettingsTestCellWaitPollMs, nullptr);
    lv_timer_set_repeat_count(SettingsTest_Ui().cell_start_timer, -1);
    lv_timer_set_user_data(SettingsTest_Ui().cell_start_timer,
                           reinterpret_cast<void*>(static_cast<uintptr_t>(start_ms)));
}

void SettingsTestCell_StopStartTimer() {
    CellDiagnosticCancel(s_request);
    s_request.reset();
    SettingsTest_Ui().cell_busy = false;
    SettingsTest_Ui().cell_auto = false;
    SettingsTest_Ui().cell_state = SettingsTestCellState::Idle;
    StopCellStartTimer();
}

void SettingsTestCell_StopRetryTimer() {
    StopCellRetryTimerInternal();
}
