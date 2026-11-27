#include <new>
#include <esp_log.h>
#include "page_lifecycle.h"

static UiPageLifecycle* s_pages = nullptr; // 仅由UI任务维护，实例由页面所有者持有

void UiPageLifecycle::Bind(lv_obj_t* root, UiPageHooks hooks, bool overlay) {
    Unbind();
    if (!root) {
        return;
    }
    root_ = root;
    hooks_ = hooks;
    overlay_ = overlay;
    paused_ = false;
    last_update_ = lv_tick_get();
    active_.store(true, std::memory_order_release);
    next_ = s_pages;
    s_pages = this;
    lv_obj_add_event_cb(root, OnEvent, LV_EVENT_ALL, this);
}

void UiPageLifecycle::Unbind() {
    active_.store(false, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_acq_rel);
    if (!root_) {
        return;
    }
    lv_obj_remove_event_cb_with_user_data(root_, OnEvent, this);
    root_ = nullptr;
    auto** link = &s_pages;
    while (*link && *link != this) {
        link = &(*link)->next_;
    }
    if (*link) {
        *link = next_;
    }
    next_ = nullptr;
    hooks_ = {};
}

uint32_t UiPageLifecycle::Generation() const {
    return generation_.load(std::memory_order_acquire);
}

bool UiPageLifecycle::IsCurrent(uint32_t generation) const {
    return active_.load(std::memory_order_acquire) && generation == Generation();
}

void UiPageLifecycle::OnEvent(lv_event_t* event) {
    auto* page = static_cast<UiPageLifecycle*>(lv_event_get_user_data(event));
    if (lv_event_get_target(event) != page->root_) {
        return;
    }
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_SCREEN_UNLOADED || code == LV_EVENT_DELETE) {
        if (page->active_.exchange(false, std::memory_order_acq_rel)) {
            page->generation_.fetch_add(1, std::memory_order_acq_rel);
            if (page->hooks_.leave) {
                page->hooks_.leave();
            }
        }
        if (code == LV_EVENT_DELETE && page->root_ == lv_event_get_target(event)) {
            page->Unbind();
        }
    } else if (code == LV_EVENT_SCREEN_LOADED) {
        page->active_.store(true, std::memory_order_release);
    }
}

void UiPageLifecycle::Poll() {
    const uint32_t now = lv_tick_get();
    for (auto* page = s_pages; page;) {
        auto* next = page->next_;
        if (page->active_.load(std::memory_order_acquire) && !page->paused_ && page->hooks_.update &&
            page->hooks_.update_ms != 0 &&
            (page->root_ == lv_screen_active() || (page->overlay_ && lv_obj_is_visible(page->root_))) &&
            now - page->last_update_ >= page->hooks_.update_ms) {
            page->last_update_ = now;
            try {
                page->hooks_.update();
            } catch (const std::bad_alloc&) {
                ESP_LOGE("PageLifecycle", "operation=update result=no_memory");
            }
        }
        page = next;
    }
}

void UiPageLifecycle::NotifyPower(UiPagePowerEvent event) {
    for (auto* page = s_pages; page;) {
        auto* next = page->next_;
        if (page->active_.load(std::memory_order_acquire)) {
            if (event == UiPagePowerEvent::PauseViews) {
                page->paused_ = true;
            } else if (event == UiPagePowerEvent::ResumeViews) {
                page->paused_ = false;
            }
            if (page->hooks_.power) {
                page->hooks_.power(event);
            }
        }
        page = next;
    }
}
