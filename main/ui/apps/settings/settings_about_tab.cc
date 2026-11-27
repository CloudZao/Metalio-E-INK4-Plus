#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_psram.h>
#include <freertos/FreeRTOS.h>
#include <lvgl.h>
#include "sysmgmt/update/firmware_update.h"
#include "ui/apps/settings/settings_tabs.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "boards/common/board/board.h"
#include "ui/apps/settings/settings_common.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/common/drawing/display.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "sysmgmt/update/update_presentation.h"
#include "ui/common/drawing/ui_theme.h"

static constexpr const char* TAG = "SettingsAbout";

static std::atomic<bool> s_tab_alive{false};
static std::atomic<uint32_t> s_check_gen{0};
static std::atomic<bool> s_check_busy{false};
static bool s_about_prompt = false;  // LVGL task / lock only.

struct SettingsAboutTabSettingsAboutInfoItem {
    const char* label;
    char value[96];
    bool firmware_row = false;
};

struct SettingsAboutTabCheckResultMsg {
    uint32_t gen = 0;            // 页面请求代次
    FirmwareCheckResult result;  // 领域查询结果，不截断下载地址
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

static void CollectInfoItems(SettingsAboutTabSettingsAboutInfoItem* items, int* count) {
    const auto* app_desc = esp_app_get_description();
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    int idx = 0;

    items[idx].label = LangString_SETTINGS_ABOUT_MODEL;
    std::snprintf(items[idx].value, sizeof(items[idx].value), "%s", BOARD_NAME);
    ++idx;

    items[idx].label = LangString_SETTINGS_ABOUT_CHIP;
    const char* chip_model = LangString_COMMON_UNKNOWN;
    switch (chip_info.model) {
        case CHIP_ESP32S3:
            chip_model = "ESP32-S3";
            break;
        case CHIP_ESP32S31:
            chip_model = "ESP32-S31";
            break;
        default:
            break;
    }
    std::snprintf(items[idx].value, sizeof(items[idx].value), "%s", chip_model);
    ++idx;

    items[idx].label = LangString_SETTINGS_ABOUT_CORES;
    std::snprintf(items[idx].value, sizeof(items[idx].value), LangString_SETTINGS_ABOUT_CORES_FMT,
                  static_cast<unsigned>(chip_info.cores));
    ++idx;

    items[idx].label = LangString_SETTINGS_ABOUT_FW;
    items[idx].firmware_row = true;
    std::snprintf(items[idx].value, sizeof(items[idx].value), LangString_SETTINGS_ABOUT_FW_VER_FMT, app_desc->version);
    ++idx;

    items[idx].label = LangString_SETTINGS_ABOUT_BUILD;
    std::snprintf(items[idx].value, sizeof(items[idx].value), "%s %s", app_desc->date, app_desc->time);
    ++idx;

    items[idx].label = LangString_SETTINGS_ABOUT_MAC;
    {
        uint8_t mac[6] = {};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        std::snprintf(items[idx].value, sizeof(items[idx].value), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
                      mac[2], mac[3], mac[4], mac[5]);
    }
    ++idx;

    items[idx].label = LangString_SETTINGS_ABOUT_FLASH;
    {
        uint32_t flash_size = 0;
        esp_flash_get_size(nullptr, &flash_size);
        FormatBytes(items[idx].value, sizeof(items[idx].value), flash_size);
    }
    ++idx;

    items[idx].label = LangString_SETTINGS_ABOUT_PSRAM;
    {
        const size_t psram_total = esp_psram_get_size();
        if (psram_total == 0) {
            std::snprintf(items[idx].value, sizeof(items[idx].value), "%s", LangString_SETTINGS_ABOUT_NONE);
        } else {
            FormatBytes(items[idx].value, sizeof(items[idx].value), psram_total);
        }
    }
    ++idx;

    *count = idx;
}

static void SetStatusTip(const char* text) {
    auto* display = Board::GetInstance().GetDisplay();
    if (display != nullptr && text != nullptr) {
        display->SetStatus(text);
    }
}

static void AsyncApplyCheckResult(void* p) try {
    std::unique_ptr<SettingsAboutTabCheckResultMsg> msg(static_cast<SettingsAboutTabCheckResultMsg*>(p));
    if (msg == nullptr) {
        return;
    }
    const uint32_t gen = msg->gen;
    const bool alive = s_tab_alive.load(std::memory_order_acquire);
    const bool current = (gen == s_check_gen.load(std::memory_order_acquire));

    if (!alive || !current) {
        return;
    }

    if (msg->result.status != FirmwareCheckStatus::Available) {
        SetStatusTip(msg->result.status != FirmwareCheckStatus::Current ? LangString_OTA_CHECK_FAILED
                                                                        : LangString_OTA_ALREADY_LATEST);
        s_check_busy.store(false, std::memory_order_release);
        return;
    }

    // 显示新版本确认框，升级交由工作任务执行。
    std::string url = std::move(msg->result.url);
    std::string cur = std::move(msg->result.current);
    std::string next = std::move(msg->result.version);

    const bool shown = OtaConfirmDialog::ShowAsync(
        cur.c_str(), next.c_str(), OtaConfirmDialog::Mode::About,
        [url = std::move(url), cur, next, gen](OtaConfirmDialog::Choice choice) {
            if (gen != s_check_gen.load(std::memory_order_acquire)) {
                return;
            }
            s_about_prompt = false;
            s_check_busy.store(false, std::memory_order_release);
            if (choice == OtaConfirmDialog::Choice::Upgrade && s_tab_alive.load(std::memory_order_acquire) &&
                !FirmwareUpdate::GetInstance().RequestFromUrl(url, cur, next)) {
                SetStatusTip(LangString_UPGRADE_FAILED);
            }
        });

    s_about_prompt = shown;
    if (!shown) {
        SetStatusTip(LangString_OTA_CHECK_FAILED);
        s_check_busy.store(false, std::memory_order_release);
    }
} catch (const std::bad_alloc&) {
    s_check_busy.store(false, std::memory_order_release);
    ESP_LOGE(TAG, "operation=present_firmware_check result=no_memory");
    SetStatusTip(LangString_OTA_CHECK_FAILED);
}

static void PostCheckResult(uint32_t gen, FirmwareCheckResult result) {
    auto* msg = new (std::nothrow) SettingsAboutTabCheckResultMsg{gen, std::move(result)};
    auto* adapter = LVAdapterDisplay::Instance();
    bool posted = false;
    if (adapter != nullptr && adapter->IsReady() && adapter->LockUi(-1)) {
        if (s_tab_alive.load(std::memory_order_acquire) && gen == s_check_gen.load(std::memory_order_acquire)) {
            posted = msg != nullptr && lv_async_call(AsyncApplyCheckResult, msg) == LV_RESULT_OK;
            if (!posted) {
                s_check_busy.store(false, std::memory_order_release);
                SetStatusTip(LangString_OTA_CHECK_FAILED);
                ESP_LOGE(TAG, "operation=present_firmware_check result=dispatch_failed");
            }
        }
        adapter->UnlockUi();
    }
    if (!posted) {
        delete msg;
    }
}

static void OnFirmwareRowClicked(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!s_tab_alive.load(std::memory_order_acquire)) {
        return;
    }
    if (s_check_busy.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (OtaConfirmDialog::IsActive()) {
        s_check_busy.store(false, std::memory_order_release);
        return;
    }

    const uint32_t gen = s_check_gen.fetch_add(1, std::memory_order_acq_rel) + 1;
    SetStatusTip(LangString_CHECKING_NEW_VERSION);
    if (!FirmwareUpdate::GetInstance().RequestVersionCheck(
            [gen] {
                return s_tab_alive.load(std::memory_order_acquire) &&
                       gen == s_check_gen.load(std::memory_order_acquire);
            },
            [gen](FirmwareCheckResult result) { PostCheckResult(gen, std::move(result)); })) {
        SetStatusTip(LangString_OTA_CHECK_FAILED);
        s_check_busy.store(false, std::memory_order_release);
    }
}

static lv_obj_t* CreateInfoRow(lv_obj_t* parent, const SettingsAboutTabSettingsAboutInfoItem& item, bool with_divider) {
    auto* row = SettingsCreateListRow(parent, item.label, item.value,
                                      item.firmware_row ? OnFirmwareRowClicked : nullptr);
    if (!with_divider) {
        lv_obj_set_style_border_width(row, 0, 0);
    }
    return row;
}

void SettingsAboutTab_Build(lv_obj_t* page) {
    s_check_gen.fetch_add(1);
    s_check_busy.store(false, std::memory_order_release);
    s_tab_alive.store(true, std::memory_order_release);

    SettingsAboutTabSettingsAboutInfoItem items[8];
    int count = 0;
    CollectInfoItems(items, &count);

    auto* section = SettingsCreateSection(page, std::strcmp(LangCode, "zh-CN") == 0 ? "设备信息" : "Device information");
    lv_obj_t* list = SettingsCreateListGroup(section);
    lv_obj_add_event_cb(list, [](lv_event_t*) { SettingsAboutTab_Reset(); }, LV_EVENT_DELETE, nullptr);

    for (int i = 0; i < count; ++i) {
        CreateInfoRow(list, items[i], i + 1 < count);
    }
}

void SettingsAboutTab_Reset() {
    s_tab_alive.store(false, std::memory_order_release);
    s_check_gen.fetch_add(1, std::memory_order_acq_rel);
    s_check_busy.store(false, std::memory_order_release);
    if (s_about_prompt) {
        OtaConfirmDialog::Dismiss();
        s_about_prompt = false;
    }
}
