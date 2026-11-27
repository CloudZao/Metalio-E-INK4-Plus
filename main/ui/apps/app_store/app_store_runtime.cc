#include <cstring>
#include <ctime>
#include <memory>
#include <new>
#include <esp_elf.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "ui/apps/app_store/app_runtime_services.h"
#include "ui/apps/app_store/app_store_screen.h"
#include "assets/lang_app_store.h"
#include "assets/lang_config.h"
#include "business/app_store/app_api.h"
#include "business/app_store/app_network.h"
#include "business/app_store/app_executable.h"
#include "ui/common/runtime/page_lifecycle.h"
#include "business/app_store/app_store_icons.h"
#include "ui/common/drawing/adapter/epd_refresh.h"
#include "ui/common/drawing/font/fontpack_lvgl.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/apps/standby/standby_screen.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/drawing/ui_profile.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/runtime/screen_common_nav.h"

struct AppStoreRuntime {
    std::shared_ptr<AppStoreExecutable> executable;
    xz_app_api_t api{}; // 生命周期与页面相同的宿主接口
    AppRuntimeServicesState services;
};

static UiPageLifecycle s_runtime_lifecycle;

struct AppStoreRuntimePage {
    std::shared_ptr<AppStoreRuntime> runtime;  // 页面与加载任务共享运行资源
    lv_obj_t* content = nullptr;               // 应用可绘制区域
    lv_obj_t* loading = nullptr;               // 应用内容就绪前的居中加载层
    lv_obj_t* status = nullptr;                // 加载状态
    bool closing = false;                      // 已离页时禁止执行 ELF 入口
    uint32_t loading_sequence = 0;             // 加载页创建前最后提交的物理帧
};

struct AppStoreRuntimeButtonContext {
    xz_app_click_t callback;          // ELF 内按钮回调
    void* context;                    // 应用自持回调数据
    AppRuntimeServicesState* state;   // 所属服务，用于待机与推白期间屏蔽回调
};

static AppStoreItem s_pending;
static AppStoreRuntimePage* s_active_page = nullptr;  // 当前运行页，供返回键分发定位

static void Style(lv_obj_t* obj, int x, int y, int width, int height) {
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_bg_color(obj, UiColorBackground(), 0);
    lv_obj_set_style_text_color(obj, UiColorForeground(), 0);
    lv_obj_set_style_border_color(obj, UiColorForeground(), 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void* Label(void* parent, int x, int y, int width, int height, const char* text) {
    auto* label = lv_label_create(static_cast<lv_obj_t*>(parent));
    Style(label, x, y, width, height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text ? text : "");
    return label;
}

static void ButtonEvent(lv_event_t* e) {
    auto* data = static_cast<AppStoreRuntimeButtonContext*>(lv_event_get_user_data(e));
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        delete data;
    } else if (lv_event_get_code(e) == LV_EVENT_CLICKED && data->callback && data->state &&
               data->state->open && !data->state->closing && !data->state->transition_pending &&
               !StandbyPresentation::IsActive() && !StandbyPresentation::IsResuming()) {
        data->callback(data->context);
    }
}

static void* Button(void* parent, int x, int y, int width, int height, const char* text, xz_app_click_t click,
                    void* context) {
    auto* data = new (std::nothrow)
        AppStoreRuntimeButtonContext{click, context, s_active_page ? &s_active_page->runtime->services : nullptr};
    if (!data) {
        return nullptr;
    }
    auto* button = lv_button_create(static_cast<lv_obj_t*>(parent));
    lv_obj_remove_style_all(button);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    Style(button, x, y, width, height);
    lv_obj_set_style_border_width(button, 2, 0);
    auto* label = lv_label_create(button);
    lv_obj_set_width(label, width - 12);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text ? text : "");
    lv_obj_center(label);
    lv_obj_add_event_cb(button, ButtonEvent, LV_EVENT_ALL, data);
    HapticAttachClick(button);
    return button;
}

static void SetText(void* widget, const char* text) {
    if (!widget) {
        return;
    }
    auto* object = static_cast<lv_obj_t*>(widget);
    if (lv_obj_check_type(object, &lv_button_class)) {
        object = lv_obj_get_child(object, 0);
    }
    if (object && lv_obj_check_type(object, &lv_label_class)) {
        lv_label_set_text(object, text ? text : "");
    } else if (object && lv_obj_check_type(object, &lv_textarea_class)) {
        lv_textarea_set_text(object, text ? text : "");
    }
}

static void WidgetStyle(void* widget, const xz_app_style_t* style) {
    if (!widget || !style) {
        return;
    }
    auto* object = static_cast<lv_obj_t*>(widget);
    const bool button = lv_obj_check_type(object, &lv_button_class);
    auto* label = button ? lv_obj_get_child(object, 0) : object;
    if (!label) {
        return;
    }
    if (UiTextFontSizeSupported(style->font_size)) {
        // 外部应用协议允许显式字号，此处按请求规格加载，不套用系统页面角色。
        const auto* font = fontpack_lv_font_get(style->font_size, 2);
        if (font) {
            lv_obj_set_style_text_font(label, font, 0);
        }
    }
    const auto align = style->text_align == XZ_APP_ALIGN_RIGHT    ? LV_TEXT_ALIGN_RIGHT
                       : style->text_align == XZ_APP_ALIGN_CENTER ? LV_TEXT_ALIGN_CENTER
                                                                  : LV_TEXT_ALIGN_LEFT;
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_style_border_width(object, style->border_width, 0);
    lv_obj_set_style_radius(object, style->radius, 0);
    lv_obj_set_style_bg_color(object, style->primary ? UiColorForeground() : UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(object, button || style->border_width || style->primary ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(label, style->primary ? UiColorBackground() : UiColorForeground(), 0);
}

static void WidgetEnabled(void* widget, int enabled) {
    if (!widget) {
        return;
    }
    auto* object = static_cast<lv_obj_t*>(widget);
    lv_obj_set_style_opa(object, LV_OPA_50, LV_STATE_DISABLED);
    if (enabled) {
        lv_obj_remove_state(object, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(object, LV_STATE_DISABLED);
    }
}

static int LocalDate(int* year, int* month, int* day) {
    if (!year || !month || !day) {
        return 0;
    }
    const time_t now = std::time(nullptr);
    struct tm date{};
    if (!localtime_r(&now, &date) || date.tm_year < 120) {
        return 0;
    }
    *year = date.tm_year + 1900;
    *month = date.tm_mon + 1;
    *day = date.tm_mday;
    return 1;
}

static uint32_t ReadAsset(void* context, uint32_t offset, void* buffer, uint32_t length) {
    return AppStoreReadAsset(static_cast<AppStoreRuntime*>(context)->executable->item, offset, buffer, length);
}

static lv_obj_t* CreateRuntime();

static void Tick() {
    auto* page = s_active_page;
    if (!page) {
        return;
    }
    auto& runtime = *page->runtime;
    if (page->closing || !runtime.executable->ready.load()) {
        return;
    }
    if (static_cast<int32_t>(EpdGetStats().completed - page->loading_sequence) <= 0) {
        return;
    }
    s_runtime_lifecycle.Unbind();
    if (!runtime.executable->loaded) {
        lv_label_set_text(page->status, runtime.executable->error.c_str());
        return;
    }
    lv_obj_clean(page->content);
    const auto asset_size = runtime.executable->asset_size;
    runtime.api = {XZ_APP_ABI_VERSION,
                   sizeof(xz_app_api_t),
                   page->content,
                   lv_obj_get_width(page->content),
                   lv_obj_get_height(page->content),
                   LangCurrent(),
                   Label,
                   Button,
                   SetText,
                   &runtime,
                   asset_size,
                   ReadAsset,
                   WidgetStyle,
                   WidgetEnabled,
                   LocalDate};
    AppRuntimeServicesBind(runtime.services, runtime.api, page->content, runtime.executable->item);
    char* args[] = {reinterpret_cast<char*>(&runtime.api), nullptr};
    const int result = esp_elf_request(&runtime.executable->elf, 0, 1, args);
    if (result != 0) {
        ESP_LOGE("AppRuntime", "operation=launch stage=entry result=failed code=%d", result);
        AppRuntimeServicesClose(runtime.services);
        lv_obj_clean(page->content);
        // 入口失败后立即释放 ELF，不等页面删除；返回值保留在日志供定位。
        runtime.executable->Close();
        lv_label_set_text(page->status, LangString_STORE_RUN_ERROR);
        return;
    }
    ESP_LOGI("AppRuntime", "operation=launch result=success");
    lv_obj_remove_flag(page->content, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete(page->loading);
    page->loading = nullptr;
    page->status = nullptr;
}

static bool RuntimeOnKey(const char* key) {
    auto* page = s_active_page;
    if (!page || page->closing || std::strcmp(key, "vk_prev") != 0) {
        return false;
    }
    return AppRuntimeServicesHandleBack(page->runtime->services);
}

static void Unload(lv_event_t* event) {
    auto* page = static_cast<AppStoreRuntimePage*>(lv_event_get_user_data(event));
    page->closing = true;
    if (s_active_page == page) {
        s_active_page = nullptr;
    }
    page->runtime->executable->cancelled.store(true);
    AppRuntimeServicesClose(page->runtime->services);
}

static void Delete(lv_event_t* e) {
    auto* page = static_cast<AppStoreRuntimePage*>(lv_event_get_user_data(e));
    if (s_active_page == page) {
        s_active_page = nullptr;
    }
    page->runtime->executable->cancelled.store(true);
    AppRuntimeServicesClose(page->runtime->services);
    lv_obj_clean(page->content);
    delete page;
}

static lv_obj_t* CreateRuntime() {
    auto* page = new AppStoreRuntimePage;
    page->runtime = std::make_shared<AppStoreRuntime>();
    page->runtime->executable = AppStoreOpenExecutable(s_pending);
    if (!page->runtime->executable) {
        delete page;
        ScreenShowToast(LangString_STORE_MEMORY_ERROR);
        return AppStoreScreen::Create();
    }
    ScreenSetIsHome(false);
    auto* root = lv_obj_create(nullptr);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_bg_color(root, UiColorBackground(), 0);
    lv_obj_set_style_text_color(root, UiColorForeground(), 0);
    lv_obj_set_style_text_font(root, UiFont(), 0);
    auto bar = ScreenCreateStatusBar(root);
    page->content = lv_obj_create(root);
    Style(page->content, 0, bar.height, LV_HOR_RES, LV_VER_RES - bar.height);
    lv_obj_set_style_border_width(page->content, 0, 0);
    lv_obj_set_style_pad_all(page->content, 0, 0);
    lv_obj_add_flag(page->content, LV_OBJ_FLAG_HIDDEN);
    page->loading = lv_obj_create(root);
    Style(page->loading, 0, bar.height, LV_HOR_RES, LV_VER_RES - bar.height);
    lv_obj_set_style_border_width(page->loading, 0, 0);
    lv_obj_set_style_pad_all(page->loading, 0, 0);
    auto* group = lv_obj_create(page->loading);
    lv_obj_remove_style_all(group);
    lv_obj_remove_flag(group, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_size(group, LV_HOR_RES - 80, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(group, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(group, 0, 0);
    auto* icon = lv_image_create(group);
    AppStoreIconsSetSource(icon, AppStoreIconsSource(page->runtime->executable->item), 128);
    lv_obj_set_size(icon, 128, 128);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_antialias(icon, false);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    auto* name =
        static_cast<lv_obj_t*>(Label(group, 0, 0, LV_PCT(100), LV_SIZE_CONTENT, AppStoreName(page->runtime->executable->item)));
    lv_obj_set_style_margin_top(name, 16, 0);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    page->status = static_cast<lv_obj_t*>(Label(group, 0, 0, LV_PCT(100), LV_SIZE_CONTENT, LangString_STORE_LOADING));
    lv_obj_set_style_margin_top(page->status, 40, 0);
    lv_obj_set_style_text_align(page->status, LV_TEXT_ALIGN_CENTER, 0);
    const auto* status_font = UiFont(UiFontRole::Caption);
    if (status_font) {
        lv_obj_set_style_text_font(page->status, status_font, 0);
    }
    lv_obj_center(group);
    page->loading_sequence = EpdGetStats().submitted;
    s_runtime_lifecycle.Bind(root, {Tick, [](UiPagePowerEvent event) {
        if (s_active_page && event == UiPagePowerEvent::PauseApplications) {
            AppNetworkStop(s_active_page->runtime->services.network, false);
        }
    }, nullptr, 100});
    lv_obj_add_event_cb(root, Unload, LV_EVENT_SCREEN_UNLOADED, page);
    lv_obj_add_event_cb(root, Delete, LV_EVENT_DELETE, page);
    VkKeyScreenDesc keys;
    keys.factory = CreateRuntime;
    keys.on_key = RuntimeOnKey;
    VkKey_AttachScreen(root, "AppRuntime", keys);
    s_active_page = page;
    return root;
}

void AppStoreScreen::Launch(const AppStoreItem& item) {
    s_pending = item;
    ScreenNavigateTo(CreateRuntime);
}
