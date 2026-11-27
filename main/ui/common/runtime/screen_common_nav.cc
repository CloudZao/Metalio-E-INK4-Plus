#include <cstring>
#include <lvgl.h>
#include <esp_log.h>
#include "ui/common/runtime/lvgl/ui_dispatch.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/runtime/page_lifecycle.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/runtime/screen_common_nav.h"
#include "ui/common/widgets/screen_frontlight.h"
#include "ui/common/drawing/adapter/epd_refresh.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/common/drawing/display_orient.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/widgets/screen_scrollbar.h"

static constexpr const char* TAG = "ScreenNavigation";
static constexpr const char* kHomeScreen = "home";
static constexpr int kMaxBack = 8;

static ScreenNavigationConfig s_navigation{};

void ScreenConfigureNavigation(const ScreenNavigationConfig& config) {
    s_navigation = config;
}

void ScreenOpenNetworkSettings() {
    if (s_navigation.open_network != nullptr) {
        s_navigation.open_network();
    }
}

static bool ScreenCommon_on_home = true;
static ScreenFactory ScreenCommon_back_stack[kMaxBack] = {};
static int ScreenCommon_back_depth = 0;

enum class ScreenCommonNavigationKind {
    None,  // 无待处理导航
    To,    // 打开指定页面
    Home,  // 返回首页
    Back,  // 返回上一页面
};

struct ScreenCommonPendingNavigation {
    ScreenCommonNavigationKind kind = ScreenCommonNavigationKind::None;  // 尚未执行的最新导航意图
    ScreenFactory factory = nullptr;                                     // To 目标的建屏工厂
    ScreenFactory return_to = nullptr;                                   // 指定返回入口，空时使用来源页面工厂
    lv_obj_t* origin = nullptr;                                          // 发起导航的活动屏幕
    bool standby = false;                                                // 待机 Overlay 不改变活动屏幕，需单独校验
};

static lv_timer_t* navigation_retry = nullptr;
static ScreenCommonPendingNavigation pending_navigation;

static void ClearPendingNavigation() {
    pending_navigation = {};
    if (navigation_retry != nullptr) {
        lv_timer_pause(navigation_retry);
    }
}

static void GoHomeNow() {
    if (DisplayUiSetOrient(kDisplayUiPortrait)) {
        if (auto* disp = LVAdapterDisplay::Instance()) {
            disp->RequestFullOnNextCommit();
        }
    }
    ScreenCommon_ClearBackStack();
    ScreenCommon_on_home = true;
    if (s_navigation.create_home != nullptr) {
        ScreenLoadReplace(s_navigation.create_home());
    }
}

static void ExecuteNavigation(const ScreenCommonPendingNavigation& target) {
    if (s_navigation.prepare != nullptr) {
        s_navigation.prepare();
    }
    if (target.kind == ScreenCommonNavigationKind::To) {
        const char* cur = VkKey_ActiveScreen();
        if (cur != nullptr && std::strcmp(cur, kHomeScreen) != 0 && std::strcmp(cur, "none") != 0) {
            if (ScreenFactory factory = target.return_to ? target.return_to : VkKey_GetScreenFactory(cur)) {
                ScreenCommon_PushBackFactory(factory);
                ESP_LOGI(TAG, "navigate to: push back factory for %s (depth=%d)", cur, ScreenCommon_back_depth);
            }
        }
        ScreenCommon_on_home = false;
        ScreenLoadReplace(target.factory());
    } else if (target.kind == ScreenCommonNavigationKind::Back) {
        ScreenFactory prev = ScreenCommon_PopBackFactory();
        if (prev != nullptr) {
            ESP_LOGI(TAG, "navigate back -> recreate (stack left=%d)", ScreenCommon_back_depth);
            ScreenCommon_on_home = false;
            ScreenLoadReplace(prev());
        } else {
            ESP_LOGI(TAG, "navigate back -> home (empty stack)");
            GoHomeNow();
        }
    } else if (target.kind == ScreenCommonNavigationKind::Home) {
        ESP_LOGI(TAG, "-> home (clear back stack depth was %d)", ScreenCommon_back_depth);
        GoHomeNow();
    }
}

static void ProcessPendingNavigation(lv_timer_t*) {
    if (pending_navigation.kind == ScreenCommonNavigationKind::None ||
        pending_navigation.origin != lv_screen_active() ||
        pending_navigation.standby != (s_navigation.is_standby != nullptr && s_navigation.is_standby())) {
        ClearPendingNavigation();
        return;
    }
    if (!EpdTryBeginUiRender()) {
        return;
    }

    struct UiGuard {
        ~UiGuard() {
            EpdEndUiRender();
        }
    } guard;

    const ScreenCommonPendingNavigation target = pending_navigation;
    ClearPendingNavigation();
    ExecuteNavigation(target);
}

static void RequestNavigation(ScreenCommonNavigationKind kind, ScreenFactory factory = nullptr, bool next_tick = false,
                              ScreenFactory return_to = nullptr) {
    // 先取消旧翻页，再等待落墨结束；合并期间不创建页面或修改返回栈。
    EpdCancelPageTurn();
    pending_navigation = {kind, factory, return_to, lv_screen_active(),
                          s_navigation.is_standby != nullptr && s_navigation.is_standby()};
    if (!next_tick) {
        ProcessPendingNavigation(nullptr);
        if (pending_navigation.kind == ScreenCommonNavigationKind::None) {
            return;
        }
    }
    if (navigation_retry == nullptr) {
        navigation_retry = lv_timer_create(ProcessPendingNavigation, 20, nullptr);
        if (navigation_retry == nullptr) {
            ClearPendingNavigation();
            ESP_LOGW(TAG, "cannot defer navigation while refreshing");
            return;
        }
    }
    lv_timer_resume(navigation_retry);
}

void ScreenCommon_CancelPendingNavigation() {
    ClearPendingNavigation();
}

void ScreenCommon_ClearBackStack() {
    ScreenCommon_back_depth = 0;
    for (int i = 0; i < kMaxBack; ++i) {
        ScreenCommon_back_stack[i] = nullptr;
    }
}

void ScreenCommon_PushBackFactory(ScreenFactory factory) {
    if (factory == nullptr) {
        return;
    }
    if (ScreenCommon_back_depth < kMaxBack) {
        ScreenCommon_back_stack[ScreenCommon_back_depth++] = factory;
    } else {
        for (int i = 1; i < kMaxBack; ++i) {
            ScreenCommon_back_stack[i - 1] = ScreenCommon_back_stack[i];
        }
        ScreenCommon_back_stack[kMaxBack - 1] = factory;
        ESP_LOGW(TAG, "back stack full, drop oldest");
    }
}

ScreenFactory ScreenCommon_PopBackFactory() {
    if (ScreenCommon_back_depth <= 0) {
        return nullptr;
    }
    --ScreenCommon_back_depth;
    ScreenFactory f = ScreenCommon_back_stack[ScreenCommon_back_depth];
    ScreenCommon_back_stack[ScreenCommon_back_depth] = nullptr;
    return f;
}

void ScreenCommon_GoHomeAsync(void* /*user_data*/) {
    if (ScreenFrontlightIsVisible()) {
        ScreenFrontlightHide();
        return;
    }
    if (ScreenCommon_on_home) {
        EpdCancelPageTurn();
        ClearPendingNavigation();
        ESP_LOGI(TAG, "home ignored (already home)");
        return;
    }
    ScreenGoHome();
}

void ScreenCommon_NavigateBackAsync(void* /*user_data*/) {
    RequestNavigation(ScreenCommonNavigationKind::Back);
}


void ScreenNavigateTo(lv_obj_t* (*create)(), lv_obj_t* (*return_to)()) {
    if (create != nullptr) {
        RequestNavigation(ScreenCommonNavigationKind::To, create, false, return_to);
    }
}

void ScreenNavigateBack() {
    if (ScreenFrontlightIsVisible()) {
        ScreenFrontlightHide();
        return;
    }
    auto* disp = LVAdapterDisplay::Instance();
    if (disp == nullptr || !disp->IsReady()) {
        ESP_LOGW(TAG, "ScreenNavigateBack: adapter not ready");
        return;
    }
    const char* active = VkKey_ActiveScreen();
    if (active == nullptr || std::strcmp(active, kHomeScreen) == 0 || std::strcmp(active, "none") == 0) {
        EpdCancelPageTurn();
        ClearPendingNavigation();
        ESP_LOGI(TAG, "ScreenNavigateBack: skip stale back from screen=%s", active ? active : "null");
        return;
    }
    // 在事件回调中只记录来源和意图，下一轮定时器再执行建屏。
    RequestNavigation(ScreenCommonNavigationKind::Back, nullptr, true);
}

void ScreenGoHome() {
    RequestNavigation(ScreenCommonNavigationKind::Home);
}

void ScreenSetIsHome(bool is_home) {
    ScreenCommon_on_home = is_home;
}

bool ScreenIsHome() {
    return ScreenCommon_on_home;
}

void ScreenLoadReplace(lv_obj_t* new_scr) {
    ScreenFrontlightHide();
    lv_obj_t* old_scr = lv_screen_active();
    EpdCancelPageTurn();
    EpdRequestScreenChange();
    lv_screen_load(new_scr);
    if (old_scr != nullptr && old_scr != new_scr) {
        lv_obj_delete_delayed(old_scr, 1);
    }
    // 清掉跨页残留的 press/gesture，避免新页首击被当成旧拖拽吞掉
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev != nullptr; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_reset(indev, nullptr);
        }
    }
}

void ScreenPollPendingWork() {
    UiFontPoll();
    if (s_navigation.poll_navigation) {
        s_navigation.poll_navigation();
    }
    ScreenLvAsyncDrain();
    UiPageLifecycle::Poll();
    HapticRefreshZones();
    ScreenScrollbarPoll();
}
