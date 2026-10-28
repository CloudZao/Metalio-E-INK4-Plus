#include "assets/lang_screen.h"
#include "settings_storage_tab.h"

#include "SdCardManager.hpp"
#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "sd_paths.h"
#include "haptic_feedback.h"
#include "ui_scale.h"
#include "usb_virtual_disk.h"
#include "screen_common.h"

#include <cstdio>
#include <cstdint>

#include <esp_log.h>
#include <esp_vfs_fat.h>

static constexpr const char* TAG = "SettingsStorage";

struct SettingsStorageUi {
    lv_obj_t* status_lbl = nullptr;
    lv_obj_t* capacity_lbl = nullptr;
    lv_obj_t* usb_btn = nullptr;
    lv_obj_t* usb_btn_lbl = nullptr;
    lv_obj_t* hint_lbl = nullptr;
    bool alive = false;
};

static SettingsStorageUi s_ui;

static bool QueryCapacity(uint64_t* total_bytes, uint64_t* free_bytes) {
    if (total_bytes == nullptr || free_bytes == nullptr) {
        return false;
    }
    *total_bytes = 0;
    *free_bytes = 0;
    return esp_vfs_fat_info(SD_MOUNT_POINT, total_bytes, free_bytes) == ESP_OK;
}

static void RefreshStorageUi() {
    if (!s_ui.alive) {
        return;
    }

    auto& sd = SdCardManager::GetInstance();
    auto& vd = UsbVirtualDisk::GetInstance();
    const bool exported = vd.IsSdExportedToHost();
    const bool has_card = sd.IsMounted() || vd.IsGadgetActive() || exported;
    const bool usable = sd.IsMounted() && !exported && !vd.IsBusy();

    if (s_ui.status_lbl != nullptr) {
        if (has_card) {
            lv_label_set_text(s_ui.status_lbl, exported ? Lang::Strings::SETTINGS_STORAGE_PC_BUSY
                                                       : Lang::Strings::SETTINGS_STORAGE_INSERTED);
        } else {
            lv_label_set_text(s_ui.status_lbl, Lang::Strings::SETTINGS_STORAGE_MISSING);
        }
    }

    if (s_ui.capacity_lbl != nullptr) {
        if (!has_card) {
            lv_label_set_text(s_ui.capacity_lbl, Lang::Strings::SETTINGS_STORAGE_CAP_DASH);
        } else if (!usable) {
            lv_label_set_text(s_ui.capacity_lbl, Lang::Strings::SETTINGS_STORAGE_CAP_EXPORT);
        } else {
            uint64_t total_bytes = 0;
            uint64_t free_bytes = 0;
            char line[96];
            if (QueryCapacity(&total_bytes, &free_bytes)) {
                const double gb = 1024.0 * 1024.0 * 1024.0;
                std::snprintf(line, sizeof(line), Lang::Strings::SETTINGS_STORAGE_CAP_FMT,
                              static_cast<double>(free_bytes) / gb,
                              static_cast<double>(total_bytes) / gb);
                lv_label_set_text(s_ui.capacity_lbl, line);
            } else {
                lv_label_set_text(s_ui.capacity_lbl, Lang::Strings::SETTINGS_STORAGE_CAP_FAIL);
            }
        }
    }

    if (s_ui.usb_btn_lbl != nullptr) {
        lv_label_set_text(s_ui.usb_btn_lbl, vd.IsGadgetActive()
                                                ? Lang::Strings::SETTINGS_STORAGE_USB_OFF
                                                : Lang::Strings::SETTINGS_STORAGE_USB_ON);
    }
    if (s_ui.usb_btn != nullptr) {
        const bool on = vd.IsGadgetActive();
        lv_obj_set_style_bg_color(s_ui.usb_btn, on ? lv_color_black() : lv_color_white(), 0);
        if (s_ui.usb_btn_lbl != nullptr) {
            lv_obj_set_style_text_color(s_ui.usb_btn_lbl, on ? lv_color_white() : lv_color_black(), 0);
        }
        // 忙时仍可点：翻转期望态，避免反复开关时点击被吞掉。
        const bool can_click =
            vd.IsSupported() && (vd.IsGadgetActive() || has_card || vd.IsBusy());
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
            lv_label_set_text(s_ui.hint_lbl, Lang::Strings::SETTINGS_STORAGE_USB_DISABLED);
        } else {
            lv_label_set_text(s_ui.hint_lbl, UsbVirtualDisk::HintText(vd.GetUiHint()));
        }
    }
}

static void OnUsbUiNotifyAsync(void* /*user_data*/) {
    if (!s_ui.alive) {
        return;
    }
    RefreshStorageUi();
}

static void OnUsbVirtualDiskNotify() {
    if (!ScreenLvAsync(OnUsbUiNotifyAsync)) {
        ESP_LOGW(TAG, "usb ui enqueue failed");
    }
}

static void OnUsbButtonClicked(lv_event_t* /*e*/) {
    auto& vd = UsbVirtualDisk::GetInstance();
    if (!vd.IsSupported()) {
        return;
    }
    vd.Toggle();
    RefreshStorageUi();
}

void SettingsStorageTab_Reset() {
    auto& vd = UsbVirtualDisk::GetInstance();
    vd.DisableIfActive();
    vd.SetUiNotify(nullptr);
    s_ui = {};
}

static lv_obj_t* AddCenterLabel(lv_obj_t* page, const char* text) {
    lv_obj_t* lbl = lv_label_create(page);
    lv_label_set_text(lbl, text);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

void SettingsStorageTab_Build(lv_obj_t* page) {
    s_ui = {};
    s_ui.alive = true;

    AddCenterLabel(page, Lang::Strings::CLOUD_USB_WAY);

    s_ui.usb_btn = lv_obj_create(page);
    lv_obj_remove_style_all(s_ui.usb_btn);
    lv_obj_set_width(s_ui.usb_btn, lv_pct(100));
    lv_obj_set_height(s_ui.usb_btn, UiSy(64));
    lv_obj_set_style_bg_opa(s_ui.usb_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_ui.usb_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_ui.usb_btn, UiSx(2), 0);
    lv_obj_set_style_radius(s_ui.usb_btn, UiSx(16), 0);
    lv_obj_clear_flag(s_ui.usb_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.usb_btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(s_ui.usb_btn);
    lv_obj_add_event_cb(s_ui.usb_btn, OnUsbButtonClicked, LV_EVENT_CLICKED, nullptr);
    s_ui.usb_btn_lbl = lv_label_create(s_ui.usb_btn);
    lv_label_set_text(s_ui.usb_btn_lbl, Lang::Strings::SETTINGS_STORAGE_USB_ON);
    lv_obj_set_style_text_font(s_ui.usb_btn_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_center(s_ui.usb_btn_lbl);
    lv_obj_clear_flag(s_ui.usb_btn_lbl, LV_OBJ_FLAG_CLICKABLE);

    s_ui.hint_lbl = AddCenterLabel(page, "");
    s_ui.status_lbl = AddCenterLabel(page, "");
    s_ui.capacity_lbl = AddCenterLabel(page, "");

    auto& vd = UsbVirtualDisk::GetInstance();
    vd.SetUiNotify(OnUsbVirtualDiskNotify);
    RefreshStorageUi();
}
