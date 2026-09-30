#include "settings_storage_tab.h"

#include "SdCardManager.hpp"
#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "reader/file_size.h"
#include "sd_paths.h"
#include "settings_common.h"
#include "usb_virtual_disk.h"

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
    const bool usable = sd.IsMounted() && !exported;

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
            char total_str[32];
            char free_str[32];
            char line[96];
            if (QueryCapacity(&total_bytes, &free_bytes)) {
                reader::FormatFileSize(total_str, sizeof(total_str), total_bytes);
                reader::FormatFileSize(free_str, sizeof(free_str), free_bytes);
                std::snprintf(line, sizeof(line), Lang::Strings::SETTINGS_STORAGE_CAP_FMT, free_str,
                              total_str);
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
        SettingsStyleSelectable(s_ui.usb_btn, s_ui.usb_btn_lbl, vd.IsGadgetActive());
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
    if (lv_async_call(OnUsbUiNotifyAsync, nullptr) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "lv_async_call failed for usb notify");
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

void SettingsStorageTab_Build(lv_obj_t* page) {
    s_ui = {};
    s_ui.alive = true;

    lv_obj_t* title = lv_label_create(page);
    lv_label_set_text(title, Lang::Strings::SETTINGS_STORAGE_TITLE);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    s_ui.status_lbl = lv_label_create(page);
    lv_label_set_text(s_ui.status_lbl, "");
    lv_obj_set_width(s_ui.status_lbl, lv_pct(100));
    lv_label_set_long_mode(s_ui.status_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_ui.status_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_ui.status_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(s_ui.status_lbl, LV_OBJ_FLAG_CLICKABLE);

    s_ui.capacity_lbl = lv_label_create(page);
    lv_label_set_text(s_ui.capacity_lbl, "");
    lv_obj_set_width(s_ui.capacity_lbl, lv_pct(100));
    lv_label_set_long_mode(s_ui.capacity_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_ui.capacity_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_ui.capacity_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(s_ui.capacity_lbl, LV_OBJ_FLAG_CLICKABLE);

    s_ui.usb_btn =
        SettingsCreateSelectableOption(page, Lang::Strings::SETTINGS_STORAGE_USB_ON, OnUsbButtonClicked, 0);
    s_ui.usb_btn_lbl = lv_obj_get_child(s_ui.usb_btn, 0);

    s_ui.hint_lbl = lv_label_create(page);
    lv_label_set_text(s_ui.hint_lbl, "");
    lv_obj_set_width(s_ui.hint_lbl, lv_pct(100));
    lv_label_set_long_mode(s_ui.hint_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.hint_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_ui.hint_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(s_ui.hint_lbl, LV_OBJ_FLAG_CLICKABLE);

    auto& vd = UsbVirtualDisk::GetInstance();
    vd.Init();
    vd.SetUiNotify(OnUsbVirtualDiskNotify);
    RefreshStorageUi();
}
