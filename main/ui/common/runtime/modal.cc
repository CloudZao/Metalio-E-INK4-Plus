#include <new>
#include <esp_log.h>
#include "modal.h"
#include "ui/common/runtime/input/vk_page_repeat.h"

static UiModal* s_modals = nullptr;

static int ModalLayerRank(lv_obj_t* root) {
    auto* layer = lv_obj_get_screen(root);
    return layer == lv_layer_sys() ? 2 : layer == lv_layer_top() ? 1 : 0;
}

static bool ModalIsAbove(lv_obj_t* first, lv_obj_t* second) {
    const int first_rank = ModalLayerRank(first), second_rank = ModalLayerRank(second);
    if (first_rank != second_rank) {
        return first_rank > second_rank;
    }
    int first_depth = 0, second_depth = 0;
    for (auto* obj = first; obj; obj = lv_obj_get_parent(obj)) {
        ++first_depth;
    }
    for (auto* obj = second; obj; obj = lv_obj_get_parent(obj)) {
        ++second_depth;
    }
    while (first_depth > second_depth) {
        first = lv_obj_get_parent(first);
        --first_depth;
        if (first == second) {
            return true;
        }
    }
    while (second_depth > first_depth) {
        second = lv_obj_get_parent(second);
        --second_depth;
        if (first == second) {
            return false;
        }
    }
    while (lv_obj_get_parent(first) != lv_obj_get_parent(second)) {
        first = lv_obj_get_parent(first);
        second = lv_obj_get_parent(second);
    }
    return lv_obj_get_index(first) > lv_obj_get_index(second);
}

void UiModal::Bind(lv_obj_t* root, void (*cancel)(void*), void* context, UiModalPolicy policy, void (*leave)(void*)) {
    Unbind();
    if (!root) {
        return;
    }
    root_ = root;
    owner_ = lv_obj_get_screen(root);
    cancel_ = cancel;
    leave_ = leave;
    context_ = context;
    policy_ = policy;
    lifecycle_.Bind(root, {}, true);
    next_ = s_modals;
    s_modals = this;
    lv_obj_add_event_cb(root, OnEvent, LV_EVENT_ALL, this);
    if (owner_ != root) {
        lv_obj_add_event_cb(owner_, OnEvent, LV_EVENT_SCREEN_UNLOADED, this);
    }
    VkPageRepeatStop();
}

void UiModal::Unbind() {
    if (!root_) {
        return;
    }
    lifecycle_.Unbind();
    lv_obj_remove_event_cb_with_user_data(root_, OnEvent, this);
    if (owner_ != root_) {
        lv_obj_remove_event_cb_with_user_data(owner_, OnEvent, this);
    }
    auto** link = &s_modals;
    while (*link && *link != this) {
        link = &(*link)->next_;
    }
    if (*link) {
        *link = next_;
    }
    root_ = owner_ = nullptr;
    next_ = nullptr;
    cancel_ = nullptr;
    leave_ = nullptr;
    context_ = nullptr;
}

bool UiModal::HandleInput(UiModalInput input) {
    UiModal* top = nullptr;
    for (auto* modal = s_modals; modal; modal = modal->next_) {
        if (modal->lifecycle_.IsCurrent(modal->lifecycle_.Generation()) && lv_obj_is_visible(modal->root_) &&
            (!top || ModalIsAbove(modal->root_, top->root_))) {
            top = modal;
        }
    }
    if (!top) {
        return false;
    }
    if (input == UiModalInput::Back && top->policy_ == UiModalPolicy::Dismissible) {
        top->Close();
    }
    return true;
}

bool UiModal::IsRoot(lv_obj_t* object) {
    for (auto* modal = s_modals; modal; modal = modal->next_) {
        if (modal->root_ == object) {
            return true;
        }
    }
    return false;
}

UiModal::~UiModal() {
    Unbind();
}

void UiModal::OnEvent(lv_event_t* event) {
    auto* modal = static_cast<UiModal*>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_DELETE && lv_event_get_target(event) == modal->root_) {
        modal->Unbind();
    } else if (lv_event_get_code(event) == LV_EVENT_SCREEN_UNLOADED && lv_event_get_target(event) == modal->owner_) {
        modal->Close(true);
    } else if (lv_event_get_code(event) == LV_EVENT_LAYOUT_CHANGED && lv_event_get_target(event) == modal->root_ &&
               VkPageRepeatIsActive() && lv_obj_is_visible(modal->root_)) {
        VkPageRepeatStop();
    }
}

void UiModal::Close(bool leaving) try {
    auto* root = root_;
    auto cancel = leaving && leave_ ? leave_ : cancel_;
    void* context = context_;
    // 异步取消期间继续保留输入所有权，避免尚未退出的进度窗口穿透按键。
    if (cancel) {
        cancel(context);
    } else {
        lv_obj_delete(root);
    }
} catch (const std::bad_alloc&) {
    ESP_LOGE("Modal", "operation=close stage=%s result=no_memory", leaving ? "leave" : "cancel");
}
