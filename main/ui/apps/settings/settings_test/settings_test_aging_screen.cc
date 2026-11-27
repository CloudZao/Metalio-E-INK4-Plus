#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include "settings_test_aging_sd.h"
#include "settings_test_screens.h"
#include "assets/lang_config.h"
#include "business/diagnostics/aging_test.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/apps/settings/settings_screen.h"
#include "ui/common/runtime/page_lifecycle.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/runtime/screen_common_nav.h"
#include "ui/common/widgets/screen_options.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/drawing/ui_scale.h"
#include "ui/common/drawing/ui_theme.h"

LV_FONT_DECLARE(font_misans_regular_160_2);

static constexpr const char* TAG = "SettingsTestAging";
static constexpr const char* kScreenId = "settings_test_aging";
static constexpr lv_coord_t kPad = UiSx(12);
static UiPageLifecycle s_page;
static lv_obj_t* s_scr = nullptr;
static lv_obj_t* s_counter_lbl = nullptr;
static bool s_exit_pending = false;
static bool s_exit_to_home = false;
static bool s_exit_navigation_queued = false;
static bool s_save_error_shown = false;
static uint32_t s_shown_cycles = UINT32_MAX;

static const lv_font_t* AgingTitleFont() {
    return UiFont(UiFontRole::Title);
}

static void ClearStatusBindings() {
    // 全屏无顶栏：必须摘掉上一页控件指针，否则 1Hz UpdateStatusBar 写已释放对象搞坏 LVGL
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->BindStatusWidgets(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    }
}

static int DigitsPerLine() {
    const lv_font_t* font = &font_misans_regular_160_2;
    lv_point_t sz = {};
    lv_text_get_size(&sz, "0", font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int avail = static_cast<int>(LV_HOR_RES) - 2 * kPad;
    if (sz.x <= 0) {
        return 4;
    }
    int n = avail / static_cast<int>(sz.x);
    return n < 1 ? 1 : n;
}

static void FormatWrappedCounter(char* out, size_t out_sz, uint32_t value) {
    if (out == nullptr || out_sz == 0) {
        return;
    }
    char digits[16];
    std::snprintf(digits, sizeof(digits), "%lu", static_cast<unsigned long>(value));
    const int per_line = DigitsPerLine();
    size_t o = 0;
    for (size_t i = 0; digits[i] != '\0'; ++i) {
        if (i > 0 && (static_cast<int>(i) % per_line) == 0) {
            if (o + 1 >= out_sz) {
                break;
            }
            out[o++] = '\n';
        }
        if (o + 1 >= out_sz) {
            break;
        }
        out[o++] = digits[i];
    }
    out[(o < out_sz) ? o : (out_sz - 1)] = '\0';
}

static void UpdateCounterLabel(uint32_t counter) {
    if (s_counter_lbl == nullptr || !lv_obj_is_valid(s_counter_lbl)) {
        return;
    }
    char buf[48];
    FormatWrappedCounter(buf, sizeof(buf), counter);
    lv_label_set_text(s_counter_lbl, buf);
    // 大字累加用全刷，避免局刷残影叠字
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->RequestNextFullRefresh();
    }
    lv_obj_invalidate(s_counter_lbl);
    if (s_scr != nullptr && lv_obj_is_valid(s_scr)) {
        lv_obj_invalidate(s_scr);
    }
}

static void PollPresentation() {
    const auto snapshot = AgingTestRead();
    if (snapshot.cycles != s_shown_cycles) {
        s_shown_cycles = snapshot.cycles;
        UpdateCounterLabel(snapshot.cycles);
    }
    SettingsTestAgingSd_Show(snapshot.preview);
    if (snapshot.save_failed && !s_save_error_shown) {
        s_save_error_shown = true;
        ScreenShowToast(LangString_SETTINGS_TEST_AGING_SAVE_FAILED);
    } else if (!snapshot.save_failed) {
        s_save_error_shown = false;
    }
    if (s_exit_pending && !s_exit_navigation_queued && snapshot.phase == AgingTestPhase::Idle) {
        s_exit_navigation_queued = true;
        if (s_exit_to_home) {
            ScreenGoHome();
        } else {
            ScreenNavigateBack();
        }
    }
}

static void RequestAgingExit(bool home = false) {
    if (s_exit_pending) {
        PollPresentation();
        return;
    }
    s_exit_pending = true;
    s_exit_to_home = home;
    s_exit_navigation_queued = false;
    AgingTestStop();
    SettingsTestAgingSd_Reset();
    PollPresentation();
}

static void StopPage() {
    AgingTestStop();
    SettingsTestAgingSd_Reset();
}

static void OnDeleted(lv_event_t* event) {
    if (lv_event_get_target(event) != s_scr) {
        return;
    }
    s_page.Unbind();
    StopPage();
    s_scr = s_counter_lbl = nullptr;
}

static bool OnAgingKey(const char* key) {
    if (std::strcmp(key, "vk_home") != 0 && std::strcmp(key, "vk_prev") != 0) {
        return false;
    }
    RequestAgingExit();
    return true;
}

static bool OnAgingLongKey(const char* key) {
    if (std::strcmp(key, "vk_home") != 0) {
        return false;
    }
    RequestAgingExit(true);
    return true;
}

lv_obj_t* SettingsTestAgingScreen::CreateBootResume() {
    ScreenCommon_ClearBackStack();
    ScreenCommon_PushBackFactory(SettingsScreen::CreateTest);
    return Create();
}

lv_obj_t* SettingsTestAgingScreen::Create() {
    ESP_LOGI(TAG, "create aging test screen");

    ClearStatusBindings();
    s_exit_pending = s_exit_to_home = s_exit_navigation_queued = false;
    s_save_error_shown = false;
    s_shown_cycles = UINT32_MAX;
    const auto before = AgingTestRead();
    lv_obj_t* scr = lv_obj_create(nullptr);
    s_scr = scr;
    lv_obj_set_style_bg_color(scr, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, UiColorForeground(), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, OnDeleted, LV_EVENT_DELETE, nullptr);

    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, LangString_SETTINGS_TEST_AGING_RUNNING);
    lv_obj_set_style_text_font(title, AgingTitleFont(), 0);
    lv_obj_set_style_text_color(title, UiColorForeground(), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, kPad);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* count_title = lv_label_create(scr);
    lv_label_set_text_fmt(count_title, LangString_SETTINGS_TEST_AGING_RESTART_COUNT_FMT,
                          static_cast<int>(std::max<int32_t>(0, before.restart_count)));
    lv_obj_set_style_text_font(count_title, AgingTitleFont(), 0);
    lv_obj_set_style_text_color(count_title, UiColorForeground(), 0);
    lv_obj_align_to(count_title, title, LV_ALIGN_OUT_BOTTOM_MID, 0, kPad);

    s_counter_lbl = lv_label_create(scr);
    lv_obj_set_width(s_counter_lbl, LV_HOR_RES - 2 * kPad);
    lv_obj_set_height(s_counter_lbl, LV_SIZE_CONTENT);
    lv_label_set_long_mode(s_counter_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_counter_lbl, &font_misans_regular_160_2, 0);
    lv_obj_set_style_text_color(s_counter_lbl, UiColorForeground(), 0);
    lv_obj_set_style_text_align(s_counter_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(s_counter_lbl, count_title, LV_ALIGN_OUT_BOTTOM_MID, 0, kPad);
    lv_obj_clear_flag(s_counter_lbl, LV_OBJ_FLAG_CLICKABLE);
    UpdateCounterLabel(0);

    const lv_coord_t wallpaper_side = std::min<lv_coord_t>(LV_HOR_RES - 2 * UiSx(24), LV_VER_RES / 3);
    int preview_side = 0;
    lv_obj_t* wallpaper = SettingsTestAgingSd_Build(scr, wallpaper_side, preview_side);
    lv_obj_center(wallpaper);

    lv_obj_t* back = ScreenCreateSelectableOption(scr, LangString_SETTINGS_TEST_BACK,
                                                  [](lv_event_t*) { RequestAgingExit(); }, 0);
    lv_obj_set_size(back, UiSx(200), UiSy(60));
    const lv_coord_t wallpaper_bottom = (LV_VER_RES + wallpaper_side) / 2;
    lv_obj_align(back, LV_ALIGN_TOP_MID, 0, (wallpaper_bottom + LV_VER_RES - UiSy(60)) / 2);

    ScreenSetIsHome(false);
    VkKey_AttachScreen(scr, kScreenId,
                       VkKeyScreenDesc{.factory = SettingsTestAgingScreen::Create,
                                       .on_key = OnAgingKey,
                                       .on_key_long_press = OnAgingLongKey});

    s_page.Bind(scr, {PollPresentation, nullptr, StopPage, 100});
    if (!AgingTestStart(preview_side)) {
        ScreenShowToast(LangString_SETTINGS_TEST_AGING_SD_TASK);
    }
    return scr;
}
