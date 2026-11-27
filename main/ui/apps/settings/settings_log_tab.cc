#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <memory>
#include <sys/stat.h>
#include <esp_log.h>
#include "infra/storage/sd_paths.h"
#include "boards/common/storage/sd_access.h"
#include "ui/common/runtime/page_lifecycle.h"
#include "sysmgmt/tasks/task_executor.h"
#include "ui/apps/settings/settings_tabs.h"
#include "assets/lang_config.h"
#include "assets/lang_log.h"
#include "boards/common/board/board.h"
#include "boards/common/storage/sd_log_writer.h"
#include "ui/apps/settings/settings_common.h"
#include "ui/apps/settings/settings_log_upload.h"
#include "ui/common/drawing/display.h"
#include "ui/common/widgets/screen_common.h"

struct SettingsLogUi {
    lv_obj_t* toggle = nullptr;  // 日志采集开关
    lv_obj_t* count = nullptr;  // 本地日志文件数量
    lv_obj_t* updated = nullptr;  // 最近文件修改时间
    lv_obj_t* upload_btn = nullptr;     // 上传按钮
    lv_obj_t* clear_btn = nullptr;
};

struct SettingsLogMetadata {
    unsigned count = 0;  // 非空日志文件数量
    time_t updated = 0;  // 最新日志修改时间
    std::atomic<bool> done{false};
};

static SettingsLogUi s_ui;
static std::atomic<bool> s_clearing{false};  // UI 与后台任务共享的清理状态

static UiPageLifecycle s_page;
static std::shared_ptr<SettingsLogMetadata> s_metadata;
static uint32_t s_last_metadata = 0;

static const char* LogText(const char* zh, const char* en) {
    return std::strcmp(LangCode, "zh-CN") == 0 ? zh : en;
}

static void PollLogMetadata() {
    if (s_metadata && s_metadata->done.load(std::memory_order_acquire)) {
        char count[32];
        std::snprintf(count, sizeof(count), "%u", s_metadata->count);
        if (std::strcmp(lv_label_get_text(s_ui.count), count) != 0) {
            lv_label_set_text(s_ui.count, count);
        }
        char updated[48] = "—";
        if (s_metadata->updated > 0) {
            struct tm local{};
            localtime_r(&s_metadata->updated, &local);
            std::strftime(updated, sizeof(updated), "%Y-%m-%d %H:%M:%S", &local);
        }
        if (std::strcmp(lv_label_get_text(s_ui.updated), updated) != 0) {
            lv_label_set_text(s_ui.updated, updated);
        }
        s_metadata.reset();
    }
    if (s_metadata || (s_last_metadata && lv_tick_elaps(s_last_metadata) < 5000)) {
        return;
    }
    auto job = std::make_shared<SettingsLogMetadata>();
    if (TaskExecutor::Schedule([job]() {
        SdAccessTransaction transaction;
        if (transaction) {
            for (const char* path : {SD_PATH_RUNTIME_LOG, SD_PATH_COREDUMP}) {
                struct stat info{};
                if (stat(path, &info) == 0) {
                    if (S_ISREG(info.st_mode) && info.st_size > 0) {
                        ++job->count;
                        job->updated = std::max(job->updated, info.st_mtime);
                    }
                } else if (errno != ENOENT) {
                    ESP_LOGE("SettingsLog", "operation=metadata result=stat_failed errno=%d", errno);
                }
            }
        }
        job->done.store(true, std::memory_order_release);
    }, TaskDomain::Storage)) {
        s_metadata = std::move(job);
    }
    s_last_metadata = lv_tick_get();
}

static void RefreshOptions() {
    const bool enabled = SdLogWriter_IsEnabled();
    auto refresh_row = [](lv_obj_t* row) {
        if (row) {
            lv_obj_set_state(row, LV_STATE_DISABLED, s_clearing.load());
        }
    };
    if (s_ui.toggle) {
        lv_obj_set_state(s_ui.toggle, LV_STATE_CHECKED, enabled);
        lv_obj_set_state(s_ui.toggle, LV_STATE_DISABLED, s_clearing.load());
    }
    refresh_row(s_ui.upload_btn);
    refresh_row(s_ui.clear_btn);
}

static void OnOptionClicked(lv_event_t* e) {
    if (s_clearing.load() || SettingsLogUpload_IsActive()) {
        return;
    }
    const bool enable = lv_obj_has_state(lv_event_get_current_target_obj(e), LV_STATE_CHECKED);
    if (SdLogWriter_IsEnabled() == enable) {
        return;
    }
    SdLogWriter_SetEnabled(enable);
    RefreshOptions();
}

static void OnUploadClicked(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_clearing.load()) {
        return;
    }
    SettingsLogUpload_Start();
}

static void OnClearClicked(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_clearing.load() || SettingsLogUpload_IsActive()) {
        return;
    }
    s_clearing.store(true);
    ScreenShowToast(LangString_SETTINGS_LOG_CLEARING);
    RefreshOptions();
    // SD 操作使用存储任务的内部 RAM 栈，结果交付前申请显示锁。
    if (!TaskExecutor::Schedule(
            []() {
                const bool ok = SdLogWriter_Clear();
                auto* display = Board::GetInstance().GetDisplay();
                DisplayLockGuard lock(display);
                s_clearing.store(false);
                if (!lock) {
                    return;
                }
                s_last_metadata = 0;
                RefreshOptions();
                // 页面重建后允许新日志页接收仍在执行的清理结果。
                if (s_ui.clear_btn != nullptr) {
                    ScreenShowToast(ok ? LangString_SETTINGS_LOG_CLEAR_OK : LangString_SETTINGS_LOG_CLEAR_FAIL);
                }
            },
            TaskDomain::Storage)) {
        s_clearing.store(false);
        RefreshOptions();
        ScreenShowToast(LangString_SETTINGS_LOG_CLEAR_FAIL);
    }
}

void SettingsLogTab_Build(lv_obj_t* page) {
    s_ui = {};

    s_last_metadata = 0;
    s_page.Bind(page, {PollLogMetadata, nullptr, SettingsLogTab_Reset, 200}, true);
    auto* section = SettingsCreateSection(page, LogText("日志设置", "Log settings"));
    auto* group = SettingsCreateListGroup(section);
    s_ui.toggle = SettingsCreateToggle(group, LogText("采集运行日志", "Collect logs"), OnOptionClicked);
    auto* count = SettingsCreateListRow(group, LogText("本地日志", "Local logs"), "—");
    s_ui.count = lv_obj_get_child(count, 1);
    auto* updated = SettingsCreateListRow(group, LogText("最近更新时间", "Last updated"), "—");
    s_ui.updated = lv_obj_get_child(updated, 1);
    auto* operation = SettingsCreateSection(page, LogText("日志操作", "Log actions"));
    auto* actions = SettingsCreateListGroup(operation);
    s_ui.upload_btn = SettingsCreateListRow(actions, LangString_SETTINGS_LOG_UPLOAD, "", OnUploadClicked);
    s_ui.clear_btn = SettingsCreateListRow(actions, LangString_SETTINGS_LOG_CLEAR, "", OnClearClicked);

    RefreshOptions();
}

void SettingsLogTab_Reset() {
    s_page.Unbind();
    s_metadata.reset();
    SettingsLogUpload_Teardown();
    s_ui = {};
}
