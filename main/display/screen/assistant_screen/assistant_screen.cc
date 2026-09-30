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
#include "assistant_boot_photo.h"
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

#include <esp_log.h>
#include <esp_lv_adapter.h>
#include <esp_timer.h>

AssistantUiState& Assistant_State() {
    static AssistantUiState s;
    return s;
}

bool Assistant_OnVkKey(const char* key) {
    if (key == nullptr) {
        return false;
    }

    // vk_home：直接出栈（阅读进百问回阅读）；不翻页、不清栈回首页
    if (std::strcmp(key, "vk_home") == 0) {
        ESP_LOGI(TAG, "vk_home -> navigate back");
        // 先停收消息/合并绘制/连翻，避免卸页过程中再 Render 踩已毁控件
        Assistant_State().active.store(false, std::memory_order_release);
        if (Assistant_State().stream.coalesce_timer != nullptr) {
            esp_timer_stop(Assistant_State().stream.coalesce_timer);
        }
        Assistant_State().stream.coalesce_pending = false;
        Assistant_State().stream.coalesce_buf.clear();
        VkPageRepeatStop();
        ScreenPaintCoalesceReset(&Assistant_State().stream.page_paint);
        ScreenNavigateBack();
        return true;
    }

    if (std::strcmp(key, "vk_prev") == 0) {
        if (Assistant_TurnPage(-1)) {
            return true;
        }
        ESP_LOGI(TAG, "vk_prev on first page -> navigate back");
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
        if (hold_through) {
            ESP_LOGI(TAG, "enter hold-through -> schedule voice + listen (skip standby)");
            app.Schedule([]() {
                TaskScreen::RequestCacheRefresh();
                Application::GetInstance().StartXiaozhiVoice(true);
            });
        } else {
            ESP_LOGI(TAG, "enter -> schedule voice session prepare (PTT, no auto listen)");
            app.Schedule([]() {
                TaskScreen::RequestCacheRefresh();
                Application::GetInstance().StartXiaozhiVoice(false);
            });
        }
        Assistant_ApplyPttWave(Assistant_IsPttWaveWanted());
    } else if (code == LV_EVENT_SCREEN_UNLOADED) {
        ESP_LOGI(TAG, "leave -> schedule voice session stop (keep session RAM)");
        Assistant_State().active.store(false, std::memory_order_release);
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
        Assistant_State().chrome.scr = nullptr;
        Assistant_State().chrome.content = nullptr;
        Assistant_State().chrome.page_label = nullptr;
        Assistant_State().chrome.hint_img = nullptr;
        Assistant_State().chrome.hint_raster = nullptr;  // 像素由 hint_img DELETE 回调释放
        Assistant_State().chrome.status_label = nullptr;
        Assistant_State().chrome.notification_label = nullptr;
        Assistant_State().ptt.ptt_wave = nullptr;
        Assistant_State().ptt.touch_ptt_hit = nullptr;
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
        app.Schedule([]() {
            Application::GetInstance().StopXiaozhiVoice();
        });
    }
}

void Assistant_OpenAssistantAsync(void* /*arg*/) {
    if (AssistantScreen::IsActive()) {
        ESP_LOGI(TAG, "RequestOpen no-op: already active");
        return;
    }
    HapticPulseIfEnabled();
    ESP_LOGI(TAG, "RequestOpen -> ScreenNavigateTo");
    ScreenNavigateTo(AssistantScreen::Create);
}

bool Assistant_OnBootPressDown() {
    // 开听只走 LongPress / hold-through；按下不听，避免短按先闪「聆听中」
    ESP_LOGI(TAG, "boot press-down ignored for listen (click/long own the semantics)");
    return true;
}

bool Assistant_OnBootPressUp() {
    // BootKey 已清 IsHeld；屏触仍按住时不能停听，否则多源 PTT 会互相打断
    Assistant_StopListeningIfNoPttHeld("boot press-up");
    return true;
}

bool Assistant_OnBootLongPress() {
    // 仅开听；打断说话由 StartListening(Speaking) 内部处理，不走 AbortSpeakingToIdle
    ESP_LOGI(TAG, "boot long-press (500ms) -> StartListening (PTT arm, not click-Assistant_abort)");
    Assistant_RequestSyncPttOverlay();
    Application::GetInstance().StartListening();
    return true;
}

bool Assistant_OnBootClick() {
    // 仅短按打断 → 待命；进待机改走电源键。长按回合已在 BootKey 层屏蔽。
    if (BootKey_DidLongPress()) {
        ESP_LOGI(TAG, "boot short ignored: long-press already armed listen");
        return true;
    }
    ESP_LOGI(TAG, "boot short -> AbortSpeakingToIdle (no listen)");
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
    return Assistant_State().active.load(std::memory_order_acquire);
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
    if (!esp_lv_adapter_is_initialized()) {
        return;
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        ESP_LOGW(TAG, "RequestOpen: adapter lock failed");
        return;
    }
    if (lv_async_call(Assistant_OpenAssistantAsync, nullptr) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "RequestOpen: lv_async_call failed");
    }
    esp_lv_adapter_unlock();
}

void AssistantScreen::SetEmotion(const char* emotion) {
    if (Assistant_State().chrome.hint_img == nullptr || !lv_obj_is_valid(Assistant_State().chrome.hint_img)) {
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
    const lv_font_t* ui_font = Assistant_UiFont();
    Assistant_UiFontBold();

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, ui_font, 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    Assistant_DisableScroll(scr);

    // 顶栏与其它 App 共用 ScreenCreateStatusBar（UI 默认字），勿另绑会话字
    EpdStatusBar status = ScreenCreateStatusBar(scr);
    const lv_coord_t header_h = status.height;
    const lv_coord_t body_h = LV_VER_RES - header_h - kFooterH;
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

    lv_obj_t* footer = lv_label_create(container);
    lv_obj_set_size(footer, LV_HOR_RES, kFooterH);
    lv_obj_set_style_pad_all(footer, 0, 0);
    lv_obj_set_style_text_align(footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(footer, ui_font, 0);
    lv_obj_set_style_text_color(footer, lv_color_black(), 0);
    lv_label_set_text(footer, "");
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_CLICKABLE);
    Assistant_DisableScroll(footer);
    lv_obj_add_flag(footer, LV_OBJ_FLAG_HIDDEN);

    // 无会话全屏提示图：盖住内容区；顶栏透明，需再抬到图之上
    lv_obj_t* hint_img = lv_image_create(scr);
    lv_obj_add_flag(hint_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(hint_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        hint_img,
        [](lv_event_t* ev) {
            auto* p = static_cast<reader::RasterImage*>(
                lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_target(ev))));
            delete p;
            if (Assistant_State().chrome.hint_raster == p) {
                Assistant_State().chrome.hint_raster = nullptr;
            }
        },
        LV_EVENT_DELETE, nullptr);
    Assistant_LoadIdleHintImage(hint_img);
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

    // 最后创建：保证 hit 在内容之上吃屏触；非 clickable 子对象不抢命中
    lv_obj_t* touch_ptt_hit = Assistant_CreateTouchPttHitLayer(scr);

    Assistant_State().chrome.scr = scr;
    Assistant_State().chrome.content = content;
    Assistant_State().chrome.page_label = footer;
    Assistant_State().chrome.hint_img = hint_img;
    Assistant_State().chrome.status_label = status.status_label;
    Assistant_State().chrome.notification_label = status.notification_label;
    Assistant_State().ptt.ptt_wave = ptt_wave;
    Assistant_State().ptt.touch_ptt_hit = touch_ptt_hit;
    Assistant_State().ptt.ptt_wave_visible = false;
    Assistant_ResetTouchPttState();
    ScreenPaintCoalesceReset(&Assistant_State().stream.page_paint);
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
        ESP_LOGI(TAG, "restore session flow=%u pages=%u idx=%d",
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
    return scr;
}
