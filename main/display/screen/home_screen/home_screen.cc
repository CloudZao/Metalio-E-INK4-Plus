#include "home_screen/home_screen_priv.h"

#include "home_screen_priv.h"
#include "home_screen_ui.h"
#include "home_apps_slash.h"
#include "home_icon_source.h"
#include "home_card_style.h"
#include "pixel/home_pixel.h"
#include "home_screen/home_hero.h"
#include "assistant_screen/assistant_screen.h"
#include "workbuddy_screen/workbuddy_screen.h"
#include "assets/lang_config.h"
#include "book_screen/book_screen.h"
#include "cloud_screen/cloud_screen.h"
#include "haptic_feedback.h"
#include "epd_refresh.h"
#include "screen_common.h"
#include "settings.h"
#include "settings_screen/settings_screen.h"
#include "task_screen/task_screen.h"
#include "vk_key_handler.h"
#include "vk_page_repeat.h"
#include "wallpaper_screen/wallpaper_screen.h"

#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "fontpack_lvgl.h"

uint8_t Home_gray_dither_l8[kDitherW * kDitherH];
lv_image_dsc_t Home_gray_dither_img;
bool Home_gray_dither_ready = false;

void Home_OnAppNameDraw(lv_event_t* e)
{
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

    
    const char *Home_AppNameTask() { return Lang::Strings::HOME_APP_TASK; }
    const char *Home_AppNameAssistant() { return AssistantScreen::DisplayName(true); }
    const char *Home_AppNameBook() { return Lang::Strings::HOME_APP_BOOK; }
    const char *Home_AppNameWallpaper() { return Lang::Strings::HOME_APP_WALLPAPER; }
    const char *Home_AppNameCloud() { return Lang::Strings::HOME_APP_CLOUD; }
    const char *Home_AppNameSettings() { return Lang::Strings::HOME_APP_SETTINGS; }

    const char* Home_AppNameWorkBuddy() { return "workbuddy"; }

    extern const AppEntry kApps[] = {
        {"A:ic_app_home_task.spng", Home_AppNameTask, TaskScreen::Create},
        {"A:ic_app_home_teacher.spng", Home_AppNameAssistant, AssistantScreen::Create},
        {"A:ic_app_home_book.spng", Home_AppNameBook, BookScreen::Create},
        {"A:ic_app_home_wallpaper.spng", Home_AppNameWallpaper, WallpaperScreen::Create},
        {"A:ic_app_home_cloud.spng", Home_AppNameCloud, CloudScreen::Create},
        {"A:ic_app_home_setting.spng", Home_AppNameSettings, SettingsScreen::Create},
        {"A:ic_app_home_workbuddy.spng", Home_AppNameWorkBuddy, WorkBuddyScreen::Create},
    };

    // 仅对外展示用户可见的应用，保持首页与设计稿一致。
    extern const AppEntry kSlashApps[] = {
        {"A:ic_app_home_book.spng", Home_AppNameBook, BookScreen::Create},
        {"A:ic_app_home_wallpaper.spng", Home_AppNameWallpaper, WallpaperScreen::Create},
        {"A:ic_app_home_teacher.spng", Home_AppNameAssistant, AssistantScreen::Create},
        {"A:ic_app_home_task.spng", Home_AppNameTask, TaskScreen::Create},
        {"A:ic_app_home_cloud.spng", Home_AppNameCloud, CloudScreen::Create},
        {"A:ic_app_home_setting.spng", Home_AppNameSettings, SettingsScreen::Create},
        {"A:ic_app_home_workbuddy.spng", Home_AppNameWorkBuddy, WorkBuddyScreen::Create},
    };

    // 每页六个应用，新增应用自动进入后续页。
    extern const int kTotalApps = static_cast<int>(sizeof(kApps) / sizeof(kApps[0]));
    extern const int kSlashAppCount = static_cast<int>(sizeof(kSlashApps) / sizeof(kSlashApps[0]));

lv_obj_t *Home_apps = nullptr;
lv_obj_t *Home_page_lbl = nullptr;
lv_obj_t *Home_scr = nullptr;
int Home_page = 0; // 普通返回保留页码，Home 操作返回第一页
bool Home_slash_layout = false;
lv_coord_t Home_slash_apps_h = 0;

int Home_PageCount()
    {
        return (kTotalApps + kAppsPerPage - 1) / kAppsPerPage;
    }

    void Home_FillAppsPage()
    {
        if (Home_card_style == HomeScreen::kCardStylePixel) {
            HomePixel_FillAppsPage();
            Home_UpdatePageIndicator();
            return;
        }
        if (Home_apps == nullptr)
        {
            return;
        }
        if (Home_slash_layout)
        {
            Home_FillSlashAppsPage();
            Home_UpdatePageIndicator();
            return;
        }
        const int pages = Home_PageCount();
        if (pages < 1)
        {
            return;
        }
        if (Home_page < 0)
        {
            Home_page = 0;
        }
        if (Home_page >= pages)
        {
            Home_page = pages - 1;
        }

        lv_obj_clean(Home_apps);
        const int card_style = HomeScreen::LoadCardStyle();
        const int start = Home_page * kAppsPerPage;
        int end = start + kAppsPerPage;
        if (end > kTotalApps)
        {
            end = kTotalApps;
        }
        for (int i = start; i < end; ++i)
        {
            Home_CreateAppCell(Home_apps, kApps[i], card_style);
        }

        Home_UpdatePageIndicator();
        ESP_LOGI(TAG, "home page %d/%d apps %d..%d of %d", Home_page + 1, pages, start, end - 1,
                 kTotalApps);
    }

void Home_UpdatePageIndicator() {
    if (!Home_page_lbl) return;
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
        lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(dot);
    }
}

static void Home_CreatePageIndicator(lv_obj_t* screen) {
    Home_page_lbl = lv_obj_create(screen);
    lv_obj_remove_style_all(Home_page_lbl);
    lv_obj_set_size(Home_page_lbl, Home_PageCount() * UiSx(20), UiSy(30));
    lv_obj_set_flex_flow(Home_page_lbl, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(Home_page_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(Home_page_lbl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(Home_page_lbl, LV_ALIGN_BOTTOM_MID, 0, -UiSy(8));
    Home_UpdatePageIndicator();
}

    ScreenPaintCoalesce s_home_paint{};

    void Home_RequestFillAppsPage() {
        if (s_home_paint.paint == nullptr) {
            s_home_paint.paint = []() {
                if (!ScreenIsHome() || Home_scr == nullptr) return;
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
            if (changed) Home_RequestFillAppsPage();
        } else ScreenRequestHome();
    };
    if (ScreenLvAsyncUrgent(go) || ScreenLvAsync(go)) return true;
    ESP_LOGW(TAG, "first home page navigation could not be queued");
    return false;
}

    bool HomePageRepeatStep(int page_delta)
    {
        const int pages = Home_PageCount();
        if (pages <= 1 || page_delta == 0) return false;
        // 长按调度传入阅读器的十页步长，首页只取方向逐页循环。
        const int next = (Home_page + (page_delta > 0 ? 1 : pages - 1)) % pages;
        Home_page = next;
        Home_RequestFillAppsPage();
        return true;
    }

    bool Home_OnVkKey(const char *key)
    {
        if (key == nullptr)
        {
            return false;
        }
        if (std::strcmp(key, "vk_home") == 0)
        {
            HomeScreen::GoToFirstPage();
            return true;
        }
        if (std::strcmp(key, "vk_prev") == 0)
        {
            HomePageRepeatStep(-1);
            return true;
        }
        if (std::strcmp(key, "vk_next") == 0)
        {
            HomePageRepeatStep(1);
            return true;
        }
        return false;
    }

    bool Home_OnVkKeyLongPress(const char *key)
    {
        if (key && std::strcmp(key, "vk_home") == 0) return Home_OnVkKey(key);
        return VkPageRepeatTryStart(key, HomePageRepeatStep);
    }

    bool Home_OnVkKeyPressUp(const char *key)
    {
        return VkPageRepeatOnPressUp(key);
    }

    // BOOT 长按快捷进百问（仍按住则 hold-through 聆听）；短按进待机改走电源键
    bool Home_OnBootLongPress()
    {
        ESP_LOGI(TAG, "boot long -> assistant (hold-through PTT if still pressed)");
        AssistantScreen::RequestOpen();
        return true;
    }

    void Home_OnHomeDeleted(lv_event_t *e)
    {
        // ScreenLoadReplace 会 async 删旧屏；勿清掉新首页的指针
        if (lv_event_get_target(e) != Home_scr)
        {
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

    void Home_LaunchAppAsync(void *user_data)
    {
        const auto *entry = static_cast<const AppEntry *>(user_data);
        if (entry == nullptr || entry->create == nullptr)
        {
            return;
        }
        ESP_LOGI(TAG, "launch %s", entry->name());
        ScreenNavigateTo(entry->create);
    }

    void Home_OnAppClicked(lv_event_t *e)
    {
        lv_obj_t *obj = static_cast<lv_obj_t *>(lv_event_get_target(e));
        const AppEntry *entry = nullptr;
        while (obj != nullptr)
        {
            entry = static_cast<const AppEntry *>(lv_obj_get_user_data(obj));
            if (entry != nullptr)
            {
                break;
            }
            obj = lv_obj_get_parent(obj);
        }
        if (entry == nullptr)
        {
            entry = static_cast<const AppEntry *>(lv_event_get_user_data(e));
        }
        if (entry == nullptr || entry->create == nullptr)
        {
            return;
        }
        // 事件回调里删当前屏不安全，延后一拍再切（与返回键一致）
        lv_async_call(Home_LaunchAppAsync, const_cast<AppEntry *>(entry));
    }

    lv_obj_t *Home_CreateAppCell(lv_obj_t *parent, const AppEntry &entry, int card_style)
    {
        // 整卡为唯一点击热区；样式来自设置→主题（NVS）。
        lv_obj_t *cell = lv_obj_create(parent);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, HOME_APP_CARD_W, HOME_APP_CARD_H);
        lv_obj_set_style_pad_all(cell, 0, 0);
        Home_ApplyCardStyle(cell, card_style);
        lv_obj_set_style_radius(cell, kCardRadius, 0);
        lv_obj_set_style_clip_corner(cell, true, 0);
        lv_obj_set_style_layout(cell, LV_LAYOUT_NONE, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(cell);
        lv_obj_set_user_data(cell, const_cast<AppEntry *>(&entry));
        lv_obj_add_event_cb(cell, Home_OnAppClicked, LV_EVENT_CLICKED, const_cast<AppEntry *>(&entry));
        lv_obj_t *icon = lv_image_create(cell);
        lv_image_set_src(icon, HomeIconSource(entry.icon_path));
        lv_obj_set_style_image_recolor(icon, lv_color_black(), 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, HOME_APP_CARD_ICON_PAD_TOP);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *name = lv_label_create(cell);
        lv_label_set_text(name, entry.name());
        lv_obj_set_width(name, HOME_APP_CARD_W);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(name, fontpack_lv_font_ui(), 0);
        lv_obj_add_event_cb(name, Home_OnAppNameDraw, LV_EVENT_DRAW_MAIN_END, nullptr);
        lv_obj_set_style_text_color(name, lv_color_black(), 0);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -HOME_APP_CARD_NAME_PAD_BOT);
        lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);

        return cell;
    }

    int Home_card_style = HOME_APP_CARD_STYLE_DEFAULT;
    bool Home_card_style_ready = false;

    int Home_NormalizeCardStyle(int style)
    {
        if (style == HomeScreen::kCardStyleWhite || style == HomeScreen::kCardStyleGray ||
            style == HomeScreen::kCardStyleBorder || style == HomeScreen::kCardStyleSlash ||
            style == HomeScreen::kCardStylePixel)
        {
            return style;
        }
        return HOME_APP_CARD_STYLE_DEFAULT;
    }

    void Home_PersistCardStyleTask(void *arg)
    {
        const int style = static_cast<int>(reinterpret_cast<intptr_t>(arg));
        Settings settings("display", true);
        settings.SetInt("card_style", style);
        ESP_LOGI(TAG, "nvs card_style=%d", style);
        vTaskDelete(nullptr);
    }

    void Home_LoadCardStyleFromNvs()
    {
        Settings settings("display", false);
        Home_card_style =
            Home_NormalizeCardStyle(static_cast<int>(settings.GetInt("card_style", HOME_APP_CARD_STYLE_DEFAULT)));
        Home_card_style_ready = true;
    }

int HomeScreen::LoadCardStyle()
{
    if (!Home_card_style_ready)
    {
        // 首次读取：只读 NVS；之后一律走内存缓存，避免 LVGL 任务反复碰 flash。
        Home_LoadCardStyleFromNvs();
    }
    return Home_card_style;
}

void HomeScreen::SaveCardStyle(int style)
{
    style = Home_NormalizeCardStyle(style);
    Home_card_style = style;
    Home_card_style_ready = true;
    // 立刻更新内存；写 NVS 放到内部 RAM 栈任务（与网络切换同理）。
    if (xTaskCreate(Home_PersistCardStyleTask, "card_style", 4096,
                    reinterpret_cast<void *>(static_cast<intptr_t>(style)), 5, nullptr) != pdPASS)
    {
        ESP_LOGE("HomeScreen", "xTaskCreate(card_style) failed");
    }
}

lv_obj_t *HomeScreen::Create()
{
    HomePixel_Stop();
    lv_obj_t *scr = lv_obj_create(nullptr);
    Home_scr = scr;
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);

    // 与其它 App 共用 ScreenCreateStatusBar 高度，勿再强制 HOME 专用高度（图标会相对上移）
    EpdStatusBar status = ScreenCreateStatusBar(scr);

    const int card_style = LoadCardStyle();
    Home_slash_layout = (card_style == kCardStyleSlash);
    const int pages = Home_PageCount();
    if (Home_page >= pages || Home_page < 0) Home_page = 0;
    ESP_LOGI(TAG, "layout apps=%d pages=%d per_page=%d style=%d slash=%d", kTotalApps, pages,
             kAppsPerPage, card_style, Home_slash_layout ? 1 : 0);

    if (card_style == kCardStylePixel) {
        HomeHero_Teardown();
        Home_apps = nullptr;
        Home_page_lbl = nullptr;
        HomePixel_Build(scr, status.height);
        Home_CreatePageIndicator(scr);
        lv_obj_add_event_cb(scr, Home_OnHomeDeleted, LV_EVENT_DELETE, nullptr);
        ScreenSetIsHome(true);
        VkKey_AttachScreen(scr, kScreenId,
                           VkKeyScreenDesc{HomeScreen::Create, Home_OnVkKey, nullptr, Home_OnBootLongPress,
                                           nullptr, nullptr, Home_OnVkKeyLongPress, Home_OnVkKeyPressUp});
        return scr;
    }

    if (Home_slash_layout)
    {
        HomeHero_MountSlash(scr, status.height);
        HomeHero_Start();

        // 下半约一半高度留给 3 行斜切宫格（与 397 同比例；勿用 get_height，创建当下可能仍为 0）
        const lv_coord_t content_h = LV_VER_RES - status.height;
        Home_slash_apps_h = content_h / 2;
        if (Home_slash_apps_h < HOME_SLASH_ROWS * HOME_SLASH_ROW_MIN_H)
        {
            Home_slash_apps_h = HOME_SLASH_ROWS * HOME_SLASH_ROW_MIN_H;
        }
        Home_apps = lv_obj_create(scr);
        lv_obj_remove_style_all(Home_apps);
        lv_obj_set_size(Home_apps, LV_HOR_RES, Home_slash_apps_h);
        lv_obj_align(Home_apps, LV_ALIGN_BOTTOM_MID, 0, -HOME_SLASH_BOTTOM_PAD);
        lv_obj_set_style_bg_color(Home_apps, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(Home_apps, LV_OPA_COVER, 0);
        lv_obj_set_flex_flow(Home_apps, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(Home_apps, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(Home_apps, HOME_SLASH_ROW_GAP, 0);
        lv_obj_add_flag(Home_apps, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_clear_flag(Home_apps, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(Home_apps, LV_OBJ_FLAG_CLICKABLE);

        Home_CreatePageIndicator(scr);
        Home_FillAppsPage();

        lv_obj_add_event_cb(scr, Home_OnHomeDeleted, LV_EVENT_DELETE, nullptr);
        ScreenSetIsHome(true);
        VkKey_AttachScreen(scr, kScreenId,
                           VkKeyScreenDesc{HomeScreen::Create, Home_OnVkKey, nullptr, Home_OnBootLongPress,
                                           nullptr, nullptr, Home_OnVkKeyLongPress, Home_OnVkKeyPressUp});
        return scr;
    }

    // 上半：时分 + 日期天气（历史经典待机上半区；不含待办）
    HomeHero_Mount(scr, status.height);
    HomeHero_Start();

    const int indicator_h = pages > 1 ? HOME_PAGE_INDICATOR_H : 0;
    const lv_coord_t apps_content_h =
        HOME_APPS_PAD_TOP + HOME_APPS_VISIBLE_ROWS * HOME_APP_CARD_H +
        (HOME_APPS_VISIBLE_ROWS > 1 ? (HOME_APPS_VISIBLE_ROWS - 1) * HOME_APP_CARD_GAP_ROW : 0) +
        HOME_APPS_PAD_BOT;
    const lv_coord_t apps_bottom_inset =
        HOME_APPS_LIFT + (indicator_h > 0 ? indicator_h : 0);

    Home_apps = lv_obj_create(scr);
    lv_obj_remove_style_all(Home_apps);
    lv_obj_set_size(Home_apps, LV_HOR_RES, apps_content_h);
    lv_obj_align(Home_apps, LV_ALIGN_BOTTOM_MID, 0, -apps_bottom_inset);
    lv_obj_set_style_bg_color(Home_apps, lv_color_white(), 0);
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
                       VkKeyScreenDesc{HomeScreen::Create, Home_OnVkKey, nullptr, Home_OnBootLongPress,
                                       nullptr, nullptr, Home_OnVkKeyLongPress, Home_OnVkKeyPressUp});
    return scr;
}
