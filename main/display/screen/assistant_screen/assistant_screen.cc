// assistant_screen.cc — Create / class API / VK / lifecycle
#include "assistant_screen.h"
#include "assistant_vk.h"
#include "assistant_ingest.h"
#include "assistant_flow.h"
#include "assistant_render.h"
#include "assistant_paginate.h"
#include "assistant_ptt.h"
#include "assistant_fonts.h"
#include "assistant_screen_priv.h"

#include "a2ui.h"
#include "a2ui_image.h"
#include "application.h"
#include "firmware_memory.h"
#include "assistant_boot_photo.h"
#include "assets/lang_screen.h"
#include "fontpack_lvgl.h"
#include "chat_cloud.h"
#include "board.h"
#include "boot_key_handler.h"
#include "display.h"
#include "haptic_feedback.h"
#include "lv_adapter_display.h"
#include "screen_common.h"
#include "task_screen/task_screen.h"
#include "vk_key_handler.h"
#include "vk_page_repeat.h"

#include <cstring>
#include <optional>
#include <new>

#include <esp_log.h>
#include "lv_adapter_epdiy.h"
#include <esp_timer.h>

namespace {
std::optional<PowerNeedHold> s_network_hold; // 仅由 LVGL 生命周期持有，独立于语音通道
std::atomic<bool> s_standby{false}; // 阻止待机期间的迟到语音启动和 UI 更新
}

AssistantUiState& Assistant_State() {
    static AssistantUiState s;
    return s;
}

bool Assistant_OnVkKey(const char* key) {
    if (key == nullptr) {
        return false;
    }
    if (AssistantPhoto_ClosePreview()) return true;

    // vk_home：直接出栈（阅读进百问回阅读）；不翻页、不清栈回首页
    if (std::strcmp(key, "vk_home") == 0) {
        ESP_LOGD(TAG, "vk_home -> navigate back");
        // 导航可能等待或取消；页面资源统一在实际卸载时清理。
        VkPageRepeatStop();
        ScreenNavigateBack();
        return true;
    }

    if (std::strcmp(key, "vk_prev") == 0) {
        if (Assistant_TurnPage(-1)) {
            return true;
        }
        ESP_LOGD(TAG, "vk_prev on first page -> navigate back");
        return false; // 默认策略 ScreenNavigateBack
    }

    if (!Assistant_State().layout.has_a2ui_content || Assistant_State().layout.pages.empty()) {
        return false;
    }
    if (std::strcmp(key, "vk_next") == 0) {
        Assistant_TurnPage(1);
        return true;
    }
    return false;
}

void Assistant_OnAssistantLifecycle(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    auto& app = Application::GetInstance();
    if (code == LV_EVENT_SCREEN_LOADED) {
        Assistant_State().chrome.scr = static_cast<lv_obj_t*>(lv_event_get_target(e));
        Assistant_State().active.store(true, std::memory_order_release);
        s_standby.store(false);
        if (!s_network_hold) s_network_hold.emplace(PowerNeed::ForegroundNetwork);
        const bool hold_through = BootKey_IsHeld();
        if (auto* disp = LVAdapterDisplay::Instance()) {
            disp->SetStatusTitlePrefix(nullptr);
            // 顶栏：联网提示走 SetStatus；聆听/说话文案过滤；Idle 后刷时钟
            disp->SetIdleStatusMode(IdleStatusMode::kClock);
            disp->UpdateStatusBar(true);
        }
        if (Assistant_State().chrome.content != nullptr && lv_obj_is_valid(Assistant_State().chrome.content)) {
            a2ui_set_host(Assistant_State().chrome.content);
        }
        if (const char* hint = Board::GetInstance().GetNetworkErrorText(nullptr)) {
            ScreenShowToast(hint);
            Assistant_ApplyPttWave(false);
            return;
        }
        app.Schedule([]() {
            if (AssistantScreen::IsActive()) TaskScreen::RequestCacheRefresh();
        });
        app.StartXiaozhiVoice(hold_through);
        Assistant_ApplyPttWave(Assistant_IsPttWaveWanted());
    } else if (code == LV_EVENT_SCREEN_UNLOADED) {
        ESP_LOGD(TAG, "leave -> schedule voice session stop (keep session RAM)");
        Assistant_State().active.store(false, std::memory_order_release);
        s_standby.store(false);
        s_network_hold.reset();
        if (Assistant_State().stream.coalesce_timer != nullptr) {
            esp_timer_stop(Assistant_State().stream.coalesce_timer);
        }
        Assistant_State().stream.coalesce_pending = false;
        Assistant_State().stream.coalesce_buf.clear();
        VkPageRepeatStop();
        Assistant_StopPttWaveAnim();
        ScreenPaintCoalesceReset(&Assistant_State().stream.page_paint);
        a2ui_set_host(nullptr);
        // 保留 Assistant_State().layout.flow/Assistant_State().layout.pages；须先毁引用像素的控件，再 a2ui_image_clear（与 Assistant_ClearSession 同序）
        a2ui_image_abort_loads();
        if (Assistant_State().chrome.content != nullptr && lv_obj_is_valid(Assistant_State().chrome.content)) {
            lv_obj_clean(Assistant_State().chrome.content);
        }
        a2ui_image_clear();
        Assistant_ResetTouchPttState();
        AssistantPhoto_Reset();
        Assistant_State().chrome.scr = nullptr;
        Assistant_State().chrome.content = nullptr;
        Assistant_State().chrome.page_label = nullptr;
        Assistant_State().chrome.photo_action = nullptr;
        Assistant_State().chrome.photo_action_label = nullptr;
        Assistant_State().chrome.idle_hint = nullptr;
        Assistant_State().chrome.status_label = nullptr;
        Assistant_State().chrome.notification_label = nullptr;
        Assistant_State().ptt.ptt_wave = nullptr;
        Assistant_State().ptt.touch_ptt_hit = nullptr;
        Assistant_State().ptt.touch_ptt_label = nullptr;
        Assistant_State().ptt.ptt_wave_visible = false;
        for (int i = 0; i < kPttWaveBars; ++i) {
            Assistant_State().ptt.ptt_wave_bars[i] = nullptr;
        }
        Assistant_State().layout.ui_font = nullptr;
        Assistant_State().layout.ui_font_bold = nullptr;
        Assistant_State().layout.viewport_w = 0;
        Assistant_State().layout.viewport_h = 0;
        if (auto* disp = LVAdapterDisplay::Instance()) {
            disp->SetStatusTitlePrefix(nullptr);
            disp->SetIdleStatusMode(IdleStatusMode::kClock);
        }
        app.StopXiaozhiVoice();
    }
}

void Assistant_OpenAssistantAsync(void* /*arg*/) {
    if (AssistantScreen::IsActive()) {
        ESP_LOGD(TAG, "RequestOpen no-op: already active");
        return;
    }
    HapticPulseIfEnabled();
    ESP_LOGD(TAG, "RequestOpen -> ScreenNavigateTo");
    ScreenNavigateTo(AssistantScreen::Create);
}

bool Assistant_OnBootPressDown() {
    // 开听只走 LongPress / hold-through；按下不听，避免短按先闪「聆听中」
    ESP_LOGD(TAG, "boot press-down ignored for listen (click/long own the semantics)");
    return true;
}

bool Assistant_OnBootPressUp() {
    // BootKey 已清 IsHeld；屏触仍按住时不能停听，否则多源 PTT 会互相打断
    Assistant_StopListeningIfNoPttHeld("boot press-up");
    return true;
}

bool Assistant_OnBootLongPress() {
    // 仅开听；打断说话由 StartListening(Speaking) 内部处理，不走 AbortSpeakingToIdle
    ESP_LOGD(TAG, "boot long-press (500ms) -> StartListening (PTT arm, not click-Assistant_abort)");
    ScreenLvAsync([](void*) {
        if (!AssistantScreen::IsActive() || !BootKey_IsHeld()) return;
        if (!Assistant_AllowListening()) return;
        Assistant_RequestSyncPttOverlay();
        Application::GetInstance().StartListening();
    });
    return true;
}

bool Assistant_OnBootClick() {
    // 仅短按打断 → 待命；进待机改走电源键。长按回合已在 BootKey 层屏蔽。
    if (BootKey_DidLongPress()) {
        ESP_LOGD(TAG, "boot short ignored: long-press already armed listen");
        return true;
    }
    ESP_LOGD(TAG, "boot short -> AbortSpeakingToIdle (no listen)");
    Application::GetInstance().AbortSpeakingToIdle();
    return true;
}

bool Assistant_OnBootDoubleClick() {
    return AssistantBootPhoto_OnDoubleClick();
}

bool Assistant_OnVkKeyLongPress(const char* key) {
    if (key == nullptr) {
        return false;
    }
    // vk_home 长按不消费 → VkKey 默认一键回系统首页（不再作 PTT）
    if (!AssistantScreen::IsActive()) {
        return false;
    }
    if (AssistantPhoto_ClosePreview()) return true;
    if (VkPageRepeatTryStart(key, Assistant_PageRepeatStep)) {
        return true;
    }
    return false;
}

bool Assistant_OnVkKeyPressUp(const char* key) {
    if (key == nullptr) {
        return false;
    }
    return VkPageRepeatOnPressUp(key);
}

bool AssistantScreen::IsActive() {
    // 主事件循环 / SetStatus 会跨线程调用；勿 lv_obj_is_valid / lv_screen_active
    return Assistant_State().active.load(std::memory_order_acquire) && !s_standby.load();
}

void AssistantScreen::SetStandby(bool standby) {
    if ((!Assistant_State().active.load() && !s_network_hold) || s_standby.exchange(standby) == standby) return;
    if (standby) {
        Assistant_ResetTouchPttState();
        AssistantPhoto_Suspend();
        Assistant_ApplyPttWave(false);
        s_network_hold.reset();
        Application::GetInstance().StopXiaozhiVoice();
    } else {
        s_network_hold.emplace(PowerNeed::ForegroundNetwork);
        Application::GetInstance().StartXiaozhiVoice(false);
    }
}

bool AssistantScreen::IsPttHeld() {
    return Assistant_IsAnyPttHeld();
}

bool AssistantScreen::IsPttWaveVisible() {
    return Assistant_State().ptt.ptt_wave_visible;
}

void AssistantScreen::SyncPttOverlay() {
    Assistant_RequestSyncPttOverlay();
}

void AssistantScreen::RequestOpen() {
    auto* adapter = LvAdapterEpdiy::Instance();
    if (adapter == nullptr || !adapter->IsReady()) {
        return;
    }
    if (!adapter->LockUi(-1)) {
        ESP_LOGW(TAG, "RequestOpen: adapter lock failed");
        return;
    }
    if (lv_async_call(Assistant_OpenAssistantAsync, nullptr) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "RequestOpen: lv_async_call failed");
    }
    adapter->UnlockUi();
}

void AssistantScreen::SetEmotion(const char* emotion) {
    if (Assistant_State().chrome.idle_hint == nullptr || !lv_obj_is_valid(Assistant_State().chrome.idle_hint)) {
        return;
    }
    if (Assistant_State().layout.has_a2ui_content || !Assistant_State().layout.flow.empty()) {
        Assistant_HideIdleHint();
        return;
    }
    (void)emotion;
    Assistant_ShowIdleHint();
}

lv_obj_t* AssistantScreen::Create() {
    ESP_LOGI(TAG, "launch AssistantScreen");
    const lv_font_t* ui_font = Assistant_UiFont();
    Assistant_UiFontBold();

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, ui_font, 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    Assistant_DisableScroll(scr);

    // 顶栏与其它 App 共用 ScreenCreateStatusBar（25@2），勿用会话 30px 字
    EpdStatusBar status = ScreenCreateStatusBar(scr);
    const lv_coord_t header_h = status.height;
    const lv_coord_t body_h = LV_VER_RES - header_h - kFooterH - kPageIndicatorH;
    Assistant_State().layout.viewport_w = LV_HOR_RES - kContentPadX * 2;
    Assistant_State().layout.viewport_h = body_h - kContentPadY * 2;
    if (Assistant_State().layout.viewport_h < Assistant_FontLineHeight(ui_font)) {
        Assistant_State().layout.viewport_h = Assistant_FontLineHeight(ui_font);
    }

    lv_obj_t* container = lv_obj_create(scr);
    lv_obj_set_size(container, LV_HOR_RES, LV_VER_RES - header_h);
    lv_obj_align(container, LV_ALIGN_TOP_MID, 0, header_h);
    lv_obj_set_style_radius(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_bg_color(container, lv_color_white(), 0);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_CLICKABLE);
    Assistant_DisableScroll(container);

    lv_obj_t* content = lv_obj_create(container);
    lv_obj_set_width(content, LV_HOR_RES);
    lv_obj_set_height(content, body_h);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_pad_hor(content, kContentPadX, 0);
    lv_obj_set_style_pad_ver(content, kContentPadY, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_bg_color(content, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_CLICKABLE);
    Assistant_DisableScroll(content);

    // 空会话页由 LVGL 控件绘制，状态栏保持在最上层。
    lv_obj_t* idle_hint = lv_obj_create(scr);
    lv_obj_remove_style_all(idle_hint);
    lv_obj_set_size(idle_hint, LV_HOR_RES, LV_VER_RES);
    lv_obj_add_flag(idle_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(idle_hint, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    Assistant_BuildIdleHint(idle_hint);
    lv_obj_move_foreground(status.bar);
    lv_obj_move_foreground(status.overlay);

    // 录音波形叠在状态栏条带居中：高度约半栏偏矮、竖条更密；不改状态栏尺寸；按住说话时盖住时钟
    Assistant_State().ptt.ptt_wave_max_h = header_h / 2;
    if (Assistant_State().ptt.ptt_wave_max_h < kPttWaveMinBarH) {
        Assistant_State().ptt.ptt_wave_max_h = kPttWaveMinBarH;
    }
    lv_obj_t* ptt_wave = Assistant_CreatePttWaveHost(scr, Assistant_State().ptt.ptt_wave_max_h);
    lv_obj_align(ptt_wave, LV_ALIGN_TOP_MID, 0, (header_h - Assistant_State().ptt.ptt_wave_max_h) / 2);
    lv_obj_add_flag(ptt_wave, LV_OBJ_FLAG_HIDDEN);

    // 正文透明层只翻页，覆盖欢迎页但不覆盖顶栏或底栏。
    auto* page_hit = lv_obj_create(scr);
    lv_obj_remove_style_all(page_hit);
    lv_obj_set_size(page_hit, LV_HOR_RES, body_h);
    lv_obj_set_pos(page_hit, 0, header_h);
    lv_obj_add_flag(page_hit, LV_OBJ_FLAG_CLICKABLE);
    Assistant_DisableScroll(page_hit);
    HapticAttachClick(page_hit);
    lv_obj_add_event_cb(page_hit, OnTouchPageClicked, LV_EVENT_CLICKED, nullptr);

    auto* bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_HOR_RES - kFooterInset * 2, kFooterH - UiSy(8));
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -UiSy(4));
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 2, 0);
    Assistant_DisableScroll(bar);
    const bool baiwen = chat_cloud::ActiveProvider() == chat_cloud::Provider::Baiwen;
    const bool camera_available = baiwen && Board::GetInstance().GetCamera() != nullptr;
    // 百问固定预留拍照入口，检测完成只显示图标，不移动录音区域。
    const lv_coord_t camera_w = baiwen ? kFooterActionW : 0;
    lv_obj_t* touch_ptt_hit = Assistant_CreateTouchPttHitLayer(bar);
    lv_obj_set_pos(touch_ptt_hit, camera_w + UiSx(4), 0);
    lv_obj_set_size(touch_ptt_hit, LV_HOR_RES - kFooterInset * 2 - camera_w - kFooterActionW - UiSx(8), kFooterH - UiSy(8));
    lv_obj_set_style_text_font(touch_ptt_hit, fontpack_lv_font_ui(), 0);
    if (baiwen) {
        auto* camera = lv_button_create(bar);
        lv_obj_remove_style_all(camera);
        lv_obj_add_flag(camera, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(camera, kFooterActionW, kFooterH - UiSy(8));
        lv_obj_set_pos(camera, UiSx(4), 0);
        if (!camera_available) lv_obj_add_flag(camera, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(camera, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(camera, 0, 0);
        lv_obj_set_style_text_color(camera, lv_color_black(), 0);
        Assistant_DisableScroll(camera);
        lv_obj_add_event_cb(camera, [](lv_event_t* event) {
            lv_area_t bounds;
            lv_obj_get_coords(static_cast<lv_obj_t*>(lv_event_get_target(event)), &bounds);
            const int cx = (bounds.x1 + bounds.x2) / 2;
            const int cy = (bounds.y1 + bounds.y2) / 2;
            lv_draw_rect_dsc_t box;
            lv_draw_rect_dsc_init(&box);
            box.bg_opa = LV_OPA_TRANSP;
            box.border_color = lv_color_black();
            box.border_width = 2;
            box.radius = UiSx(4);
            lv_area_t area = {cx - UiSx(18), cy - UiSy(12), cx + UiSx(18), cy + UiSy(12)};
            lv_draw_rect(lv_event_get_layer(event), &box, &area);
            lv_draw_arc_dsc_t lens;
            lv_draw_arc_dsc_init(&lens);
            lens.color = lv_color_black();
            lens.width = 2;
            lens.center = {cx, cy};
            lens.radius = UiSx(7);
            lens.start_angle = 0;
            lens.end_angle = 360;
            lv_draw_arc(lv_event_get_layer(event), &lens);
            area = {cx - UiSx(8), cy - UiSy(16), cx + UiSx(8), cy - UiSy(12)};
            box.bg_opa = LV_OPA_COVER;
            box.bg_color = lv_color_black();
            lv_draw_rect(lv_event_get_layer(event), &box, &area);
        }, LV_EVENT_DRAW_MAIN, nullptr);
        HapticAttachClick(camera);
        lv_obj_add_event_cb(camera, [](lv_event_t*) { AssistantBootPhoto_OnDoubleClick(); }, LV_EVENT_CLICKED, nullptr);
    }
    auto* action = lv_obj_create(bar);
    lv_obj_remove_style_all(action);
    lv_obj_set_size(action, kFooterActionW, kFooterH - UiSy(16));
    lv_obj_align(action, LV_ALIGN_RIGHT_MID, -UiSx(4), 0);
    lv_obj_set_style_radius(action, LV_RADIUS_CIRCLE, 0);
    Assistant_DisableScroll(action);
    lv_obj_clear_flag(action, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(action);
    lv_obj_add_event_cb(action, [](lv_event_t* event) {
        if (AssistantPhoto_HasPending()) return;
        lv_area_t area;
        lv_obj_get_coords(static_cast<lv_obj_t*>(lv_event_get_target(event)), &area);
        const int cx = (area.x1 + area.x2) / 2;
        const int cy = (area.y1 + area.y2) / 2;
        lv_draw_rect_dsc_t ink;
        lv_draw_rect_dsc_init(&ink);
        ink.bg_color = lv_color_black();
        ink.bg_opa = LV_OPA_COVER;
        ink.radius = LV_RADIUS_CIRCLE;
        const int toes[][4] = {{-17,-9,7,9},{-7,-16,8,10},{5,-16,8,10},{15,-8,7,9}};
        for (const auto& toe : toes) {
            const int x = cx + UiSx(toe[0]);
            const int y = cy + UiSy(toe[1]);
            lv_area_t dot = {x - UiSx(toe[2]) / 2, y - UiSy(toe[3]) / 2,
                             x + UiSx(toe[2]) / 2, y + UiSy(toe[3]) / 2};
            lv_draw_rect(lv_event_get_layer(event), &ink, &dot);
        }
        lv_area_t pad = {cx - UiSx(12), cy, cx + UiSx(12), cy + UiSy(17)};
        ink.radius = UiSx(10);
        lv_draw_rect(lv_event_get_layer(event), &ink, &pad);
    }, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(action, [](lv_event_t*) { AssistantPhoto_Send(); }, LV_EVENT_CLICKED, nullptr);
    auto* action_label = lv_label_create(action);
    lv_label_set_text(action_label, Lang::Strings::ASSISTANT_SEND);
    lv_obj_set_style_text_font(action_label, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(action_label, lv_color_white(), 0);
    lv_obj_center(action_label);
    lv_obj_add_flag(action_label, LV_OBJ_FLAG_HIDDEN);
    auto* footer = lv_label_create(scr);
    lv_obj_set_size(footer, LV_HOR_RES, kPageIndicatorH);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -kFooterH);
    lv_obj_set_style_text_align(footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(footer, fontpack_lv_font_ui(), 0);
    lv_label_set_text(footer, "");
    lv_obj_add_flag(footer, LV_OBJ_FLAG_HIDDEN);

    Assistant_State().chrome.scr = scr;
    Assistant_State().chrome.content = content;
    Assistant_State().chrome.page_label = footer;
    Assistant_State().chrome.photo_action = action;
    Assistant_State().chrome.photo_action_label = action_label;
    Assistant_State().chrome.idle_hint = idle_hint;
    Assistant_State().chrome.status_label = status.status_label;
    Assistant_State().chrome.notification_label = status.notification_label;
    Assistant_State().ptt.ptt_wave = ptt_wave;
    Assistant_State().ptt.touch_ptt_hit = touch_ptt_hit;
    Assistant_State().ptt.ptt_wave_visible = false;
    if (camera_available) AssistantPhoto_Create(scr);
    Assistant_ResetTouchPttState();
    ScreenPaintCoalesceReset(&Assistant_State().stream.page_paint);
    Assistant_State().stream.rendered_page_json.clear();
    const bool restore_session = !Assistant_State().layout.flow.empty();
    if (!restore_session) {
        Assistant_State().layout.pages.clear();
        Assistant_State().layout.flow_first_page.clear();
        Assistant_State().layout.page_flow_begin.clear();
        Assistant_State().layout.open_page_flow_begin = static_cast<size_t>(-1);
        Assistant_State().layout.page_index = 0;
        Assistant_State().layout.pin_flow_idx = -1;
        Assistant_State().layout.has_a2ui_content = false;
    }

    Assistant_EnsureA2ui(scr);
    a2ui_set_host(content);
    if (restore_session) {
        ESP_LOGD(TAG, "restore session flow=%u pages=%u idx=%d",
                 static_cast<unsigned>(Assistant_State().layout.flow.size()), static_cast<unsigned>(Assistant_State().layout.pages.size()),
                 Assistant_State().layout.page_index);
        Assistant_RebuildPages();
        Assistant_TrimFlowToMaxPages();
        if (Assistant_State().layout.page_index >= static_cast<int>(Assistant_State().layout.pages.size())) {
            Assistant_State().layout.page_index = static_cast<int>(Assistant_State().layout.pages.size()) - 1;
        }
        if (Assistant_State().layout.page_index < 0) {
            Assistant_State().layout.page_index = 0;
        }
        if (!Assistant_State().layout.pages.empty()) {
            Assistant_State().layout.has_a2ui_content = true;
            Assistant_HideIdleHint();
            Assistant_RenderCurrentPage();
        } else {
            Assistant_ShowIdleHint();
        }
    } else {
        Assistant_ReplayHistoryFromStore();
        if (Assistant_State().layout.has_a2ui_content || !Assistant_State().layout.flow.empty()) {
            Assistant_HideIdleHint();
        } else {
            Assistant_ShowIdleHint();
        }
    }
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->SetStatusTitlePrefix(nullptr);
        disp->SetIdleStatusMode(IdleStatusMode::kClock);
        disp->UpdateStatusBar(true);
    }

    ScreenSetIsHome(false);
    {
        VkKeyScreenDesc desc{};
        desc.factory = AssistantScreen::Create;
        desc.on_key = Assistant_OnVkKey;
        desc.on_boot_click = Assistant_OnBootClick;
        desc.on_boot_long_press = Assistant_OnBootLongPress;
        desc.on_boot_press_down = Assistant_OnBootPressDown;
        desc.on_boot_press_up = Assistant_OnBootPressUp;
        desc.on_key_long_press = Assistant_OnVkKeyLongPress;
        desc.on_key_press_up = Assistant_OnVkKeyPressUp;
        desc.on_boot_double_click = Assistant_OnBootDoubleClick;
        VkKey_AttachScreen(scr, "assistant", desc);
    }
    lv_obj_add_event_cb(scr, Assistant_OnAssistantLifecycle, LV_EVENT_SCREEN_LOADED, nullptr);
    lv_obj_add_event_cb(scr, Assistant_OnAssistantLifecycle, LV_EVENT_SCREEN_UNLOADED, nullptr);
    FirmwareMemoryCheckpoint("assistant-screen-ready");
    return scr;
}

void Assistant_UpdatePhotoAction() {
    auto& state = Assistant_State();
    if (!state.chrome.photo_action) return;
    const bool pending = AssistantPhoto_HasPending();
    lv_obj_set_style_bg_color(state.chrome.photo_action, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(state.chrome.photo_action, pending ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    if (pending) {
        lv_obj_add_flag(state.chrome.photo_action, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(state.chrome.photo_action_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(state.chrome.photo_action, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(state.chrome.photo_action_label, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_invalidate(state.chrome.photo_action);
}

bool Assistant_AllowListening() {
    if (const char* hint = Board::GetInstance().GetNetworkErrorText(nullptr)) {
        ScreenShowToast(hint);
        return false;
    }
    return AssistantPhoto_AllowListening();
}

bool AssistantScreen::BeginPhotoListening(uint32_t turn) {
    return ScreenLvAsync([](void* value) {
        if (AssistantScreen::IsActive()) AssistantPhoto_BeginListening(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(value)));
    }, reinterpret_cast<void*>(static_cast<uintptr_t>(turn)));
}

bool AssistantScreen::EndPhotoListening(uint32_t turn) {
    return ScreenLvAsync([](void* value) {
        if (AssistantScreen::IsActive()) AssistantPhoto_EndListening(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(value)));
    }, reinterpret_cast<void*>(static_cast<uintptr_t>(turn)));
}

bool AssistantScreen::UploadPhotoQuestion(uint32_t turn, bool ready) {
    struct Result {
        uint32_t turn; // 提问轮次。
        bool ready; // WebSocket 会话是否就绪。
    };
    auto* result = new (std::nothrow) Result{turn, ready};
    if (!result) {
        ESP_LOGE(TAG, "photo readiness failed stage=allocate");
        return false;
    }
    const auto apply = [](void* context) {
        std::unique_ptr<Result> result(static_cast<Result*>(context));
        AssistantPhoto_UploadQuestion(result->turn, result->ready);
    };
    if (ScreenLvAsync(apply, result) || ScreenLvAsyncUrgent(apply, result)) return true;
    delete result;
    ESP_LOGE(TAG, "photo readiness failed stage=dispatch");
    return false;
}

void AssistantScreen::PhotoQuestionError(const std::string& image_id, bool reupload, bool disabled) {
    struct Error {
        std::string image_id; // 失败问题关联的图片编号，仅用于匹配，不写入日志。
        bool reupload; // 是否需要重新上传原图。
        bool disabled; // 服务端是否未开启识图。
    };
    Error* error = nullptr;
    try { error = new Error{image_id, reupload, disabled}; }
    catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "photo error delivery failed stage=allocate");
        return;
    }
    const auto apply = [](void* context) {
        std::unique_ptr<Error> error(static_cast<Error*>(context));
        if (AssistantScreen::IsActive()) AssistantPhoto_QuestionError(error->image_id, error->reupload, error->disabled);
    };
    if (!ScreenLvAsync(apply, error) && !ScreenLvAsyncUrgent(apply, error)) {
        delete error;
        ESP_LOGE(TAG, "photo error delivery failed stage=dispatch");
    }
}

void AssistantScreen::FinishPhotoQuestion() {
    const auto apply = [](void*) { AssistantPhoto_FinishQuestion(); };
    if (!ScreenLvAsync(apply) && !ScreenLvAsyncUrgent(apply)) {
        ESP_LOGE(TAG, "photo release failed stage=dispatch");
    }
}
