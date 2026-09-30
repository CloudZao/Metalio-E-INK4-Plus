#include "book_screen/settings/book_tap_zones.h"

#include "settings.h"

#include <cstdint>
#include <cstring>
#include <string>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>


static constexpr const char* TAG = "BookTapZones";
static constexpr const char* kNvsNs = "reader";
static constexpr const char* kNvsKey = "tap_zones";

// 行主序：col0=Prev, col1=Menu, col2=Next
static constexpr uint8_t kDefaultCells[kBookTapZoneCellCount] = {
    static_cast<uint8_t>(BookTapZoneAction::kPrev),
    static_cast<uint8_t>(BookTapZoneAction::kMenu),
    static_cast<uint8_t>(BookTapZoneAction::kNext),
    static_cast<uint8_t>(BookTapZoneAction::kPrev),
    static_cast<uint8_t>(BookTapZoneAction::kMenu),
    static_cast<uint8_t>(BookTapZoneAction::kNext),
    static_cast<uint8_t>(BookTapZoneAction::kPrev),
    static_cast<uint8_t>(BookTapZoneAction::kMenu),
    static_cast<uint8_t>(BookTapZoneAction::kNext),
};

static_assert(kBookTapZoneCellCount == 9, "3x3");
static_assert(kDefaultCells[0] == static_cast<uint8_t>(BookTapZoneAction::kPrev), "left col");
static_assert(kDefaultCells[1] == static_cast<uint8_t>(BookTapZoneAction::kMenu), "mid col");
static_assert(kDefaultCells[2] == static_cast<uint8_t>(BookTapZoneAction::kNext), "right col");

static uint8_t s_cells[kBookTapZoneCellCount] = {};
static bool s_ready = false;

static bool ActionOk(uint8_t v) {
    return v < static_cast<uint8_t>(BookTapZoneAction::kCount);
}
bool ParsePacked(const std::string& packed, uint8_t* out) {
    if (out == nullptr || packed.size() != static_cast<size_t>(kBookTapZoneCellCount)) {
        return false;
    }
    uint8_t tmp[kBookTapZoneCellCount];
    for (int i = 0; i < kBookTapZoneCellCount; ++i) {
        const char c = packed[static_cast<size_t>(i)];
        if (c < '0' || c > '9') {
            return false;
        }
        const uint8_t v = static_cast<uint8_t>(c - '0');
        if (!ActionOk(v)) {
            return false;
        }
        tmp[i] = v;
    }
    std::memcpy(out, tmp, sizeof(tmp));
    return true;
}

static void CopyDefault() {
    std::memcpy(s_cells, kDefaultCells, sizeof(s_cells));
}

static void LoadFromNvs() {
    CopyDefault();
    Settings settings(kNvsNs, false);
    const std::string packed = settings.GetString(kNvsKey, "");
    if (!packed.empty() && !ParsePacked(packed, s_cells)) {
        ESP_LOGW(TAG, "nvs tap_zones invalid, using defaults");
        CopyDefault();
    }
    s_ready = true;
    ESP_LOGI(TAG, "nvs load tap_zones");
}

struct BookTapPersistArg {
    uint8_t cells[kBookTapZoneCellCount] = {};
};

static void PersistTask(void* arg) {
    auto* p = static_cast<BookTapPersistArg*>(arg);
    if (p == nullptr) {
        vTaskDelete(nullptr);
        return;
    }
    char packed[kBookTapZoneCellCount + 1] = {};
    for (int i = 0; i < kBookTapZoneCellCount; ++i) {
        packed[i] = static_cast<char>('0' + p->cells[i]);
    }
    packed[kBookTapZoneCellCount] = '\0';
    Settings settings(kNvsNs, true);
    settings.SetString(kNvsKey, packed);
    ESP_LOGI(TAG, "nvs save tap_zones=%s", packed);
    delete p;
    vTaskDelete(nullptr);
}

static bool StartPersist() {
    auto* arg = new BookTapPersistArg{};
    std::memcpy(arg->cells, s_cells, sizeof(s_cells));
    if (xTaskCreate(PersistTask, "tap_zones_nvs", 4096, arg, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(tap_zones_nvs) failed");
        delete arg;
        return false;
    }
    return true;
}


void BookTapZonesEnsureLoaded() {
    if (!s_ready) {
        LoadFromNvs();
    }
}

BookTapZoneAction BookTapZonesActionAt(int cell) {
    if (cell < 0 || cell >= kBookTapZoneCellCount) {
        return BookTapZoneAction::kNone;
    }
    // 未 Ensure 时勿在 LVGL/PSRAM 栈碰 NVS：回退默认图（开机已 Ensure）
    if (!s_ready) {
        return static_cast<BookTapZoneAction>(kDefaultCells[cell]);
    }
    const uint8_t v = s_cells[cell];
    if (!ActionOk(v)) {
        return static_cast<BookTapZoneAction>(kDefaultCells[cell]);
    }
    return static_cast<BookTapZoneAction>(v);
}

void BookTapZonesSetAction(int cell, BookTapZoneAction action) {
    BookTapZonesEnsureLoaded();
    if (cell < 0 || cell >= kBookTapZoneCellCount) {
        ESP_LOGW(TAG, "reject bad cell=%d", cell);
        return;
    }
    const uint8_t v = static_cast<uint8_t>(action);
    if (!ActionOk(v)) {
        ESP_LOGW(TAG, "reject bad action=%u", static_cast<unsigned>(v));
        return;
    }
    if (s_cells[cell] == v) {
        return;
    }
    s_cells[cell] = v;
    s_ready = true;
    (void)StartPersist();
}

void BookTapZonesResetDefaults() {
    BookTapZonesEnsureLoaded();
    if (std::memcmp(s_cells, kDefaultCells, sizeof(s_cells)) == 0) {
        return;
    }
    CopyDefault();
    s_ready = true;
    (void)StartPersist();
}

int BookTapZonesHit(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) {
        return -1;
    }
    if (x < 0) {
        x = 0;
    } else if (x >= w) {
        x = w - 1;
    }
    if (y < 0) {
        y = 0;
    } else if (y >= h) {
        y = h - 1;
    }
    const int col = static_cast<int>((static_cast<int64_t>(x) * 3) / w);
    const int row = static_cast<int>((static_cast<int64_t>(y) * 3) / h);
    const int c = col > 2 ? 2 : col;
    const int r = row > 2 ? 2 : row;
    return r * 3 + c;
}
