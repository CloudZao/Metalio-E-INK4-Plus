#include "settings_about_tab.h"

#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "settings_common.h"

#include <cstdio>
#include <cstring>

#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_mac.h>
#include <esp_psram.h>
#include <esp_system.h>


struct SettingsAboutInfoItem {
    const char* label;
    char value[96];
};

static void FormatBytes(char* out, size_t out_len, uint64_t bytes) {
    if (bytes >= (1024ull * 1024ull * 1024ull)) {
        std::snprintf(out, out_len, "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= (1024ull * 1024ull)) {
        std::snprintf(out, out_len, "%.1f MB", bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        std::snprintf(out, out_len, "%.0f KB", bytes / 1024.0);
    } else {
        std::snprintf(out, out_len, "%llu B", (unsigned long long)bytes);
    }
}

static void CollectInfoItems(SettingsAboutInfoItem* items, int* count) {
    const auto* app_desc = esp_app_get_description();
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    int idx = 0;

    items[idx].label = Lang::Strings::SETTINGS_ABOUT_MODEL;
    std::snprintf(items[idx].value, sizeof(items[idx].value), "%s", BOARD_NAME);
    ++idx;

    items[idx].label = Lang::Strings::SETTINGS_ABOUT_CHIP;
    std::snprintf(items[idx].value, sizeof(items[idx].value), "ESP32-S31");
    ++idx;

    items[idx].label = Lang::Strings::SETTINGS_ABOUT_CORES;
    std::snprintf(items[idx].value, sizeof(items[idx].value), Lang::Strings::SETTINGS_ABOUT_CORES_FMT,
                  static_cast<unsigned>(chip_info.cores));
    ++idx;

    items[idx].label = Lang::Strings::SETTINGS_ABOUT_FW;
    std::snprintf(items[idx].value, sizeof(items[idx].value), Lang::Strings::SETTINGS_ABOUT_FW_VER_FMT,
                  app_desc->version);
    ++idx;

    items[idx].label = Lang::Strings::SETTINGS_ABOUT_BUILD;
    std::snprintf(items[idx].value, sizeof(items[idx].value), "%s %s", app_desc->date, app_desc->time);
    ++idx;

    items[idx].label = Lang::Strings::SETTINGS_ABOUT_MAC;
    {
        uint8_t mac[6] = {};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        std::snprintf(items[idx].value, sizeof(items[idx].value),
                      "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
                      mac[5]);
    }
    ++idx;

    items[idx].label = Lang::Strings::SETTINGS_ABOUT_FLASH;
    {
        uint32_t flash_size = 0;
        esp_flash_get_size(nullptr, &flash_size);
        FormatBytes(items[idx].value, sizeof(items[idx].value), flash_size);
    }
    ++idx;

    items[idx].label = Lang::Strings::SETTINGS_ABOUT_PSRAM;
    {
        const size_t psram_total = esp_psram_get_size();
        if (psram_total == 0) {
            std::snprintf(items[idx].value, sizeof(items[idx].value), "%s",
                          Lang::Strings::SETTINGS_ABOUT_NONE);
        } else {
            FormatBytes(items[idx].value, sizeof(items[idx].value), psram_total);
        }
    }
    ++idx;

    *count = idx;
}

static lv_obj_t* CreateInfoRow(lv_obj_t* parent, const SettingsAboutInfoItem& item, bool with_divider) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, kSettingsInfoRowH);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    if (with_divider) {
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
    }
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_hor(row, kSettingsInfoRowPadH, 0);
    lv_obj_set_style_pad_ver(row, kSettingsInfoRowPadV, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(row, 2, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title = lv_label_create(row);
    lv_label_set_text(title, item.label);
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* subtitle = lv_label_create(row);
    lv_label_set_text(subtitle, item.value);
    lv_obj_set_width(subtitle, lv_pct(100));
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(subtitle, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(subtitle, lv_color_black(), 0);
    lv_obj_clear_flag(subtitle, LV_OBJ_FLAG_CLICKABLE);

    return row;
}

void SettingsAboutTab_Reset() {}

void SettingsAboutTab_Build(lv_obj_t* page) {
    SettingsAboutInfoItem items[8];
    int count = 0;
    CollectInfoItems(items, &count);

    lv_obj_t* list = lv_obj_create(page);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_height(list, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list, kSettingsInfoRowGap, 0);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < count; ++i) {
        CreateInfoRow(list, items[i], i + 1 < count);
    }
}
