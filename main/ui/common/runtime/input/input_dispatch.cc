#include <new>
#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>
#include "business/diagnostics/aging_test.h"
#include "boards/common/input/input.h"
#include "boards/common/storage/sd_access.h"
#include "infra/storage/key_bindings.h"
#include "sysmgmt/power/standby_presentation.h"
#include "sysmgmt/tasks/task_executor.h"
#include "sysmgmt/update/update_presentation.h"
#include "ui/common/runtime/input/binding_dispatch.h"
#include "ui/common/runtime/input/input_dispatch.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"

static bool s_bindings_loading = false;
static bool s_bindings_checked = false;
static unsigned s_bindings_generation = 0;
static StorageErrorCode s_bindings_storage = StorageErrorCode::NotReady;
static int64_t s_bindings_retry_us = 0;
struct InputBindingsLoadCompletion : ScreenUiCompletion {
    bool ready = false; // 介质暂忙或分配失败时，UI 延后重试
};
static InputBindingsLoadCompletion s_bindings_loaded;

static void PollBindingsStorage() {
    const auto status = SdAccessStatus().code;
    if (s_bindings_loading || (status != StorageErrorCode::None && status != StorageErrorCode::NotReady)) {
        return;
    }
    const unsigned generation = SdAccessMountGeneration();
    if (s_bindings_checked && s_bindings_generation == generation && s_bindings_storage == status) {
        return;
    }
    if (esp_timer_get_time() < s_bindings_retry_us) {
        return;
    }
    s_bindings_loaded.apply = [](ScreenUiCompletion *) {
        s_bindings_loading = false;
        s_bindings_checked = s_bindings_loaded.ready;
        if (!s_bindings_checked) {
            s_bindings_retry_us = esp_timer_get_time() + 5000000;
            return;
        }
        BindingDispatchCancelAll();
        lv_obj_send_event(lv_screen_active(), LV_EVENT_REFRESH, nullptr);
    };
    s_bindings_loading = true;
    if (TaskExecutor::Schedule([] {
            s_bindings_loaded.ready = false;
            try {
                s_bindings_loaded.ready = KeyBindingsPollStorage();
            } catch (const std::bad_alloc &) {
                ESP_LOGE("InputDispatch", "operation=load_bindings result=no_memory");
            }
            ScreenCompleteWork(&s_bindings_loaded);
        }, TaskDomain::Storage)) {
        s_bindings_checked = true;
        s_bindings_generation = generation;
        s_bindings_storage = status;
    } else {
        s_bindings_loading = false;
        s_bindings_retry_us = esp_timer_get_time() + 5000000;
    }
}

bool UiInputNavigationBlocked() {
    return StandbyPresentation::IsActive() || OtaUpgradeScreen::IsActive() || AgingTestIsActive();
}

bool UiInputQuickPanelBlocked() {
    return OtaUpgradeScreen::IsActive();
}

void UiInputPoll() {
    if (!StandbyPresentation::IsActive()) {
        PollBindingsStorage();
    }
    InputEvent event;
    if (InputTakeOverflow()) {
        while (InputTakeEvent(&event)) {
        }
        BindingDispatchCancelAll();
    } else {
        while (InputTakeEvent(&event)) {
            BindingDispatch(static_cast<BindingKey>(event.key), event.action);
        }
    }
    if (!StandbyPresentation::IsActive()) {
        if (InputTakeShutdownRequest()) {
            BindingDispatch(BindingKey::Power, InputLongPress);
        } else if (InputTakePowerClick()) {
            BindingDispatch(BindingKey::Power, InputClick);
        }
    }
}
