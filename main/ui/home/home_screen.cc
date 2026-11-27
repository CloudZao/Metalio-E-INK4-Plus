#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "sdkconfig.h"
#include "sysmgmt/tasks/task_executor.h"
#include "business/conversation/conversation_presentation.h"
#include "ui/home/pixel/home_pixel.h"
#include "assets/lang_config.h"
#include "boards/common/storage/SdCardManager.hpp"
#include "business/app_store/app_store.h"
#include "business/app_store/app_store_icons.h"
#include "ui/home/home_apps_slash.h"
#include "ui/home/home_screen_priv.h"
#include "ui/home/home_screen_ui.h"
#include "infra/storage/settings.h"
#include "ui/common/drawing/adapter/epd_refresh.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/widgets/screen_text.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/drawing/rendering/home_icon_source.h"
#include "ui/apps/app_store/app_store_screen.h"
#include "ui/home/home_hero.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/runtime/input/vk_page_repeat.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/runtime/screen_common_nav.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"

uint8_t Home_gray_dither_l8[kDitherW * kDitherH];
lv_image_dsc_t Home_gray_dither_img;
bool Home_gray_dither_ready = false;

void Home_OnAppNameDraw(lv_event_t* e) {
    lv_obj_t* name = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    lv_obj_init_draw_label_dsc(name, LV_PART_MAIN, &dsc);
    dsc.text = lv_label_get_text(name);
    dsc.opa = LV_OPA_50;
    dsc.ofs_x = 1;
    lv_area_t area;
    lv_obj_get_content_coords(name, &area);
    lv_draw_label(lv_event_get_layer(e), &dsc, &area);
}

static constexpr UiAppId kBuiltinApps[] = {
    UiAppId::Checklist, UiAppId::Assistant, UiAppId::Book, UiAppId::Wallpaper, UiAppId::Cloud, UiAppId::Settings,
    #if !CONFIG_PRODUCT_GLOBAL
    UiAppId::WorkBuddy,
    #endif
};

static constexpr UiAppId kBuiltinSlashApps[] = {
    UiAppId::Book, UiAppId::Wallpaper, UiAppId::Assistant, UiAppId::Checklist, UiAppId::Cloud, UiAppId::Settings,
    #if !CONFIG_PRODUCT_GLOBAL
    UiAppId::WorkBuddy,
    #endif
};

static AppEntry BuiltinApp(UiAppId id) {
    const auto info = UiGetAppInfo(id);
    AppEntry entry{};
    entry.icon_path = info.icon_path;
    entry.builtin_id = id;
    return entry;
}

// 每页六个应用，新增应用自动进入后续页。
std::vector<AppEntry> kApps;
std::vector<AppEntry> kSlashApps;
int kTotalApps = 0;
int kSlashAppCount = 0;
static std::vector<AppStoreItem> s_home_installed;

const char* Home_AppLabel(const AppEntry& entry) {
    if (entry.builtin_id != UiAppId::None) {
        return UiGetAppInfo(entry.builtin_id).name;
    }
    return std::strcmp(LangCurrent(), "zh-CN") == 0 ? entry.installed_name.c_str() : entry.installed_name_en.c_str();
}

static const void* Home_AppIcon(const AppEntry& entry, const char* fallback) {
    if (entry.builtin_id == UiAppId::Store) {
        return AppStoreIconsSource("app-store");
    }
    if (!entry.installed_id.empty()) {
        for (const auto& item : s_home_installed) {
            if (item.app_id == entry.installed_id) {
                return AppStoreIconsSource(item);
            }
        }
        return AppStoreIconsSource(nullptr);
    }
    return HomeIconSource(fallback ? fallback : entry.icon_path);
}

void Home_SetAppIcon(lv_obj_t* image, const AppEntry& entry, int edge, const char* fallback) {
    if (!entry.installed_id.empty() || entry.builtin_id == UiAppId::Store ||
        !HomeIconSetFullSource(image, fallback ? fallback : entry.icon_path)) {
        AppStoreIconsSetSource(image, Home_AppIcon(entry, fallback), edge);
    }
    lv_obj_set_size(image, edge, edge);
    lv_image_header_t header{};
    const void* source = lv_image_get_src(image);
    const lv_result_t info_result = source ? lv_image_decoder_get_info(source, &header) : LV_RESULT_INVALID;
    const bool valid_size = info_result == LV_RESULT_OK && header.w > 0 && header.h > 0;
    if (!valid_size) {
        ESP_LOGW(TAG, "operation=app_icon stage=read_header result=fit_fallback code=%d width=%u height=%u",
                 static_cast<int>(info_result), static_cast<unsigned>(header.w), static_cast<unsigned>(header.h));
    }
    const bool native_size = valid_size && header.w <= edge && header.h <= edge;
    // 空间足够时保留原像素，避免轻微放大后二值化造成边缘凸点。
    lv_image_set_inner_align(image, native_size ? LV_IMAGE_ALIGN_CENTER : LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_antialias(image, !native_size);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE);
}

static void Home_RefreshInstalledApps(std::vector<AppStoreItem> items) {
    kApps.clear();
    kSlashApps.clear();
    for (const auto id : kBuiltinApps) {
        kApps.push_back(BuiltinApp(id));
    }
    for (const auto id : kBuiltinSlashApps) {
        kSlashApps.push_back(BuiltinApp(id));
    }
    #if !CONFIG_BOARD_TYPE_METALIO_E_INK_4
    kApps.push_back(BuiltinApp(UiAppId::Store));
    kSlashApps.push_back(BuiltinApp(UiAppId::Store));
    #endif
    s_home_installed = std::move(items);
    for (const auto& item : s_home_installed) {
        const AppEntry entry = {"A:ic_app_home_task.spng", nullptr, item.app_id, item.name, item.name_en};
        kApps.push_back(entry);
        kSlashApps.push_back(entry);
    }
    kTotalApps = static_cast<int>(kApps.size());
    kSlashAppCount = static_cast<int>(kSlashApps.size());
}

struct HomeScreenHomeStorageWait {
    lv_timer_t* timer;   // 首次挂载等待定时器
    unsigned ticks = 0;  // 无卡时的有限等待次数
};

static void Home_ApplyStorageApps(std::vector<AppStoreItem> items);

static void Home_DeleteStorageWait(lv_event_t* event) {
    auto* wait = static_cast<HomeScreenHomeStorageWait*>(lv_event_get_user_data(event));
    if (wait->timer) {
        lv_timer_delete(wait->timer);
    }
    delete wait;
}

static void Home_StorageReady(lv_timer_t* timer) {
    auto* wait = static_cast<HomeScreenHomeStorageWait*>(lv_timer_get_user_data(timer));
    std::vector<AppStoreItem> items;
    std::string error;
    const bool ready = SdCardManager::GetInstance().IsMounted() && ScreenIsHome() && AppStoreScan(items, error);
    if (!ready && ++wait->ticks < 120) {
        return;
    }
    if (ready && !items.empty()) {
        if (!EpdTryBeginUiRender()) {
            return;
        }
        struct HomeScreenUiRenderGuard {
            ~HomeScreenUiRenderGuard() { EpdEndUiRender(); }
        } guard;
        Home_ApplyStorageApps(std::move(items));
    }
    wait->timer = nullptr;
    lv_timer_delete(timer);
}

static void Home_WaitForStorage(lv_obj_t* screen) {
    if (SdCardManager::GetInstance().IsMounted()) {
        return;
    }
    auto* wait = new HomeScreenHomeStorageWait{};
    wait->timer = lv_timer_create(Home_StorageReady, 500, wait);
    lv_obj_add_event_cb(screen, Home_DeleteStorageWait, LV_EVENT_DELETE, wait);
}

lv_obj_t* Home_apps = nullptr;
lv_obj_t* Home_page_lbl = nullptr;
lv_obj_t* Home_scr = nullptr;
int Home_page = 0;  // 普通返回保留页码，Home 操作返回第一页
bool Home_slash_layout = false;
lv_coord_t Home_slash_apps_h = 0;

int Home_PageCount() {
    return (kTotalApps + kAppsPerPage - 1) / kAppsPerPage;
}

void Home_FillAppsPage() {
    if (Home_card_style == HomeScreen::kCardStylePixel) {
        HomePixel_FillAppsPage();
        Home_UpdatePageIndicator();
        return;
    }
    if (Home_apps == nullptr) {
        return;
    }
    if (Home_slash_layout) {
        Home_FillSlashAppsPage();
        Home_UpdatePageIndicator();
        return;
    }
    const int pages = Home_PageCount();
    if (pages < 1) {
        return;
    }
    if (Home_page < 0) {
        Home_page = 0;
    }
    if (Home_page >= pages) {
        Home_page = pages - 1;
    }

    lv_obj_clean(Home_apps);
    const int card_style = HomeScreen::LoadCardStyle();
    const int start = Home_page * kAppsPerPage;
    int end = start + kAppsPerPage;
    if (end > kTotalApps) {
        end = kTotalApps;
    }
    for (int i = start; i < end; ++i) {
        Home_CreateAppCell(Home_apps, kApps[i], card_style);
    }

    Home_UpdatePageIndicator();
    ESP_LOGI(TAG, "home page %d/%d apps %d..%d of %d", Home_page + 1, pages, start, end - 1, kTotalApps);
}

static void Home_PlaceClassicApps(lv_obj_t* hero) {
    const int pages = Home_PageCount();
    const int indicator_h = pages > 1 ? HOME_PAGE_INDICATOR_H : 0;
    const lv_coord_t desired_apps_h =
        HOME_APPS_PAD_TOP + HOME_APPS_VISIBLE_ROWS * HOME_APP_CARD_H +
        (HOME_APPS_VISIBLE_ROWS > 1 ? (HOME_APPS_VISIBLE_ROWS - 1) * HOME_APP_CARD_GAP_ROW : 0) + HOME_APPS_PAD_BOT;
    lv_obj_update_layout(hero);
    const int hero_bottom = lv_obj_get_y(hero) + lv_obj_get_height(hero);
    const int available = std::max<int>(1, LV_VER_RES - indicator_h - hero_bottom);
    const lv_coord_t apps_content_h = std::min<int>(desired_apps_h, available);
    const lv_coord_t apps_bottom_inset = indicator_h + std::max<int>(0, available - apps_content_h) / 2;
    lv_obj_set_size(Home_apps, LV_HOR_RES, apps_content_h);
    lv_obj_align(Home_apps, LV_ALIGN_BOTTOM_MID, 0, -apps_bottom_inset);
}

void Home_UpdatePageIndicator() {
    if (!Home_page_lbl) {
        return;
    }
    lv_obj_clean(Home_page_lbl);
    const int pages = Home_PageCount();
    for (int i = 0; i < pages; ++i) {
        auto* target = lv_obj_create(Home_page_lbl);
        lv_obj_remove_style_all(target);
        lv_obj_set_size(target, UiSx(20), UiSy(30));
        lv_obj_clear_flag(target, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(target, LV_OBJ_FLAG_SCROLLABLE);
        auto* dot = lv_obj_create(target);
        lv_obj_remove_style_all(dot);
        const int size = UiSx(i == Home_page ? 11 : 6);
        lv_obj_set_size(dot, size, size);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, UiColorForeground(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(dot);
    }
}

static void Home_CreatePageIndicator(lv_obj_t* screen) {
    if (Home_PageCount() <= 1) {
        Home_page_lbl = nullptr;
        return;
    }
    Home_page_lbl = lv_obj_create(screen);
    lv_obj_remove_style_all(Home_page_lbl);
    lv_obj_set_size(Home_page_lbl, Home_PageCount() * UiSx(20), UiSy(30));
    lv_obj_set_flex_flow(Home_page_lbl, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(Home_page_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(Home_page_lbl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(Home_page_lbl, LV_ALIGN_BOTTOM_MID, 0, -UiSy(8));
    Home_UpdatePageIndicator();
}

static void Home_ApplyStorageApps(std::vector<AppStoreItem> items) {
    const int old_pages = Home_PageCount();
    Home_RefreshInstalledApps(std::move(items));
    const int pages = Home_PageCount();
    if (Home_page < 0 || Home_page >= pages) {
        Home_page = 0;
    }
    if (old_pages != pages && Home_scr != nullptr) {
        if (Home_page_lbl) {
            lv_obj_delete(Home_page_lbl);
        }
        Home_page_lbl = nullptr;
        Home_CreatePageIndicator(Home_scr);
        if (Home_apps != nullptr && !Home_slash_layout && Home_card_style != HomeScreen::kCardStylePixel) {
            lv_obj_t* hero = nullptr;
            lv_obj_t* parent = lv_obj_get_parent(Home_apps);
            const uint32_t count = lv_obj_get_child_count(parent);
            for (uint32_t i = 0; i < count; ++i) {
                lv_obj_t* child = lv_obj_get_child(parent, i);
                if (child == Home_apps) {
                    break;
                }
                hero = child;
            }
            if (hero != nullptr) {
                Home_PlaceClassicApps(hero);
            }
        }
    }
    // 只更新当前首页的应用区，不重新进入首页，也不请求切页推白。
    Home_FillAppsPage();
    ESP_LOGI(TAG, "installed apps loaded in place count=%u", static_cast<unsigned>(s_home_installed.size()));
}

ScreenPaintCoalesce s_home_paint{};

void Home_RequestFillAppsPage() {
    if (s_home_paint.paint == nullptr) {
        s_home_paint.paint = []() {
            if (!ScreenIsHome() || Home_scr == nullptr) {
                return;
            }
            EpdRequestScreenChange();
            Home_FillAppsPage();
        };
    }
    ScreenPaintCoalesceRequest(&s_home_paint);
}

bool HomeScreen::GoToFirstPage() {
    const auto go = [](void*) {
        const bool changed = Home_page != 0;
        Home_page = 0;
        if (ScreenIsHome()) {
            if (changed) {
                Home_RequestFillAppsPage();
            }
        } else {
            ScreenRequestHome();
        }
    };
    if (ScreenLvAsyncUrgent(go) || ScreenLvAsync(go)) {
        return true;
    }
    ESP_LOGW(TAG, "first home page navigation could not be queued");
    return false;
}

bool HomePageRepeatStep(int page_delta) {
    const int pages = Home_PageCount();
    if (pages <= 1 || page_delta == 0) {
        return false;
    }
    // 长按调度传入阅读器的十页步长，首页只取方向逐页循环。
    const int next = (Home_page + (page_delta > 0 ? 1 : pages - 1)) % pages;
    Home_page = next;
    Home_RequestFillAppsPage();
    return true;
}

bool Home_OnVkKey(const char* key) {
    if (key == nullptr) {
        return false;
    }
    if (std::strcmp(key, "vk_home") == 0) {
        HomeScreen::GoToFirstPage();
        return true;
    }
    if (std::strcmp(key, "vk_prev") == 0) {
        HomePageRepeatStep(-1);
        return true;
    }
    if (std::strcmp(key, "vk_next") == 0) {
        HomePageRepeatStep(1);
        return true;
    }
    return false;
}

static bool Home_OnPageKey(const char* key, bool long_press) {
    if (long_press) {
        VkPageRepeatTryStart(key, HomePageRepeatStep);
    } else {
        Home_OnVkKey(key);
    }
    return true;
}

bool Home_OnVkKeyLongPress(const char* key) {
    if (key && std::strcmp(key, "vk_home") == 0) {
        return Home_OnVkKey(key);
    }
    return VkPageRepeatTryStart(key, HomePageRepeatStep);
}

bool Home_OnVkKeyPressUp(const char* key) {
    return VkPageRepeatOnPressUp(key);
}

// BOOT 长按快捷进百问（仍按住则 hold-through 聆听）；短按进待机改走电源键
bool Home_OnBootLongPress() {
    ESP_LOGI(TAG, "boot long -> assistant (hold-through PTT if still pressed)");
    ConversationPresentation::RequestOpen();
    return true;
}

void Home_OnHomeDeleted(lv_event_t* e) {
    // ScreenLoadReplace 会 async 删旧屏；勿清掉新首页的指针
    if (lv_event_get_target(e) != Home_scr) {
        return;
    }
    HomeHero_Teardown();
    HomePixel_Stop();
    Home_scr = nullptr;
    Home_apps = nullptr;
    Home_page_lbl = nullptr;
    Home_slash_layout = false;
    Home_slash_apps_h = 0;
}

void Home_LaunchAppAsync(void* user_data) {
    std::unique_ptr<AppEntry> entry(static_cast<AppEntry*>(user_data));
    if (entry == nullptr || (entry->create == nullptr && entry->installed_id.empty() && entry->builtin_id == UiAppId::None)) {
        return;
    }
    ESP_LOGI(TAG, "launch %s", Home_AppLabel(*entry));
    if (!entry->installed_id.empty()) {
        for (const auto& item : s_home_installed) {
            if (item.app_id == entry->installed_id) {
                AppStoreScreen::Launch(item);
                break;
            }
        }
    } else if (entry->create != nullptr) {
        ScreenNavigateTo(entry->create);
    } else {
        UiOpenApp(entry->builtin_id);
    }
}

void Home_OnAppClicked(lv_event_t* e) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const AppEntry* entry = nullptr;
    while (obj != nullptr) {
        entry = static_cast<const AppEntry*>(lv_obj_get_user_data(obj));
        if (entry != nullptr) {
            break;
        }
        obj = lv_obj_get_parent(obj);
    }
    if (entry == nullptr) {
        entry = static_cast<const AppEntry*>(lv_event_get_user_data(e));
    }
    if (entry == nullptr || (entry->create == nullptr && entry->installed_id.empty() && entry->builtin_id == UiAppId::None)) {
        return;
    }
    // 事件回调里删当前屏不安全，延后一拍再切（与返回键一致）
    auto* launch = new (std::nothrow) AppEntry(*entry);
    if (launch && lv_async_call(Home_LaunchAppAsync, launch) != LV_RESULT_OK) {
        delete launch;
    }
}

lv_obj_t* Home_CreateAppCell(lv_obj_t* parent, const AppEntry& entry, int card_style) {
    // 整卡为唯一点击热区；样式来自设置→主题（NVS）。
    lv_obj_t* cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_update_layout(parent);
    const int card_h =
        std::max<int>(1, (lv_obj_get_content_height(parent) - (HOME_APPS_VISIBLE_ROWS - 1) * HOME_APP_CARD_GAP_ROW) /
                             HOME_APPS_VISIBLE_ROWS);
    lv_obj_set_size(cell, HomeAppCardWidth(), card_h);
    lv_obj_set_style_pad_all(cell, 0, 0);
    Home_ApplyCardStyle(cell, card_style);
    lv_obj_set_style_radius(cell, kCardRadius, 0);
    // 子控件无需圆角裁剪，避免名称上下部分分别走黑白直绘与 ARGB 混合。
    lv_obj_set_style_layout(cell, LV_LAYOUT_NONE, 0);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(cell);
    lv_obj_set_user_data(cell, const_cast<AppEntry*>(&entry));
    lv_obj_add_event_cb(cell, Home_OnAppClicked, LV_EVENT_CLICKED, const_cast<AppEntry*>(&entry));
    lv_obj_t* icon = lv_image_create(cell);
    const int icon_edge = HomeAppIconEdge(card_h, UiFont()->line_height);
    Home_SetAppIcon(icon, entry, icon_edge);
    lv_obj_set_style_image_recolor(icon, UiColorForeground(), 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, HOME_APP_CARD_ICON_PAD_TOP);

    lv_obj_t* name = lv_label_create(cell);
    lv_label_set_text(name, Home_AppLabel(entry));
    ScreenFitLabel(name, HomeAppCardWidth() - UiSx(12),
                   card_h - icon_edge - HOME_APP_CARD_ICON_PAD_TOP - HOME_APP_CARD_NAME_PAD_BOT - UiSy(6));
    lv_obj_add_event_cb(name, Home_OnAppNameDraw, LV_EVENT_DRAW_MAIN_END, nullptr);
    lv_obj_set_style_text_color(name, UiColorForeground(), 0);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -HOME_APP_CARD_NAME_PAD_BOT);
    lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);

    return cell;
}

int Home_card_style = HOME_APP_CARD_STYLE_DEFAULT;
bool Home_card_style_ready = false;

static std::mutex s_card_style_persist_mutex;  // 保护保存快照与任务归属
static int s_pending_card_style = -1;          // 最新待保存的卡片样式
static bool s_card_style_writer = false;       // 单一内部 RAM 写入任务

static int Home_NormalizeCardStyle(int style) {
    if (style == HomeScreen::kCardStyleWhite || style == HomeScreen::kCardStyleGray ||
        style == HomeScreen::kCardStyleBorder || style == HomeScreen::kCardStyleSlash ||
        style == HomeScreen::kCardStylePixel) {
        return style;
    }
    return HOME_APP_CARD_STYLE_DEFAULT;
}

static void PersistCardStyleTask(void*) {
    unsigned failures = 0;
    for (;;) {
        int style;
        {
            std::lock_guard<std::mutex> lock(s_card_style_persist_mutex);
            style = s_pending_card_style;
        }
        bool saved = false;
        try {
            Settings settings("display", true);
            const bool written = settings.SetInt("card_style", style);
            saved = settings.Commit() && written;
        } catch (const std::bad_alloc&) {
            ESP_LOGE(TAG, "operation=save_card_style stage=allocate result=no_memory");
        }
        {
            std::lock_guard<std::mutex> lock(s_card_style_persist_mutex);
            if (style != s_pending_card_style) {
                failures = 0;
                continue;
            }
            if (saved) {
                s_pending_card_style = -1;
            }
            if (saved || ++failures >= 3) {
                s_card_style_writer = false;
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250 * failures));
    }
}

static void Home_LoadCardStyleFromNvs() {
    Settings settings("display", false);
    Home_card_style =
        Home_NormalizeCardStyle(static_cast<int>(settings.GetInt("card_style", HOME_APP_CARD_STYLE_DEFAULT)));
    Home_card_style_ready = true;
}

int HomeScreen::LoadCardStyle() {
    if (!Home_card_style_ready) {
        // 首次读取：只读 NVS；之后一律走内存缓存，避免 LVGL 任务反复碰 flash。
        Home_LoadCardStyleFromNvs();
    }
    return Home_card_style;
}

void HomeScreen::SaveCardStyle(int style) {
    style = Home_NormalizeCardStyle(style);
    Home_card_style = style;
    Home_card_style_ready = true;
    std::lock_guard<std::mutex> lock(s_card_style_persist_mutex);
    s_pending_card_style = style;
    if (s_card_style_writer) {
        return;
    }
    s_card_style_writer = true;
    if (!TaskExecutor::Schedule([] { PersistCardStyleTask(nullptr); }, TaskDomain::Storage)) {
        s_card_style_writer = false;
        ESP_LOGE(TAG, "operation=save_card_style stage=create_task result=no_memory; RAM retained");
    }
}

lv_obj_t* HomeScreen::Create() {
    HomePixel_Stop();
    std::vector<AppStoreItem> items;
    std::string error;
    AppStoreScan(items, error);
    Home_RefreshInstalledApps(std::move(items));
    lv_obj_t* scr = lv_obj_create(nullptr);
    Home_scr = scr;
    Home_WaitForStorage(scr);
    lv_obj_set_style_bg_color(scr, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, UiFont(), 0);
    lv_obj_set_style_text_color(scr, UiColorForeground(), 0);

    // 与其它 App 共用 ScreenCreateStatusBar 高度，勿再强制 HOME 专用高度（图标会相对上移）
    EpdStatusBar status = ScreenCreateStatusBar(scr);

    const int card_style = LoadCardStyle();
    Home_slash_layout = (card_style == kCardStyleSlash);
    const int pages = Home_PageCount();
    if (Home_page >= pages || Home_page < 0) {
        Home_page = 0;
    }
    ESP_LOGI(TAG, "layout apps=%d pages=%d per_page=%d style=%d slash=%d", kTotalApps, pages, kAppsPerPage, card_style,
             Home_slash_layout ? 1 : 0);

    if (card_style == kCardStylePixel) {
        HomeHero_Teardown();
        Home_apps = nullptr;
        Home_page_lbl = nullptr;
        HomePixel_Build(scr, status.height);
        Home_CreatePageIndicator(scr);
        lv_obj_add_event_cb(scr, Home_OnHomeDeleted, LV_EVENT_DELETE, nullptr);
        ScreenSetIsHome(true);
        VkKey_AttachScreen(scr, kScreenId,
                           VkKeyScreenDesc{HomeScreen::Create, Home_OnVkKey, nullptr, Home_OnBootLongPress, nullptr,
                                           nullptr, Home_OnVkKeyLongPress, Home_OnVkKeyPressUp, nullptr, nullptr,
                                           Home_OnPageKey});
        return scr;
    }

    if (Home_slash_layout) {
        auto* hero = HomeHero_MountSlash(scr, status.height);
        HomeHero_Start();

        // 保持三行卡片占内容区约一半，空间不足时再按时钟实际底边收缩。
        lv_obj_update_layout(hero);
        const int hero_bottom = lv_obj_get_y(hero) + lv_obj_get_height(hero);
        const int available = std::max<int>(1, LV_VER_RES - HOME_SLASH_BOTTOM_PAD - hero_bottom - UiSy(12));
        Home_slash_apps_h = std::min<int>((LV_VER_RES - status.height) / 2, available);
        Home_apps = lv_obj_create(scr);
        lv_obj_remove_style_all(Home_apps);
        lv_obj_set_size(Home_apps, LV_HOR_RES, Home_slash_apps_h);
        lv_obj_align(Home_apps, LV_ALIGN_BOTTOM_MID, 0, -HOME_SLASH_BOTTOM_PAD);
        lv_obj_set_style_bg_color(Home_apps, UiColorBackground(), 0);
        lv_obj_set_style_bg_opa(Home_apps, LV_OPA_COVER, 0);
        lv_obj_set_flex_flow(Home_apps, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(Home_apps, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(Home_apps, HOME_SLASH_ROW_GAP, 0);
        lv_obj_add_flag(Home_apps, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_clear_flag(Home_apps, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(Home_apps, LV_OBJ_FLAG_CLICKABLE);

        Home_CreatePageIndicator(scr);
        Home_FillAppsPage();

        lv_obj_add_event_cb(scr, Home_OnHomeDeleted, LV_EVENT_DELETE, nullptr);
        ScreenSetIsHome(true);
        VkKey_AttachScreen(scr, kScreenId,
                           VkKeyScreenDesc{HomeScreen::Create, Home_OnVkKey, nullptr, Home_OnBootLongPress, nullptr,
                                           nullptr, Home_OnVkKeyLongPress, Home_OnVkKeyPressUp, nullptr, nullptr,
                                           Home_OnPageKey});
        return scr;
    }

    // 上半：时分 + 日期天气（历史经典待机上半区；不含待办）
    auto* hero = HomeHero_Mount(scr, status.height);
    HomeHero_Start();

    Home_apps = lv_obj_create(scr);
    lv_obj_remove_style_all(Home_apps);
    Home_PlaceClassicApps(hero);
    lv_obj_set_style_bg_color(Home_apps, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(Home_apps, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(Home_apps, HOME_APPS_PAD_TOP, 0);
    lv_obj_set_style_pad_bottom(Home_apps, HOME_APPS_PAD_BOT, 0);
    lv_obj_set_style_pad_left(Home_apps, HOME_APP_CARD_MARGIN_H, 0);
    lv_obj_set_style_pad_right(Home_apps, HOME_APP_CARD_MARGIN_H, 0);
    lv_obj_set_style_pad_column(Home_apps, HOME_APP_CARD_GAP_COL, 0);
    lv_obj_set_style_pad_row(Home_apps, HOME_APP_CARD_GAP_ROW, 0);
    lv_obj_set_flex_flow(Home_apps, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(Home_apps, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(Home_apps, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(Home_apps, LV_OBJ_FLAG_CLICKABLE);

    Home_CreatePageIndicator(scr);

    Home_FillAppsPage();

    lv_obj_add_event_cb(scr, Home_OnHomeDeleted, LV_EVENT_DELETE, nullptr);
    ScreenSetIsHome(true);
    VkKey_AttachScreen(scr, kScreenId,
                       VkKeyScreenDesc{HomeScreen::Create, Home_OnVkKey, nullptr, Home_OnBootLongPress, nullptr,
                                       nullptr, Home_OnVkKeyLongPress, Home_OnVkKeyPressUp, nullptr, nullptr,
                                       Home_OnPageKey});
    return scr;
}
