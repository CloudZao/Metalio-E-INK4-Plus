// assistant_flow.cc — split from assistant_screen.cc
#include "assistant_screen_priv.h"

#include "assistant_flow.h"
#include "a2ui.h"
#include "a2ui_image.h"
#include "application.h"
#include "assistant_chat_store.h"
#include "assistant_screen.h"
#include "board.h"
#include "display.h"
#include "screen_common.h"

#include <cstring>
#include <mutex>
#include <string>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>

void AdjustPinAfterDrop(size_t drop) {
    if (Assistant_State().layout.pin_flow_idx < 0) {
        return;
    }
    if (static_cast<size_t>(Assistant_State().layout.pin_flow_idx) < drop) {
        Assistant_State().layout.pin_flow_idx = 0;
    } else {
        Assistant_State().layout.pin_flow_idx -= static_cast<int>(drop);
    }
}

// 总页超过 kMaxPages 时从 flow 头丢掉旧内容，保留最近 kMaxPages 页。
bool Assistant_TrimFlowToMaxPages() {
    if (static_cast<int>(Assistant_State().layout.pages.size()) <= kMaxPages) {
        return false;
    }
    const int keep_from_page = static_cast<int>(Assistant_State().layout.pages.size()) - kMaxPages;
    size_t keep_from = Assistant_State().layout.flow.size();
    for (size_t i = 0; i < Assistant_State().layout.flow.size(); ++i) {
        const int fp = (i < Assistant_State().layout.flow_first_page.size()) ? Assistant_State().layout.flow_first_page[i] : -1;
        if (fp >= keep_from_page) {
            keep_from = i;
            break;
        }
    }
    if (keep_from == 0 || keep_from >= Assistant_State().layout.flow.size()) {
        return false;
    }
    Assistant_State().layout.flow.erase(Assistant_State().layout.flow.begin(),
                 Assistant_State().layout.flow.begin() + static_cast<std::ptrdiff_t>(keep_from));
    AdjustPinAfterDrop(keep_from);
    ESP_LOGW(TAG, "flow trimmed for max %d pages (drop %u items, remain %u)", kMaxPages,
             static_cast<unsigned>(keep_from), static_cast<unsigned>(Assistant_State().layout.flow.size()));
    Assistant_RebuildPages();
    return true;
}

void Assistant_ClearSession() {
    if (Assistant_State().stream.coalesce_timer != nullptr) {
        esp_timer_stop(Assistant_State().stream.coalesce_timer);
    }
    Assistant_State().stream.coalesce_buf.clear();
    Assistant_State().stream.coalesce_jump = false;
    Assistant_State().stream.coalesce_pending = false;
    {
        std::lock_guard<std::mutex> g(Assistant_State().stream.session_mu);
        Assistant_State().layout.flow.clear();
        Assistant_State().layout.pages.clear();
        Assistant_State().layout.flow_first_page.clear();
        Assistant_State().layout.page_flow_begin.clear();
        Assistant_State().layout.open_page_flow_begin = static_cast<size_t>(-1);
        Assistant_State().layout.page_index = 0;
        Assistant_State().layout.pin_flow_idx = -1;
        Assistant_State().layout.has_a2ui_content = false;
    }
    ScreenPaintCoalesceReset(&Assistant_State().stream.page_paint);
    // 与 a2ui updateComponents / deleteSurface 一致：Assistant_abort → 毁控件 → 释像素
    a2ui_image_abort_loads();
    if (Assistant_State().chrome.content != nullptr && lv_obj_is_valid(Assistant_State().chrome.content)) {
        lv_obj_clean(Assistant_State().chrome.content);
    }
    a2ui_image_clear();
    Assistant_UpdatePageIndicator();
}

void PresentAppendUi(bool jump_to_new, int reflow_from_page, bool show_content) {
    bool want_paint = false;
    {
        std::lock_guard<std::mutex> g(Assistant_State().stream.session_mu);
        want_paint = jump_to_new || Assistant_State().layout.page_index >= reflow_from_page;
        if (want_paint && !jump_to_new) {
            const int64_t now = esp_timer_get_time();
            if (now - Assistant_State().stream.last_stream_paint_us < kStreamPaintMinUs) {
                want_paint = false;
            } else {
                Assistant_State().stream.last_stream_paint_us = now;
            }
        } else if (want_paint) {
            Assistant_State().stream.last_stream_paint_us = esp_timer_get_time();
        }
    }

    Display* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    if (!AssistantScreen::IsActive() || Assistant_State().chrome.content == nullptr || !lv_obj_is_valid(Assistant_State().chrome.content)) {
        return;
    }
    if (!Assistant_State().layout.a2ui_ready) {
        Assistant_EnsureA2ui(Assistant_State().chrome.scr);
        if (!Assistant_State().layout.a2ui_ready) {
            return;
        }
    }
    a2ui_set_host(Assistant_State().chrome.content);
    if (show_content) {
        Assistant_HideIdleHint();
    }
    if (want_paint) {
        Assistant_RequestRenderCurrentPage();
    } else {
        Assistant_UpdatePageIndicator();
    }
}

void Assistant_AppendFlowAndShow(FlowList&& chunk, bool jump_to_new) {
    if (chunk.empty()) {
        return;
    }
    int reflow_from_page = 0;
    {
        std::lock_guard<std::mutex> g(Assistant_State().stream.session_mu);
        size_t chunk_at = Assistant_State().layout.flow.size();
        if (!Assistant_State().layout.flow.empty()) {
            FlowItem gap;
            gap.kind = FlowKind::MsgGap;
            Assistant_State().layout.flow.push_back(std::move(gap));
            chunk_at = Assistant_State().layout.flow.size();
        }
        for (auto& it : chunk) {
            Assistant_State().layout.flow.push_back(std::move(it));
        }
        if (jump_to_new) {
            if (Assistant_State().layout.replaying_history) {
                Assistant_State().layout.pin_flow_idx = static_cast<int>(Assistant_State().layout.flow.size()) - 1;
                while (Assistant_State().layout.pin_flow_idx >= 0 &&
                       Assistant_State().layout.flow[static_cast<size_t>(Assistant_State().layout.pin_flow_idx)].kind == FlowKind::MsgGap) {
                    --Assistant_State().layout.pin_flow_idx;
                }
            } else {
                Assistant_State().layout.pin_flow_idx = static_cast<int>(chunk_at);
            }
        }
        Assistant_State().layout.has_a2ui_content = true;
        if (!Assistant_State().layout.pages.empty() && Assistant_State().layout.page_flow_begin.size() == Assistant_State().layout.pages.size()) {
            while (Assistant_State().layout.flow_first_page.size() < Assistant_State().layout.flow.size()) {
                Assistant_State().layout.flow_first_page.push_back(-1);
            }
            reflow_from_page = Assistant_RebuildPagesTail();
        } else {
            Assistant_RebuildPages();
        }
        if (Assistant_TrimFlowToMaxPages()) {
            reflow_from_page = 0;
        }
        if (jump_to_new && Assistant_State().layout.pin_flow_idx >= 0 &&
            static_cast<size_t>(Assistant_State().layout.pin_flow_idx) < Assistant_State().layout.flow_first_page.size() &&
            Assistant_State().layout.flow_first_page[static_cast<size_t>(Assistant_State().layout.pin_flow_idx)] >= 0) {
            Assistant_State().layout.page_index = Assistant_State().layout.flow_first_page[static_cast<size_t>(Assistant_State().layout.pin_flow_idx)];
        } else if (jump_to_new) {
            const int last_page = static_cast<int>(Assistant_State().layout.pages.size()) - 1;
            if (last_page >= 0) {
                Assistant_State().layout.page_index = last_page;
            }
        }
        Assistant_ClampPageIndex();
    }
    PresentAppendUi(jump_to_new, reflow_from_page, true);
}

void CoalesceTimerCb(void* /*arg*/) {
    Application::GetInstance().Schedule([]() {
        FlowList chunk;
        bool jump = false;
        {
            if (!Assistant_State().stream.coalesce_pending) {
                return;
            }
            chunk = std::move(Assistant_State().stream.coalesce_buf);
            jump = Assistant_State().stream.coalesce_jump;
            Assistant_State().stream.coalesce_jump = false;
            Assistant_State().stream.coalesce_pending = false;
            Assistant_State().stream.coalesce_buf.clear();
        }
        if (chunk.empty() || !AssistantScreen::IsActive()) {
            return;
        }
        Assistant_AppendFlowAndShow(std::move(chunk), jump);
    });
}

bool EnsureCoalesceTimer() {
    if (Assistant_State().stream.coalesce_timer != nullptr) {
        return true;
    }
    const esp_timer_create_args_t args = {
        .callback = &CoalesceTimerCb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "as_coalesce",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &Assistant_State().stream.coalesce_timer) != ESP_OK) {
        Assistant_State().stream.coalesce_timer = nullptr;
        return false;
    }
    return true;
}

void Assistant_QueueOrShowChunk(FlowList&& chunk, bool jump_to_new) {
    if (chunk.empty()) {
        return;
    }
    // 用户开口 / 回放：立刻排版；assistant 流式合并，避免主循环被每包堵住
    if (jump_to_new || Assistant_State().layout.replaying_history) {
        if (Assistant_State().stream.coalesce_pending) {
            FlowList merged = std::move(Assistant_State().stream.coalesce_buf);
            const bool pending_jump = Assistant_State().stream.coalesce_jump;
            Assistant_State().stream.coalesce_buf.clear();
            Assistant_State().stream.coalesce_jump = false;
            Assistant_State().stream.coalesce_pending = false;
            if (Assistant_State().stream.coalesce_timer != nullptr) {
                esp_timer_stop(Assistant_State().stream.coalesce_timer);
            }
            if (!merged.empty()) {
                Assistant_AppendFlowAndShow(std::move(merged), pending_jump);
            }
        }
        Assistant_AppendFlowAndShow(std::move(chunk), jump_to_new);
        return;
    }
    if (!Assistant_State().stream.coalesce_buf.empty()) {
        FlowItem gap;
        gap.kind = FlowKind::MsgGap;
        Assistant_State().stream.coalesce_buf.push_back(std::move(gap));
    }
    for (auto& it : chunk) {
        Assistant_State().stream.coalesce_buf.push_back(std::move(it));
    }
    Assistant_State().stream.coalesce_pending = true;
    if (!EnsureCoalesceTimer()) {
        Assistant_AppendFlowAndShow(std::move(Assistant_State().stream.coalesce_buf), false);
        Assistant_State().stream.coalesce_pending = false;
        return;
    }
    esp_timer_stop(Assistant_State().stream.coalesce_timer);
    if (esp_timer_start_once(Assistant_State().stream.coalesce_timer, kStreamCoalesceUs) != ESP_OK) {
        Assistant_AppendFlowAndShow(std::move(Assistant_State().stream.coalesce_buf), false);
        Assistant_State().stream.coalesce_pending = false;
    }
}

void Assistant_ReplayHistoryFromStore()
{
    if (!assistant_chat_store_is_ready()) {
        ESP_LOGI(TAG, "history: no store (no SD or init skipped)");
        return;
    }

    /* Stream: store frees each file buf after the visitor returns. Parse here so
     * we never hold 40 JSON blobs (~192KB) plus a cJSON tree at once. Extra RAM
     * is one file (≤24KB) + one parse tree, then only FlowItem text (UI state). */
    struct Acc {
        FlowList chunk;
        unsigned ok = 0;
        unsigned skip = 0;
    } acc;

    Assistant_State().layout.replaying_history = true;
    const esp_err_t err = assistant_chat_store_for_each(
        [](const char* json, size_t len, void* ctx) -> bool {
            auto* a = static_cast<Acc*>(ctx);
            cJSON* root = cJSON_Parse(json);
            if (root == nullptr) {
                ESP_LOGW(TAG, "history skip: parse fail len=%u", static_cast<unsigned>(len));
                a->skip++;
                return true;
            }
            FlowList part;
            const IngestResult r = Assistant_IngestA2uiMessage(root, part);
            cJSON_Delete(root);
            if (r != IngestResult::kAppended || part.empty()) {
                a->skip++;
                return true;
            }
            if (!a->chunk.empty()) {
                FlowItem gap;
                gap.kind = FlowKind::MsgGap;
                a->chunk.push_back(std::move(gap));
            }
            for (auto& it : part) {
                a->chunk.push_back(std::move(it));
            }
            a->ok++;
            return true;
        },
        &acc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "history load: %s", esp_err_to_name(err));
        Assistant_State().layout.replaying_history = false;
        return;
    }
    if (acc.ok == 0) {
        ESP_LOGI(TAG, "history: empty (skip=%u)", acc.skip);
        Assistant_State().layout.replaying_history = false;
        return;
    }

    ESP_LOGI(TAG, "history replay %u record(s) skip=%u", acc.ok, acc.skip);
    if (!acc.chunk.empty()) {
        // 进页回放落在末条所在页（最近一轮）
        Assistant_AppendFlowAndShow(std::move(acc.chunk), true);
    }
    Assistant_State().layout.replaying_history = false;
}

