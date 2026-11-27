#include <atomic>
#include <mutex>
#include <new>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "sysmgmt/tasks/task_executor.h"
#include "ui_theme.h"
#include "infra/storage/settings.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "config.h"

static_assert(BOARD_THEME_DARK_DEFAULT == 0 || BOARD_THEME_DARK_DEFAULT == 1);

static std::atomic<int> s_dark{-1};  // 首次读取后供绘制和工作任务共享
static std::mutex s_persist_mutex;   // 保护保存快照与写入任务归属
static int s_pending = -1;           // 最新待保存模式，成功提交后清空
static bool s_writer = false;        // 同时只运行一个内部 RAM 写入任务

static void SaveTheme(void*) {
    unsigned failures = 0;
    for (;;) {
        int value;
        {
            std::lock_guard<std::mutex> lock(s_persist_mutex);
            value = s_pending;
        }
        bool saved = false;
        try {
            Settings settings("display", true);
            const bool written = settings.SetBool("dark", value != 0);
            saved = settings.Commit() && written;
        } catch (const std::bad_alloc&) {
            ESP_LOGE("UiTheme", "operation=save_preferences stage=allocate result=no_memory");
        }
        {
            std::lock_guard<std::mutex> lock(s_persist_mutex);
            if (value != s_pending) {
                failures = 0;
                continue;
            }
            if (saved) {
                s_pending = -1;
            }
            if (saved || ++failures >= 3) {
                s_writer = false;
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250 * failures));
    }
}

bool UiThemeIsDark() {
    int value = s_dark.load();
    if (value < 0) {
        Settings settings("display");
        const int restored = settings.GetBool("dark", BOARD_THEME_DARK_DEFAULT != 0) ? 1 : 0;
        s_dark.compare_exchange_strong(value, restored);
        value = s_dark.load();
    }
    return value != 0;
}

bool UiThemeSetDark(bool dark) {
    std::lock_guard<std::mutex> lock(s_persist_mutex);
    s_pending = dark ? 1 : 0;
    if (!s_writer) {
        s_writer = true;
        if (!TaskExecutor::Schedule([] { SaveTheme(nullptr); }, TaskDomain::Storage)) {
            s_writer = false;
            ESP_LOGE("UiTheme", "operation=save_preferences stage=create_task result=no_memory");
            return false;
        }
    }
    s_dark.store(dark ? 1 : 0);
    return true;
}

void UiThemeApply(lv_display_t* display) {
    lv_theme_t* theme = lv_theme_mono_init(display, UiThemeIsDark(), UiFont());
    lv_display_set_theme(display, theme);
}
