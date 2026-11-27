#include <algorithm>
#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include "wifi_station.h"
#include "ui/common/runtime/page_lifecycle.h"
#include "ui/common/drawing/font/font_awesome.h"
#include "ui/common/drawing/adapter/epd_refresh.h"
#include "boards/common/display/frontlight.h"
#include "boards/common/bluetooth/ble_page_turner.h"
#include "boards/common/storage/sd_log_writer.h"
#include "business/bluetooth/bluetooth_control.h"
#include "ui/home/home_screen.h"
#include "infra/storage/sd_paths.h"
#include "assets/lang_screen.h"
#include "assets/lang_bluetooth.h"
#include "assets/lang_pixel.h"
#include "ui/apps/settings/settings_font_tab.h"
#include "ui/apps/settings/settings_test/settings_test_tab.h"
#include "ui/apps/settings/settings_screen.h"
#include "ui/apps/settings/settings_keys.h"
#include "assets/lang_charging.h"
#include "assets/lang_config.h"
#include "ui/apps/settings/settings_tabs.h"
#include "ui/apps/settings/settings_common.h"
#include "ui/apps/settings/settings_conversation_tab.h"
#include "ui/apps/settings/settings_frontlight_tab.h"
#include "ui/apps/settings/settings_log_upload.h"
#include "ui/apps/settings/settings_network_tab.h"
#include "ui/apps/settings/network/network_screen.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/apps/settings/bluetooth/bluetooth_screen.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/widgets/screen_text.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/runtime/screen_common_nav.h"

#define TAG "SettingsScreen"
LV_FONT_DECLARE(font_awesome_30_1);

static constexpr const char* kScreenId = "settings";
static constexpr int kGestureIconHeight = 32; // 手势图标绘制高度，单位：px

enum class SettingsTab : int {
    kHome = -1,  // 分组首页
    kWifi = 16,  // Wi-Fi 开关及网络列表
    kFont = 14,
    kScreen = 15,
    kNetwork = 0,
    kTheme = 1,
    kLanguage = 2,
    kHaptic = 3,
    kPower = 4,
    kFrontlight = 5,
    kConversation = 6,
    kBluetooth = 7,
    kPager = 12,  // BLE 翻页器
    kKeys = 13,   // 按键与手势
    kTest = 8,
    kLog = 9,
    kAbout = 10,     // 关于始终放最后
};

struct SettingsTabEntry {
    SettingsTab id;
    const char* const* name; // 指向语言资源槽，切换语言后按需读取
    void (*build)(lv_obj_t*); // 在当前设置内容区构建控件
    void (*reset)(); // 清理本Tab资源，空表示无需独立清理
};

static void BuildWifiPage(lv_obj_t* page);

static const char* s_wifi_tab_name = "Wi-Fi";
static const char* s_keys_tab_name = "Keys";
static const char* s_font_tab_name = "System font";
static const char* s_screen_tab_name = "Screen";
static constexpr SettingsTabEntry kTabs[] = {
    {SettingsTab::kWifi, &s_wifi_tab_name, BuildWifiPage, SettingsNetworkTab_Reset},
    {SettingsTab::kFont, &s_font_tab_name, SettingsFontTab_Build, SettingsFontTab_Reset},
    {SettingsTab::kScreen, &s_screen_tab_name, SettingsDisplayTab_Build, SettingsFrontlightTab_Reset},
    {SettingsTab::kNetwork, &LangString_SETTINGS_TAB_NETWORK, SettingsNetworkTab_Build, SettingsNetworkTab_Reset},
    {SettingsTab::kTheme, &LangString_SETTINGS_TAB_THEME, SettingsThemeTab_Build, SettingsThemeTab_Reset},
    {SettingsTab::kLanguage, &LangString_SETTINGS_TAB_LANGUAGE, SettingsLanguageTab_Build, SettingsLanguageTab_Reset},
    {SettingsTab::kHaptic, &LangString_SETTINGS_TAB_HAPTIC, SettingsHapticTab_Build, SettingsHapticTab_Reset},
    {SettingsTab::kPower, &LangString_SETTINGS_TAB_POWER, SettingsPowerTab_Build, SettingsPowerTab_Reset},
    {SettingsTab::kFrontlight, &LangString_SETTINGS_TAB_FRONTLIGHT, SettingsFrontlightTab_Build, SettingsFrontlightTab_Reset},
    {SettingsTab::kConversation, &LangString_SETTINGS_TAB_CONVERSATION, SettingsConversationTab_Build, SettingsConversationTab_Reset},
    {SettingsTab::kBluetooth, &LangString_SETTINGS_TAB_BLUETOOTH, BluetoothScreen::BuildInto, BluetoothScreen::ResetUi},
    {SettingsTab::kPager, &LangString_SETTINGS_PAGER, SettingsPagerTab_Build, SettingsPagerTab_Reset},
    {SettingsTab::kKeys, &s_keys_tab_name, SettingsKeysBuild, nullptr},
    {SettingsTab::kTest, &LangString_SETTINGS_TAB_TEST, SettingsTestTab_Build, nullptr},
    {SettingsTab::kLog, &LangString_SETTINGS_TAB_LOG, SettingsLogTab_Build, SettingsLogTab_Reset},
    {SettingsTab::kAbout, &LangString_SETTINGS_TAB_ABOUT, SettingsAboutTab_Build, SettingsAboutTab_Reset},
};
static constexpr int kTabCount = sizeof(kTabs) / sizeof(kTabs[0]);
static_assert(kTabs[kTabCount - 1].id == SettingsTab::kAbout, "about tab must be last");

struct SettingsShellUi {
    lv_obj_t* wifi_controls = nullptr;  // 仅在 Wi-Fi 列表显示的开关与连接方式
    lv_obj_t* wifi_hint = nullptr;  // 无线网络不可用时的说明
    bool wifi_shown = false;  // Wi-Fi 扫描面板是否已接入
    bool return_to_wifi = false;  // 网络设置子页的返回目标
    lv_obj_t* title = nullptr;  // 当前页面的居中标题
    lv_obj_t* back = nullptr;  // 子页返回按钮
    lv_coord_t home_offset = 0;  // 返回设置首页时恢复的位置
    lv_obj_t* content_page = nullptr;  // 首页和独立功能页共用的内容容器
    int active_tab = -1;  // -1 为设置首页，其余为功能表索引
    bool content_built = false;  // 当前功能已构建，离开时须清理
};

static SettingsShellUi s_ui;
static UiPageLifecycle s_page_lifecycle;
static SettingsTab s_pending_tab = SettingsTab::kHome;
static lv_obj_t* s_settings_scr = nullptr;

static void RefreshWifiPanel() {
    if (s_ui.active_tab < 0 || kTabs[s_ui.active_tab].id != SettingsTab::kWifi || !s_ui.wifi_hint) {
        return;
    }
    const bool wifi = std::strcmp(SettingsNetworkTab_CurrentName(), "WiFi") == 0;
    const bool enabled = wifi && WifiStation::GetInstance().IsUserEnabled();
    if (enabled && s_ui.wifi_shown) {
        return;
    }
    if (enabled && NetworkScreen::BuildSettings(s_ui.content_page)) {
        s_ui.wifi_shown = true;
        lv_obj_add_flag(s_ui.wifi_hint, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (!enabled && s_ui.wifi_shown) {
        SettingsScreen::SetWifiDetail(nullptr);
        NetworkScreen::CloseSettings();
        s_ui.wifi_shown = false;
    }
    if (!wifi) {
        lv_obj_add_flag(s_ui.wifi_hint, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const bool chinese = std::strcmp(LangCode, "zh-CN") == 0;
    const char* hint = enabled ? (chinese ? "正在准备网络…" : "Preparing network...")
                               : (chinese ? "Wi-Fi 已关闭" : "Wi-Fi is off.");
    if (std::strcmp(lv_label_get_text(s_ui.wifi_hint), hint) != 0) {
        lv_label_set_text(s_ui.wifi_hint, hint);
    }
    lv_obj_clear_flag(s_ui.wifi_hint, LV_OBJ_FLAG_HIDDEN);
}

static void BuildWifiPage(lv_obj_t* page) {
    auto* controls = SettingsCreateListGroup(page);
    s_ui.wifi_controls = controls;
    SettingsNetworkTab_BuildWifi(controls);
    const bool wifi = std::strcmp(SettingsNetworkTab_CurrentName(), "WiFi") == 0;
    SettingsCreateListRow(controls, std::strcmp(LangCode, "zh-CN") == 0 ? "网络设置" : "Network settings",
                          wifi ? "Wi-Fi" : "4G", [](lv_event_t*) { SettingsScreen::OpenNetworkMode(); });
    s_ui.wifi_shown = false;
    s_ui.wifi_hint = lv_label_create(page);
    lv_label_set_text(s_ui.wifi_hint, "");
    lv_obj_set_width(s_ui.wifi_hint, lv_pct(100));
    lv_label_set_long_mode(s_ui.wifi_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.wifi_hint, UiFont(UiFontRole::Caption), 0);
    RefreshWifiPanel();
}

static void ShowHome();

static int TabIndexOf(SettingsTab id) {
    for (int i = 0; i < kTabCount; ++i) {
        if (kTabs[i].id == id) {
            return i;
        }
    }
    return 0;
}

static ScreenFactory FactoryForTab(SettingsTab id) {
    if (id == SettingsTab::kWifi) {
        return SettingsScreen::CreateNetwork;
    }
    if (id == SettingsTab::kKeys) {
        return SettingsScreen::CreateKeys;
    }
    if (id == SettingsTab::kPager) {
        return SettingsScreen::CreatePager;
    }
    if (id == SettingsTab::kBluetooth) {
        return SettingsScreen::CreateBluetooth;
    }
    if (id == SettingsTab::kTest) {
        return SettingsScreen::CreateTest;
    }
    return SettingsScreen::Create;
}

static void ResetTab(SettingsTab id) {
    if (id == SettingsTab::kWifi) {
        NetworkScreen::CloseSettings();
        s_ui.wifi_hint = nullptr;
        s_ui.wifi_controls = nullptr;
        s_ui.wifi_shown = false;
    }
    for (const auto& tab : kTabs) {
        if (tab.id == id && tab.reset != nullptr) {
            tab.reset();
            return;
        }
    }
}

static void BuildTab(SettingsTab id) {
    lv_obj_t* page = s_ui.content_page;
    if (page == nullptr) {
        return;
    }
    for (const auto& tab : kTabs) {
        if (tab.id == id) {
            tab.build(page);
            break;
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(page); ++i) {
        auto* child = lv_obj_get_child(page, i);
        if (lv_obj_check_type(child, &lv_label_class)) {
            lv_obj_set_width(child, LV_PCT(100));
            lv_label_set_long_mode(child, LV_LABEL_LONG_WRAP);
        }
    }
    s_ui.content_built = true;
}

static void TearDownActiveTab() {
    if (s_ui.content_page == nullptr || !s_ui.content_built || s_ui.active_tab < 0 || s_ui.active_tab >= kTabCount) {
        return;
    }
    const SettingsTab id = kTabs[s_ui.active_tab].id;
    if (id == SettingsTab::kBluetooth) {
        BluetoothScreen::OnDeactivated();
    }
    ResetTab(id);
    lv_obj_clean(s_ui.content_page);
    s_ui.content_built = false;
}

static void ShowTab(int index) {
    if (index < 0 || index >= kTabCount) {
        return;
    }
    if (s_ui.content_built && s_ui.active_tab == index) {
        return;
    }

    TearDownActiveTab();
    s_ui.active_tab = index;
    lv_obj_set_style_pad_row(s_ui.content_page, kSettingsSectionGap, 0);

    lv_obj_clear_flag(s_ui.back, LV_OBJ_FLAG_HIDDEN);
    SettingsScreen::SetTitle(*kTabs[index].name);
    lv_obj_scroll_to_y(s_ui.content_page, 0, LV_ANIM_OFF);

    const SettingsTab id = kTabs[index].id;
    BuildTab(id);
    if (id == SettingsTab::kConversation) {
        SettingsConversationTab_OnActivated();
    }
    if (id == SettingsTab::kBluetooth) {
        BluetoothScreen::OnActivated();
    }
    VkKey_SetScreenFactory(kScreenId, FactoryForTab(id));
}

static const char* SettingsText(const char* zh, const char* en) {
    return std::strcmp(LangCode, "zh-CN") == 0 ? zh : en;
}

static const char* TabIcon(SettingsTab id) {
    switch (id) {
        case SettingsTab::kWifi: return FONT_AWESOME_WIFI;
        case SettingsTab::kBluetooth: return FONT_AWESOME_BLUETOOTH;
        case SettingsTab::kPager: return FONT_AWESOME_GAMEPAD;
        case SettingsTab::kTheme: return FONT_AWESOME_IMAGE;
        case SettingsTab::kFont: return "T";
        case SettingsTab::kFrontlight: return FONT_AWESOME_BRIGHTNESS;
        case SettingsTab::kScreen: return FONT_AWESOME_ARROWS_ROTATE;
        case SettingsTab::kHaptic: return FONT_AWESOME_BELL;
        case SettingsTab::kKeys: return "";
        case SettingsTab::kConversation: return FONT_AWESOME_COMMENT;
        case SettingsTab::kPower: return FONT_AWESOME_POWER_OFF;
        case SettingsTab::kLanguage: return FONT_AWESOME_GLOBE;
        case SettingsTab::kTest: return FONT_AWESOME_MICROCHIP_AI;
        case SettingsTab::kLog: return FONT_AWESOME_PEN_TO_SQUARE;
        default: return FONT_AWESOME_CIRCLE_INFO;
    }
}

static void DrawGestureIcon(lv_event_t* event) {
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_current_target_obj(event), &area);
    auto* layer = lv_event_get_layer(event);
    auto scale = [](int value) { return value * kGestureIconHeight / 37; };
    const int left = area.x1 + (lv_area_get_width(&area) - scale(24)) / 2;
    const int top = area.y1 + (lv_area_get_height(&area) - kGestureIconHeight) / 2;
    static constexpr lv_point_t outline[] = {
        {7, 22}, {7, 12}, {8, 10}, {10, 10}, {11, 12}, {11, 18},
        {13, 17}, {15, 18}, {15, 20}, {17, 19}, {19, 20}, {19, 22},
        {21, 21}, {22, 23}, {22, 28}, {19, 35}, {9, 35}, {8, 31},
        {5, 26}, {3, 23}, {3, 21}, {5, 20}, {7, 22},
    };
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = UiColorForeground();
    line.width = 1;
    line.round_start = true;
    line.round_end = true;
    for (size_t i = 1; i < sizeof(outline) / sizeof(outline[0]); ++i) {
        line.p1 = {left + scale(outline[i - 1].x), top + scale(outline[i - 1].y)};
        line.p2 = {left + scale(outline[i].x), top + scale(outline[i].y)};
        lv_draw_line(layer, &line);
    }
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color = UiColorForeground();
    arc.width = line.width;
    arc.rounded = true;
    arc.center = {left + scale(9), top + scale(10)};
    arc.start_angle = 200;
    arc.end_angle = 340;
    for (int radius : {4, 8}) {
        arc.radius = scale(radius);
        lv_draw_arc(layer, &arc);
    }
}

static void OnHomeRowClicked(lv_event_t* event) {
    s_ui.home_offset = lv_obj_get_scroll_y(s_ui.content_page);
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    lv_obj_clean(s_ui.content_page);
    ShowTab(index);
}

static void CreateHomeRow(lv_obj_t* parent, SettingsTab id) {
    if (id == SettingsTab::kFrontlight && !BOARD_HAS_FRONTLIGHT) {
        return;
    }
    const int index = TabIndexOf(id);
    const char* name = *kTabs[index].name;
    if (id == SettingsTab::kWifi) {
        name = "Wi-Fi";
    }
    char summary[SD_FILE_NAME_CAPACITY] = {};
    switch (id) {
        case SettingsTab::kWifi: {
            const auto ssid = WifiStation::GetInstance().GetSsid();
            std::snprintf(summary, sizeof(summary), "%s", !WifiStation::GetInstance().IsConnected() || ssid.empty() ? SettingsText("未连接", "Not connected") : ssid.c_str());
            break;
        }
        case SettingsTab::kBluetooth: {
            const auto mode = BluetoothCurrentMode();
            const char* text = mode == BtMode::kMode1 ? LangString_BT_MODE1 :
                               mode == BtMode::kMode2 ? LangString_BT_MODE2 :
                               mode == BtMode::kMode3 ? LangString_BT_MODE3 :
                               mode == BtMode::kMode5 ? LangString_BT_MODE5 : LangString_COMMON_UNKNOWN;
            std::snprintf(summary, sizeof(summary), "%s", text);
            break;
        }
        case SettingsTab::kPager: {
            const auto pager = BlePageTurnerGetSnapshot();
            std::snprintf(summary, sizeof(summary), "%s", pager.state == BlePageTurnerState::kConnected
                ? BlePageTurnerModelName(pager.target_model) : SettingsText("未连接", "Not connected"));
            break;
        }
        case SettingsTab::kTheme: {
            const char* names[] = {LangString_SETTINGS_THEME_WHITE, LangString_SETTINGS_THEME_GRAY,
                LangString_SETTINGS_THEME_BORDER, LangString_SETTINGS_THEME_SLASH, LangString_SETTINGS_THEME_PIXEL};
            const int style = HomeScreen::LoadCardStyle();
            std::snprintf(summary, sizeof(summary), "%s", names[std::clamp(style, 0, 4)]);
            break;
        }
        case SettingsTab::kFont:
            std::snprintf(summary, sizeof(summary), "%s", UiFontFile()[0] ? UiFontFile() : SettingsText("默认字体", "Default font"));
            break;
        case SettingsTab::kFrontlight:
            if (Frontlight::GetInstance().cct() == FrontlightCct::kOff) {
                std::snprintf(summary, sizeof(summary), "%s", SettingsText("关闭", "Off"));
            } else {
                std::snprintf(summary, sizeof(summary), "%u%%", Frontlight::GetInstance().target_brightness());
            }
            break;
        case SettingsTab::kScreen: {
            const char* names[] = {LangString_SETTINGS_SCREEN_REFRESH_CLEAR, LangString_SETTINGS_SCREEN_REFRESH_BALANCED,
                                   LangString_SETTINGS_SCREEN_REFRESH_FAST};
            std::snprintf(summary, sizeof(summary), "%s", names[std::clamp(static_cast<int>(EpdGetRefreshProfile()), 0, 2)]);
            break;
        }
        case SettingsTab::kHaptic:
            std::snprintf(summary, sizeof(summary), "%s", HapticIsEnabled() ? SettingsText("开启", "On") : SettingsText("关闭", "Off"));
            break;
        case SettingsTab::kLog:
            std::snprintf(summary, sizeof(summary), "%s", SdLogWriter_IsEnabled() ? SettingsText("开启", "On") : SettingsText("关闭", "Off"));
            break;
        case SettingsTab::kLanguage:
            std::snprintf(summary, sizeof(summary), "%s", SettingsText("简体中文", "English"));
            break;
        default:
            break;
    }
    auto* row = SettingsCreateListRow(parent, name, summary, OnHomeRowClicked, index);
    auto* icon = lv_label_create(row);
    lv_label_set_text(icon, TabIcon(id));
    lv_obj_set_style_text_font(icon, id == SettingsTab::kFont ? UiFont() : &font_awesome_30_1, 0);
    lv_obj_set_width(icon, UiSx(24));
    if (id == SettingsTab::kKeys) {
        lv_obj_set_height(icon, kGestureIconHeight);
        lv_obj_add_event_cb(icon, DrawGestureIcon, LV_EVENT_DRAW_MAIN, nullptr);
    }
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_move_to_index(icon, 0);
}

static void ShowHome() {
    TearDownActiveTab();
    lv_obj_clean(s_ui.content_page);
    s_ui.active_tab = -1;
    s_ui.return_to_wifi = false;
    lv_obj_set_style_pad_row(s_ui.content_page, UiSy(8), 0);
    SettingsScreen::SetTitle(SettingsText("设置", "Settings"));
    lv_obj_add_flag(s_ui.back, LV_OBJ_FLAG_HIDDEN);
    SettingsCreateSectionTitle(s_ui.content_page, SettingsText("连接", "Connections"));
    auto* connections = SettingsCreateListGroup(s_ui.content_page);
    for (auto id : {SettingsTab::kWifi, SettingsTab::kBluetooth, SettingsTab::kPager}) {
        CreateHomeRow(connections, id);
    }
    SettingsCreateSectionTitle(s_ui.content_page, SettingsText("显示", "Display"));
    auto* display = SettingsCreateListGroup(s_ui.content_page);
    for (auto id : {SettingsTab::kTheme, SettingsTab::kFont, SettingsTab::kFrontlight, SettingsTab::kScreen}) {
        CreateHomeRow(display, id);
    }
    SettingsCreateSectionTitle(s_ui.content_page, SettingsText("使用偏好", "Preferences"));
    auto* preferences = SettingsCreateListGroup(s_ui.content_page);
    for (auto id : {SettingsTab::kHaptic, SettingsTab::kKeys, SettingsTab::kConversation,
                    SettingsTab::kPower, SettingsTab::kLanguage}) {
        CreateHomeRow(preferences, id);
    }
    SettingsCreateSectionTitle(s_ui.content_page, SettingsText("系统", "System"));
    auto* system = SettingsCreateListGroup(s_ui.content_page);
    for (auto id : {SettingsTab::kTest, SettingsTab::kLog, SettingsTab::kAbout}) {
        CreateHomeRow(system, id);
    }
    for (auto* group : {connections, display, preferences, system}) {
        lv_obj_set_style_border_width(lv_obj_get_child(group, -1), 0, 0);
    }
    lv_obj_update_layout(s_ui.content_page);
    lv_obj_scroll_to_y(s_ui.content_page, s_ui.home_offset, LV_ANIM_OFF);
    VkKey_SetScreenFactory(kScreenId, SettingsScreen::Create);
}

static void OnScreenDeleted(lv_event_t* e) {
    if (lv_event_get_target(e) != s_settings_scr) {
        return;
    }
    SettingsLogUpload_Teardown();
    TearDownActiveTab();
    s_ui = {};
    s_settings_scr = nullptr;
}

static bool HandleSettingsBack() {
    if (BluetoothScreen::DismissOverlay()) {
        return true;
    }
    if (s_ui.active_tab >= 0 && kTabs[s_ui.active_tab].id == SettingsTab::kWifi && NetworkScreen::OnVkKey("vk_home")) {
        return true;
    }
    if (s_ui.active_tab >= 0) {
        if (SettingsFontTab_Back()) {
            return true;
        }
        if (s_ui.return_to_wifi) {
            s_ui.return_to_wifi = false;
            ShowTab(TabIndexOf(SettingsTab::kWifi));
        } else {
            ShowHome();
        }
        return true;
    }
    return false;
}

static bool OnSettingsKey(const char* key) {
    if (!key) {
        return false;
    }
    if (std::strcmp(key, "vk_home") == 0) {
        return HandleSettingsBack();
    }
    if (s_ui.active_tab >= 0 && kTabs[s_ui.active_tab].id == SettingsTab::kWifi && NetworkScreen::OnVkKey(key)) {
        return true;
    }
    if (s_ui.active_tab >= 0 && kTabs[s_ui.active_tab].id == SettingsTab::kFont &&
        (std::strcmp(key, "vk_prev") == 0 || std::strcmp(key, "vk_next") == 0) &&
        SettingsFontTab_Scroll(std::strcmp(key, "vk_next") == 0 ? 1 : -1)) {
        return true;
    }
    if (std::strcmp(key, "vk_prev") == 0) {
        return SettingsScrollPage(s_ui.content_page, -1) || HandleSettingsBack();
    }
    if (std::strcmp(key, "vk_next") == 0) {
        SettingsScrollPage(s_ui.content_page, 1);
        return true;
    }
    return false;
}

static bool OnSettingsPageKey(const char* key, bool long_press) {
    if (s_ui.active_tab >= 0 && kTabs[s_ui.active_tab].id == SettingsTab::kWifi &&
        NetworkScreen::OnPageKey(key, long_press)) {
        return true;
    }
    if (!s_ui.content_page) {
        return false;
    }
    if (!long_press) {
        const int direction = std::strcmp(key, "vk_next") == 0 ? 1 : -1;
        if (s_ui.active_tab < 0 || kTabs[s_ui.active_tab].id != SettingsTab::kFont ||
            !SettingsFontTab_Scroll(direction)) {
            SettingsScrollPage(s_ui.content_page, direction);
        }
    }
    return true;
}

static bool OnSettingsLongKey(const char* key) {
    if (key && strcmp(key, "vk_home") == 0 && BluetoothScreen::DismissOverlay()) {
        return true;
    }
    return false;
}

static lv_obj_t* CreateInternal() {
    s_keys_tab_name = SettingsText("按键与手势", "Keys & gestures");
    s_font_tab_name = SettingsText("系统字体", "System font");
    s_screen_tab_name = SettingsText("屏幕", "Screen");
    s_ui = {};

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_settings_scr = scr;
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, UiFont(), 0);
    lv_obj_set_style_text_color(scr, UiColorForeground(), 0);
    lv_obj_add_event_cb(scr, OnScreenDeleted, LV_EVENT_DELETE, nullptr);

    EpdStatusBar status = ScreenCreateStatusBar(scr);

    auto* header = lv_obj_create(scr);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_HOR_RES, UiSy(52));
    lv_obj_set_pos(header, 0, status.height);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    s_ui.title = lv_label_create(header);
    lv_obj_set_style_text_font(s_ui.title, UiFont(UiFontRole::TitleEmphasis), 0);
    lv_obj_center(s_ui.title);
    s_ui.back = SettingsCreateBackButton(header, nullptr);
    lv_obj_align(s_ui.back, LV_ALIGN_LEFT_MID, UiSx(6), 0);
    lv_obj_add_event_cb(s_ui.back, [](lv_event_t*) {
        HandleSettingsBack();
    }, LV_EVENT_CLICKED, nullptr);

    auto* content = lv_obj_create(scr);
    lv_obj_remove_style_all(content);
    const int top = status.height + UiSy(52);
    const int side_inset = UiSx(8);
    lv_obj_set_pos(content, side_inset, top);
    lv_obj_set_size(content, LV_HOR_RES - 2 * side_inset, LV_VER_RES - top - UiSy(16));
    lv_obj_set_style_pad_hor(content, UiSx(16) - 6, 0);
    lv_obj_set_style_pad_ver(content, UiSy(16), 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, UiSy(8), 0);
    SettingsAttachPagedScroll(content);
    s_ui.content_page = content;

    const SettingsTab open_tab = s_pending_tab;
    s_pending_tab = SettingsTab::kHome;

    ScreenSetIsHome(false);
    VkKeyScreenDesc keys{FactoryForTab(open_tab)};
    keys.on_key = OnSettingsKey;
    keys.on_page_key = OnSettingsPageKey;
    keys.on_key_long_press = OnSettingsLongKey;
    VkKey_AttachScreen(scr, kScreenId, keys);
    if (open_tab == SettingsTab::kHome) {
        ShowHome();
    } else {
        ShowTab(TabIndexOf(open_tab));
    }
    ESP_LOGI(TAG, "settings ready tabs=%d open=%d haptic=%d", kTabCount, (int)open_tab, HapticIsEnabled() ? 1 : 0);
    s_page_lifecycle.Bind(scr, {[] {
        RefreshWifiPanel();
        SettingsKeysRefresh();
    }, [](UiPagePowerEvent event) {
        if (event == UiPagePowerEvent::PauseViews) {
            SettingsScreen_OnEnterStandby();
        } else if (event == UiPagePowerEvent::ResumeViews) {
            SettingsScreen_OnResumeFromStandby();
        }
    }, nullptr, 200});
    return scr;
}

lv_obj_t* SettingsScreen::Create() {
    s_pending_tab = SettingsTab::kHome;
    return CreateInternal();
}

lv_obj_t* SettingsScreen::CreateNetwork() {
    s_pending_tab = SettingsTab::kWifi;
    return CreateInternal();
}

lv_obj_t* SettingsScreen::CreatePager() {
    s_pending_tab = SettingsTab::kPager;
    return CreateInternal();
}

lv_obj_t* SettingsScreen::CreateBluetooth() {
    s_pending_tab = SettingsTab::kBluetooth;
    return CreateInternal();
}

lv_obj_t* SettingsScreen::CreateTest() {
    s_pending_tab = SettingsTab::kTest;
    return CreateInternal();
}

lv_obj_t* SettingsScreen::CreateKeys() {
    s_pending_tab = SettingsTab::kKeys;
    return CreateInternal();
}

void SettingsScreen::ReloadAppearance() {
    s_pending_tab = s_ui.active_tab < 0 ? SettingsTab::kHome : kTabs[s_ui.active_tab].id;
    TearDownActiveTab();
    ScreenLoadReplace(CreateInternal());
}

void SettingsScreen::SetTitle(const char* title) {
    if (s_ui.title && title) {
        lv_label_set_text(s_ui.title, title);
        ScreenFitLabel(s_ui.title, LV_HOR_RES - UiSx(120), UiSy(48), UiFont(UiFontRole::TitleEmphasis));
        lv_obj_center(s_ui.title);
    }
}

void SettingsScreen_OnEnterStandby() {
    SettingsLogUpload_Teardown();
}

void SettingsScreen_OnResumeFromStandby() {
    SettingsFrontlightTab_Refresh();
}

void SettingsScreen::OpenNetworkMode() {
    s_ui.return_to_wifi = true;
    ShowTab(TabIndexOf(SettingsTab::kNetwork));
}

void SettingsScreen::SetWifiDetail(const char* title) {
    if (!s_ui.wifi_controls) {
        return;
    }
    lv_obj_set_flag(s_ui.wifi_controls, LV_OBJ_FLAG_HIDDEN, title != nullptr);
    SetTitle(title ? title : "Wi-Fi");
    lv_obj_scroll_to_y(s_ui.content_page, 0, LV_ANIM_OFF);
}
