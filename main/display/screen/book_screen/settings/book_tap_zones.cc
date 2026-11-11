#include "book_screen/settings/book_tap_zones.h"

#include "settings.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <mutex>

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

static bool ParsePacked(const std::string& packed, uint8_t* out) {
    if (out == nullptr || packed.size() != static_cast<size_t>(kBookTapZoneCellCount)) {
        return false;
    }
    uint8_t tmp[kBookTapZoneCellCount];
    bool has_menu = false;
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
        has_menu |= v == static_cast<uint8_t>(BookTapZoneAction::kMenu);
    }
    if (!has_menu) {
        return false;
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
    }
    s_ready = true;
    ESP_LOGI(TAG, "nvs load tap_zones");
}

static std::mutex s_persist_mutex; // 保存任务与 UI 之间的配置交接
static uint8_t s_pending_cells[kBookTapZoneCellCount] = {}; // 仅保留最新待写配置
static bool s_persist_pending = false; // 是否存在尚未写入的快照
static bool s_persist_running = false; // 同时只运行一个保存任务

static void PersistTask(void*) {
    for (;;) {
        char packed[kBookTapZoneCellCount + 1] = {};
        {
            std::lock_guard<std::mutex> lock(s_persist_mutex);
            if (!s_persist_pending) {
                s_persist_running = false;
                break;
            }
            for (int i = 0; i < kBookTapZoneCellCount; ++i) {
                packed[i] = static_cast<char>('0' + s_pending_cells[i]);
            }
            s_persist_pending = false;
        }
        {
            Settings settings(kNvsNs, true);
            settings.SetString(kNvsKey, packed);
        }
    }
    vTaskDelete(nullptr);
}

static void StartPersist() {
    std::lock_guard<std::mutex> lock(s_persist_mutex);
    std::memcpy(s_pending_cells, s_cells, sizeof(s_cells));
    s_persist_pending = true;
    if (s_persist_running) return;
    s_persist_running = true;
    if (xTaskCreate(PersistTask, "tap_zones_nvs", 4096, nullptr, 5, nullptr) == pdPASS) return;
    s_persist_running = false;
    ESP_LOGE(TAG, "preference task creation failed");
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

bool BookTapZonesSetAction(int cell, BookTapZoneAction action) {
    BookTapZonesEnsureLoaded();
    if (cell < 0 || cell >= kBookTapZoneCellCount) {
        ESP_LOGW(TAG, "reject bad cell=%d", cell);
        return false;
    }
    const uint8_t v = static_cast<uint8_t>(action);
    if (!ActionOk(v)) {
        ESP_LOGW(TAG, "reject bad action=%u", static_cast<unsigned>(v));
        return false;
    }
    if (s_cells[cell] == v) {
        return true;
    }
    bool has_menu = false;
    for (int i = 0; i < kBookTapZoneCellCount; ++i) {
        has_menu |= (i == cell ? v : s_cells[i]) ==
                    static_cast<uint8_t>(BookTapZoneAction::kMenu);
    }
    if (!has_menu) {
        return false;
    }
    s_cells[cell] = v;
    StartPersist();
    return true;
}

bool BookTapZonesIsDefault() {
    BookTapZonesEnsureLoaded();
    return std::memcmp(s_cells, kDefaultCells, sizeof(s_cells)) == 0;
}

void BookTapZonesResetDefaults() {
    BookTapZonesEnsureLoaded();
    if (std::memcmp(s_cells, kDefaultCells, sizeof(s_cells)) == 0) {
        return;
    }
    CopyDefault();
    StartPersist();
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
