#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <cstdio>
#include <lvgl.h>
#include <esp_log.h>
#include "ui/common/widgets/storage_error_text.h"
#include "boards/common/storage/sd_access.h"
#include "ui/common/runtime/page_lifecycle.h"
#include "ui/apps/cloud/usb/cloud_usb_panel.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "boards/common/storage/SdCardManager.hpp"
#include "infra/storage/sd_paths.h"
#include "boards/common/storage/usb_virtual_disk.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/drawing/ui_scale.h"
#include "ui/common/drawing/ui_theme.h"

static constexpr const char* TAG = "CloudUsbPanel";

struct CloudUsbPanelUi {
    lv_obj_t* status_lbl = nullptr;
    lv_obj_t* capacity_lbl = nullptr;
    lv_obj_t* usb_btn = nullptr;
    lv_obj_t* usb_btn_lbl = nullptr;
    lv_obj_t* hint_lbl = nullptr;
    bool alive = false;
    bool capacity_applied = false;                     // 只消费一次查询结果
    std::shared_ptr<const SdCapacityResult> capacity;  // 工作任务和页面分别持有
};

static CloudUsbPanelUi s_ui;
static UiPageLifecycle s_page;

static std::atomic<bool> s_usb_changed{false};  // 工作任务只发布状态变化

static void SetStorageText(lv_obj_t* label, const char* text) {
    if (label && std::strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void RefreshStorageUi() {
    if (!s_ui.alive) {
        return;
    }

    auto& sd = SdCardManager::GetInstance();
    auto& vd = UsbVirtualDisk::GetInstance();
    const bool exported = vd.IsSdExportedToHost();
    const bool has_card = sd.IsMounted() || vd.IsGadgetActive() || exported;
    const auto status = SdAccessStatus();
    const bool usable = !status && !exported && !vd.IsBusy();

    if (s_ui.status_lbl != nullptr) {
        if (status) {
            SetStorageText(s_ui.status_lbl, StorageErrorText(status));
        } else if (has_card) {
            SetStorageText(s_ui.status_lbl,
                           exported ? LangString_SETTINGS_STORAGE_PC_BUSY : LangString_SETTINGS_STORAGE_INSERTED);
        } else {
            SetStorageText(s_ui.status_lbl, LangString_SETTINGS_STORAGE_MISSING);
        }
    }

    if (!usable) {
        s_ui.capacity.reset();
        s_ui.capacity_applied = false;
    }
    if (s_ui.capacity_lbl != nullptr) {
        if (!has_card) {
            SetStorageText(s_ui.capacity_lbl, LangString_SETTINGS_STORAGE_CAP_DASH);
        } else if (!usable) {
            SetStorageText(s_ui.capacity_lbl, LangString_SETTINGS_STORAGE_CAP_EXPORT);
        } else {
            if (!s_ui.capacity || s_ui.capacity->mount_generation != SdAccessMountGeneration()) {
                s_ui.capacity = sd.QueryCapacityAsync();
                s_ui.capacity_applied = false;
            }
            if (!s_ui.capacity) {
                SetStorageText(s_ui.capacity_lbl, LangString_SETTINGS_STORAGE_CAP_FAIL);
            } else {
                const auto& request = *s_ui.capacity;
                char line[96];
                if (!request.ready.load(std::memory_order_acquire)) {
                    SetStorageText(s_ui.capacity_lbl, LangString_SETTINGS_STORAGE_CAP_DASH);
                } else if (request.ok) {
                    s_ui.capacity_applied = true;
                    const double gb = 1024.0 * 1024.0 * 1024.0;
                    std::snprintf(line, sizeof(line), LangString_SETTINGS_STORAGE_CAP_FMT,
                                  static_cast<double>(request.free_bytes) / gb,
                                  static_cast<double>(request.total_bytes) / gb);
                    SetStorageText(s_ui.capacity_lbl, line);
                } else {
                    s_ui.capacity_applied = true;
                    SetStorageText(s_ui.capacity_lbl, StorageErrorText(request.error));
                }
            }
        }
    }

    if (s_ui.usb_btn_lbl != nullptr) {
        SetStorageText(s_ui.usb_btn_lbl,
                       vd.IsGadgetActive() ? LangString_SETTINGS_STORAGE_USB_OFF : LangString_SETTINGS_STORAGE_USB_ON);
    }
    if (s_ui.usb_btn != nullptr) {
        const bool on = vd.IsGadgetActive();
        lv_obj_set_style_bg_color(s_ui.usb_btn, on ? UiColorForeground() : UiColorBackground(), 0);
        if (s_ui.usb_btn_lbl != nullptr) {
            lv_obj_set_style_text_color(s_ui.usb_btn_lbl, on ? UiColorBackground() : UiColorForeground(), 0);
        }
        // 忙时仍可点：翻转期望态，避免反复开关时点击被吞掉。
        const bool can_click = vd.IsSupported() && (vd.IsGadgetActive() || has_card || vd.IsBusy());
        if (can_click) {
            lv_obj_add_flag(s_ui.usb_btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_state(s_ui.usb_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_flag(s_ui.usb_btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_state(s_ui.usb_btn, LV_STATE_DISABLED);
        }
    }
    if (s_ui.hint_lbl != nullptr) {
        if (!vd.IsSupported()) {
            SetStorageText(s_ui.hint_lbl, LangString_SETTINGS_STORAGE_USB_DISABLED);
        } else {
            SetStorageText(s_ui.hint_lbl, UsbVirtualDisk::HintText(vd.GetUiHint()));
        }
    }
}

static void OnUsbVirtualDiskNotify() {
    s_usb_changed.store(true, std::memory_order_release);
}

static void OnUsbButtonClicked(lv_event_t* /*e*/) {
    auto& vd = UsbVirtualDisk::GetInstance();
    if (!vd.IsSupported()) {
        return;
    }
    vd.Toggle();
    RefreshStorageUi();
}

static lv_obj_t* AddCenterLabel(lv_obj_t* page, const char* text) {
    lv_obj_t* lbl = lv_label_create(page);
    lv_label_set_text(lbl, text);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl, UiFont(), 0);
    lv_obj_set_style_text_color(lbl, UiColorForeground(), 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

void CloudUsbPanelBuild(lv_obj_t* page) {
    s_ui = {};
    s_ui.alive = true;

    AddCenterLabel(page, LangString_CLOUD_USB_WAY);

    s_ui.usb_btn = lv_obj_create(page);
    lv_obj_remove_style_all(s_ui.usb_btn);
    lv_obj_set_width(s_ui.usb_btn, lv_pct(100));
    lv_obj_set_height(s_ui.usb_btn, UiSy(64));
    lv_obj_set_style_bg_opa(s_ui.usb_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_ui.usb_btn, UiColorForeground(), 0);
    lv_obj_set_style_border_width(s_ui.usb_btn, UiSx(2), 0);
    lv_obj_set_style_radius(s_ui.usb_btn, UiSx(16), 0);
    lv_obj_clear_flag(s_ui.usb_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.usb_btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(s_ui.usb_btn);
    lv_obj_add_event_cb(s_ui.usb_btn, OnUsbButtonClicked, LV_EVENT_CLICKED, nullptr);
    s_ui.usb_btn_lbl = lv_label_create(s_ui.usb_btn);
    SetStorageText(s_ui.usb_btn_lbl, LangString_SETTINGS_STORAGE_USB_ON);
    lv_obj_set_style_text_font(s_ui.usb_btn_lbl, UiFont(), 0);
    lv_obj_center(s_ui.usb_btn_lbl);
    lv_obj_clear_flag(s_ui.usb_btn_lbl, LV_OBJ_FLAG_CLICKABLE);

    s_ui.hint_lbl = AddCenterLabel(page, "");
    s_ui.status_lbl = AddCenterLabel(page, "");
    s_ui.capacity_lbl = AddCenterLabel(page, "");

    auto& vd = UsbVirtualDisk::GetInstance();
    vd.SetUiNotify(OnUsbVirtualDiskNotify);
    s_page.Bind(page, {[] {
        if (s_usb_changed.exchange(false) ||
            (s_ui.capacity && !s_ui.capacity_applied && s_ui.capacity->ready.load(std::memory_order_acquire))) {
            RefreshStorageUi();
        }
    }, nullptr, nullptr, 250}, true);
    RefreshStorageUi();
}

void CloudUsbPanelReset() {
    auto& vd = UsbVirtualDisk::GetInstance();
    vd.DisableIfActive();
    vd.SetUiNotify(nullptr);
    s_page.Unbind();
    s_ui = {};
}
