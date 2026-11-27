#include <lvgl.h>
#include <esp_log.h>
#include "sdkconfig.h"
#include "assets/lang_config.h"
#include "assets/lang_app_store.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/apps/app_store/app_store_screen.h"
#include "ui/apps/assistant/assistant_screen.h"
#include "ui/apps/cloud/cloud_screen.h"
#include "ui/apps/settings/settings_screen.h"
#include "ui/apps/workbuddy/workbuddy_screen.h"
#include "ui/application_ui.h"
#include "ui/common/runtime/display_ui.h"
#include "ui/common/widgets/touch_missing_screen.h"
#include "business/diagnostics/aging_test.h"
#include "business/conversation/conversation_presentation.h"
#include "ui/apps/settings/settings_test/settings_test_screens.h"
#include "boards/common/board/board.h"
#include "ui/common/runtime/screen_common_nav.h"
#include "ui/common/runtime/page_lifecycle.h"
#include "ui/apps/book/book_nav.h"
#include "ui/apps/book/book_screen.h"
#include "ui/apps/settings/network/network_screen.h"
#include "ui/apps/standby/standby_screen.h"
#include "ui/apps/checklist/checklist_screen.h"
#include "ui/apps/wallpaper/wallpaper_screen.h"
#include "ui/home/home_screen.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/runtime/input/input_dispatch.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"
#include "sysmgmt/power/standby_presentation.h"

static constexpr const char* TAG = "ApplicationUi";

struct UiApplicationEntry {
    UiAppId id; // 内置应用标识
    const char* icon_path; // 首页展示图标
    const char* const* name; // 语言资源槽，动态名称由查询入口处理
    lv_obj_t* (*create)(); // 创建应用首屏
};

#if !CONFIG_PRODUCT_GLOBAL
static const char* kWorkBuddyName = "workbuddy";
#endif
static const UiApplicationEntry kApplications[] = {
    {UiAppId::Checklist, "A:ic_app_home_task.spng", &LangString_HOME_APP_TASK, ChecklistScreen::Create},
    {UiAppId::Assistant, "A:ic_app_home_teacher.spng", nullptr, AssistantScreen::Create},
    {UiAppId::Book, "A:ic_app_home_book.spng", &LangString_HOME_APP_BOOK, BookScreen::Create},
    {UiAppId::Wallpaper, "A:ic_app_home_wallpaper.spng", &LangString_HOME_APP_WALLPAPER, WallpaperScreen::Create},
    {UiAppId::Cloud, "A:ic_app_home_cloud.spng", &LangString_HOME_APP_CLOUD, CloudScreen::Create},
    {UiAppId::Settings, "A:ic_app_home_setting.spng", &LangString_HOME_APP_SETTINGS, SettingsScreen::Create},
    #if !CONFIG_PRODUCT_GLOBAL
    {UiAppId::WorkBuddy, "A:ic_app_home_workbuddy.spng", &kWorkBuddyName, WorkBuddyScreen::Create},
    #endif
    {UiAppId::Store, "A:ic_app_home_cloud.spng", &LangString_STORE_TITLE, AppStoreScreen::Create},
};
static lv_obj_t* s_pending_origin = nullptr; // 请求来源，仅用于比较，不解引用
static UiAppId s_pending_app = UiAppId::None; // 等待待机硬件及唤醒首帧完成的应用请求

static const UiApplicationEntry* FindApplication(UiAppId id) {
    for (const auto& entry : kApplications) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

static void OpenNetworkSettings() {
    if (ScreenIsHome() && Board::GetInstance().GetNetworkStatus().kind == NetworkKind::Wifi &&
        !Board::GetInstance().IsNetworkReady()) {
        ScreenNavigateTo(NetworkScreen::Create);
    }
}

UiAppInfo UiGetAppInfo(UiAppId id) {
    const auto* entry = FindApplication(id);
    if (entry == nullptr) {
        return {};
    }
    const char* name = id == UiAppId::Assistant ? AssistantScreen::DisplayName(true) : *entry->name;
    return {entry->icon_path, name};
}

bool UiOpenApp(UiAppId id) {
    const auto* entry = FindApplication(id);
    if (entry == nullptr) {
        ESP_LOGW(TAG, "operation=open_app result=unknown id=%u", static_cast<unsigned>(id));
        return false;
    }
    if (StandbyPresentation::IsActive() || StandbyPresentation::IsResuming()) {
        s_pending_app = id;
        s_pending_origin = lv_screen_active();
        if (StandbyPresentation::IsActive()) {
            StandbyPresentation::RequestWake();
        }
        return true;
    }
    s_pending_app = UiAppId::None;
    if (UiInputNavigationBlocked()) {
        return false;
    }
    ScreenNavigateTo(entry->create);
    return true;
}

static void PollApplicationNavigation() {
    if (s_pending_app != UiAppId::None) {
        if (s_pending_origin != lv_screen_active()) {
            s_pending_app = UiAppId::None;
        } else if (!StandbyPresentation::IsActive() && !StandbyPresentation::IsResuming()) {
            UiOpenApp(s_pending_app);
        }
    }
}

static lv_obj_t* CreateStartupPage(bool touch_ready, bool defer_restore, DisplayUiScreenFactory& pending) {
    pending = nullptr;
    if (!touch_ready) {
        return TouchMissingScreen::Create();
    }
    if (AgingTestLoadBootState()) {
        if (defer_restore) {
            pending = SettingsTestAgingScreen::CreateBootResume;
            return lv_obj_create(nullptr);
        }
        return SettingsTestAgingScreen::CreateBootResume();
    }
    if (defer_restore && BookScreen::LoadBootResume()) {
        pending = BookScreen::CreateBootResume;
        return lv_obj_create(nullptr);
    }
    return HomeScreen::Create();
}

static bool IsPageWaveVisible() {
    return ConversationPresentation::IsPttWaveVisible() || WorkBuddyScreen::IsPttWaveVisible();
}

void UiInitializeApplications() {
    DisplayUiConfigure({CreateStartupPage, BookScreen::PreparePowerOff, IsPageWaveVisible});
    ScreenConfigureNavigation({HomeScreen::Create, ReleaseReaderForSystemHome,
                               StandbyPresentation::IsActive, OpenNetworkSettings, PollApplicationNavigation});
}


void UiLoadApplicationPreferences() {
    HomeScreen::LoadCardStyle();
}

void UiRollbackPowerOff() {
    if (StandbyPresentation::IsActive()) {
        StandbyPresentation::FinishWake();
    }
    if (auto* display = LVAdapterDisplay::Instance()) {
        display->CancelPowerOff();
    }
    ScreenLoadReplace(HomeScreen::Create());
}

void UiPrepareStandbyOverlay() {
    UiPageLifecycle::NotifyPower(UiPagePowerEvent::PrepareOverlay);
    if (auto* display = LVAdapterDisplay::Instance()) {
        display->PrepareWallpaperOverlay();
    }
}

void UiPauseStandbyApplications() {
    UiPageLifecycle::NotifyPower(UiPagePowerEvent::PauseApplications);
}

void UiPauseStandbyViews() {
    UiPageLifecycle::NotifyPower(UiPagePowerEvent::PauseViews);
    if (auto* display = LVAdapterDisplay::Instance()) {
        display->PauseStandbyBackgroundWork();
    }
}

void UiResumeStandbyCalibration() {
    UiPageLifecycle::NotifyPower(UiPagePowerEvent::ResumeCalibration);
}

void UiResumeStandbyApplications() {
    UiPageLifecycle::NotifyPower(UiPagePowerEvent::ResumeApplications);
}

void UiResumeStandbyViews() {
    UiPageLifecycle::NotifyPower(UiPagePowerEvent::ResumeViews);
    if (auto* display = LVAdapterDisplay::Instance()) {
        display->ResumeStandbyBackgroundWork();
    }
}

void UiPauseWallpaperDecode(bool paused) {
    UiPageLifecycle::NotifyPower(paused ? UiPagePowerEvent::PauseImages : UiPagePowerEvent::ResumeImages);
}

void UiFinishStandbyResume() {
    UiPageLifecycle::NotifyPower(UiPagePowerEvent::FinishResume);
}

void UiStopTransferForStandby() {
    UiPageLifecycle::NotifyPower(UiPagePowerEvent::StopTransfers);
}
