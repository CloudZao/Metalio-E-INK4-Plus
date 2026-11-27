#include <cstdio>
#include <cstring>
#include <utility>
#include <lvgl.h>
#include "ui/apps/settings/settings_conversation_tab.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "business/conversation/chat_cloud.h"
#include "business/conversation/conversation_settings.h"
#include "ui/apps/settings/settings_common.h"
#include "ui/common/runtime/page_lifecycle.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/drawing/ui_theme.h"

struct SettingsConversationUi {
    lv_obj_t* toggle = nullptr;
    lv_obj_t* status_lbl = nullptr;
    bool alive = false;
    bool known = false;
    bool enabled = true;
};

static SettingsConversationUi s_ui;
static UiPageLifecycle s_page;
static ConversationSettingsRequest s_request;

static bool TtsBusy() {
    return s_request && !s_request->completed.load(std::memory_order_acquire);
}

static void RefreshOptions();

static const char* CloudText(const char* zh, const char* en) {
    return std::strcmp(LangCode, "zh-CN") == 0 ? zh : en;
}

static void SetStatus(const char* text) {
    if (s_ui.status_lbl != nullptr) {
        lv_label_set_text(s_ui.status_lbl, text != nullptr ? text : "");
        if (text != nullptr && text[0] != '\0') {
            lv_obj_clear_flag(s_ui.status_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui.status_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void RefreshOptions() {
    if (s_ui.toggle) {
        lv_obj_set_state(s_ui.toggle, LV_STATE_CHECKED, s_ui.known && s_ui.enabled);
        lv_obj_set_state(s_ui.toggle, LV_STATE_DISABLED, TtsBusy() || !s_ui.known);
    }
}

static void PollConversationSettings() {
    if (!s_request || !s_request->completed.load(std::memory_order_acquire)) {
        return;
    }
    auto result = std::move(s_request);
    if (result->ok) {
        s_ui.known = true;
        s_ui.enabled = result->enabled;
        SetStatus("");
    } else {
        SetStatus(result->error[0] ? result->error : LangString_SETTINGS_CONV_REQUEST_FAIL);
    }
    RefreshOptions();
}

static void OnOptionClicked(lv_event_t* event) {
    if (TtsBusy()) {
        return;
    }
    const bool enabled = lv_obj_has_state(lv_event_get_current_target_obj(event), LV_STATE_CHECKED);
    if (s_ui.known && s_ui.enabled == enabled) {
        return;
    }
    s_request = ConversationSettingsSaveTts(enabled);
    SetStatus(s_request ? (enabled ? LangString_SETTINGS_CONV_ENABLING : LangString_SETTINGS_CONV_DISABLING)
                        : LangString_SETTINGS_CONV_BUSY);
    RefreshOptions();
}

void SettingsConversationTab_Build(lv_obj_t* page) {
    s_ui = {};
    s_ui.alive = true;
    s_page.Bind(page, {PollConversationSettings, nullptr, SettingsConversationTab_Reset, 100}, true);

    if (ChatCloudUsesXiaozhiProtocol()) {
        s_ui.known = true;
        s_ui.enabled = ChatCloudLocalTtsEnabled();
    }

    auto* section = SettingsCreateSection(page, CloudText("语音设置", "Speech settings"));
    auto* speech = SettingsCreateListGroup(section);
    s_ui.toggle = SettingsCreateToggle(speech, LangString_SETTINGS_CONV_TITLE, OnOptionClicked);

    s_ui.status_lbl = lv_label_create(section);
    lv_label_set_text(s_ui.status_lbl, "");
    lv_obj_add_flag(s_ui.status_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(s_ui.status_lbl, lv_pct(100));
    lv_label_set_long_mode(s_ui.status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.status_lbl, UiFont(UiFontRole::Caption), 0);
    lv_obj_set_style_text_color(s_ui.status_lbl, UiColorForeground(), 0);
    lv_obj_clear_flag(s_ui.status_lbl, LV_OBJ_FLAG_CLICKABLE);

    const int provider = static_cast<int>(ChatCloudActiveProvider());
    auto* cloud = SettingsCreateSection(page, CloudText("云服务", "Cloud service"));
    auto* group = SettingsCreateListGroup(cloud);
    const char* provider_name = provider == 0 ? CloudText("百问AI", "Baiwen AI") :
        provider == 1 ? CloudText("小智AI", "Xiaozhi AI") : CloudText("网易AI", "NetEase AI");
    auto* current = SettingsCreateListRow(group, provider_name, "", nullptr, 0, false);
    SettingsSetListRowCurrent(current, true);

    RefreshOptions();
}

void SettingsConversationTab_Reset() {
    s_page.Unbind();
    s_request.reset();
    s_ui = {};
}

void SettingsConversationTab_OnActivated() {
    if (!s_ui.alive) {
        return;
    }
    s_request = ConversationSettingsCurrent();
    if (!s_request) {
        s_request = ConversationSettingsQueryTts();
    }
    SetStatus(s_request ? LangString_SETTINGS_CONV_CONNECTING : LangString_SETTINGS_CONV_BUSY);
    RefreshOptions();
}
