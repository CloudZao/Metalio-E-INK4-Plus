#include "wallpaper_screen/wallpaper_active.h"
#include "wallpaper_screen/wallpaper_screen_priv.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "assets/lang_config.h"
#include "SdCardManager.hpp"
#include "screen_common.h"
#include "sd_paths.h"
#include "settings.h"


static constexpr const char* kLogTag = "WpActive";
static constexpr const char* kNvsNs = "wallpaper";
static constexpr const char* kNvsKeyShutdown = "shutdown";
static constexpr const char* kNvsKeyStandby = "standby";
static constexpr size_t kNameMax = 95;

enum class WallpaperActiveSlot : uint8_t { Shutdown = 0, Standby = 1, Count = 2 };

static char s_names[static_cast<size_t>(WallpaperActiveSlot::Count)][kNameMax + 1] = {};
static std::atomic<bool> s_cache_ready{false};
static std::atomic<bool> s_hydrate_busy{false};

struct WallpaperActiveHydrateWork {
    Wallpaper_HydrateDoneFn done = nullptr;
    void* user = nullptr;
};

static const char* NvsKeyFor(WallpaperActiveSlot slot) {
    return slot == WallpaperActiveSlot::Standby ? kNvsKeyStandby : kNvsKeyShutdown;
}
static const char* SlotTag(WallpaperActiveSlot slot) {
    return slot == WallpaperActiveSlot::Standby ? "standby" : "shutdown";
}

static bool FileExistsRegular(const char* path) {
    struct stat st {};
    return path != nullptr && stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static std::string AbsPathForName(const std::string& name) {
    std::string path = SD_PATH_WALLPAPER;
    if (!path.empty() && path.back() != '/') {
        path.push_back('/');
    }
    path.append(name);
    return path;
}

// 仅允许单段、无路径穿越的文件名。
static bool IsSafeWallpaperBasename(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    const size_t n = std::strlen(name);
    if (n > kNameMax) {
        return false;
    }
    if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
        if (c == '/' || c == '\\' || c == ':' || c == '\0') {
            return false;
        }
    }
    return true;
}

static std::string BasenameOf(const char* path_or_name) {
    if (path_or_name == nullptr || path_or_name[0] == '\0') {
        return {};
    }
    const char* slash = std::strrchr(path_or_name, '/');
    const char* base = slash != nullptr ? slash + 1 : path_or_name;
    if (!IsSafeWallpaperBasename(base)) {
        return {};
    }
    return base;
}

static void SetSlotCache(WallpaperActiveSlot slot, const char* name) {
    const size_t i = static_cast<size_t>(slot);
    if (name == nullptr || name[0] == '\0') {
        s_names[i][0] = '\0';
    } else {
        std::snprintf(s_names[i], sizeof(s_names[i]), "%s", name);
    }
}

static void MarkCacheReady() {
    s_cache_ready.store(true, std::memory_order_release);
}

static void PersistSlotToNvs(WallpaperActiveSlot slot, const char* name) {
    Settings settings(kNvsNs, true);
    const char* key = NvsKeyFor(slot);
    if (name == nullptr || name[0] == '\0') {
        settings.EraseKey(key);
    } else {
        settings.SetString(key, name);
    }
}

// 读取 NVS 值并做合法性校验；非法值返回空并要求清理。
static std::string ReadSlotNameFromNvs(WallpaperActiveSlot slot, bool* should_erase) {
    if (should_erase != nullptr) {
        *should_erase = false;
    }
    Settings settings(kNvsNs, false);
    const std::string name = settings.GetString(NvsKeyFor(slot), "");
    if (name.empty()) {
        return {};
    }
    if (!IsSafeWallpaperBasename(name.c_str())) {
        if (should_erase != nullptr) {
            *should_erase = true;
        }
        return {};
    }
    return name;
}

static void LoadAllFromNvsIntoCache() {
    // 先灌入两槽；若 SD 未挂载则延迟标记缓存已就绪。
    bool defer_ready = false;
    for (uint8_t s = 0; s < static_cast<uint8_t>(WallpaperActiveSlot::Count); ++s) {
        const WallpaperActiveSlot slot = static_cast<WallpaperActiveSlot>(s);
        bool erase = false;
        const std::string name = ReadSlotNameFromNvs(slot, &erase);
        if (erase) {
            PersistSlotToNvs(slot, nullptr);
            SetSlotCache(slot, "");
            continue;
        }
        if (name.empty()) {
            SetSlotCache(slot, "");
            continue;
        }
        const std::string path = AbsPathForName(name);
        if (FileExistsRegular(path.c_str())) {
            SetSlotCache(slot, name.c_str());
            ESP_LOGI(kLogTag, "hydrate %s=%s", SlotTag(slot), name.c_str());
            continue;
        }
        if (SdCardManager::GetInstance().IsMounted()) {
            ESP_LOGW(kLogTag, "hydrate %s NVS=%s missing on SD → clear", SlotTag(slot), name.c_str());
            PersistSlotToNvs(slot, nullptr);
            SetSlotCache(slot, "");
        } else {
            SetSlotCache(slot, "");
            defer_ready = true;
            ESP_LOGW(kLogTag, "hydrate SD unmounted, keep NVS %s=%s", SlotTag(slot), name.c_str());
        }
    }
    if (!defer_ready) {
        MarkCacheReady();
    }
}

static void InvokeDoneAsync(Wallpaper_HydrateDoneFn done, void* user) {
    if (done == nullptr) {
        return;
    }
    auto* cb = new WallpaperActiveHydrateWork{done, user};
    if (!ScreenLvAsync(
            [](void* p) {
                auto* c = static_cast<WallpaperActiveHydrateWork*>(p);
                if (c->done != nullptr) {
                    c->done(c->user);
                }
                delete c;
            },
            cb)) {
        delete cb;
    }
}

static void HydrateTask(void* arg) {
    auto* work = static_cast<WallpaperActiveHydrateWork*>(arg);
    LoadAllFromNvsIntoCache();
    const Wallpaper_HydrateDoneFn done = work != nullptr ? work->done : nullptr;
    void* user = work != nullptr ? work->user : nullptr;
    delete work;
    s_hydrate_busy.store(false, std::memory_order_release);
    InvokeDoneAsync(done, user);
    vTaskDelete(nullptr);
}

static void WaitThenDoneTask(void* arg) {
    auto* work = static_cast<WallpaperActiveHydrateWork*>(arg);
    for (int i = 0; i < 200 && !s_cache_ready.load(std::memory_order_acquire); ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    const Wallpaper_HydrateDoneFn done = work != nullptr ? work->done : nullptr;
    void* user = work != nullptr ? work->user : nullptr;
    delete work;
    InvokeDoneAsync(done, user);
    vTaskDelete(nullptr);
}

static std::string GetSlotFilename(WallpaperActiveSlot slot) {
    if (!s_cache_ready.load(std::memory_order_acquire)) {
        return {};
    }
    const char* name = s_names[static_cast<size_t>(slot)];
    if (name[0] == '\0') {
        return {};
    }
    // 文件可能已被删；此处只返回空，不直接清理 NVS。
    const std::string path = AbsPathForName(name);
    if (!FileExistsRegular(path.c_str())) {
        return {};
    }
    return name;
}

static bool SetSlotFromFile(WallpaperActiveSlot slot, const char* path_or_name, std::string& err_out) {
    err_out.clear();
    const std::string name = BasenameOf(path_or_name);
    if (name.empty()) {
        err_out = Lang::Strings::WALLPAPER_NAME_INVALID;
        return false;
    }

    const std::string src = AbsPathForName(name);
    const std::string prefix = std::string(SD_PATH_WALLPAPER) + "/";
    if (src.rfind(prefix, 0) != 0) {
        err_out = Lang::Strings::WALLPAPER_PATH_INVALID;
        return false;
    }
    if (!FileExistsRegular(src.c_str())) {
        err_out = Lang::Strings::WALLPAPER_FILE_MISSING;
        return false;
    }

    // 若尚未 hydrate，先灌入两槽再更新当前槽，避免覆盖另一槽状态。
    if (!s_cache_ready.load(std::memory_order_acquire)) {
        LoadAllFromNvsIntoCache();
    }
    SetSlotCache(slot, name.c_str());
    MarkCacheReady();
    PersistSlotToNvs(slot, name.c_str());
    ESP_LOGI(kLogTag, "%s wallpaper -> %s (NVS only)", SlotTag(slot), name.c_str());
    return true;
}

static void ClearSlot(WallpaperActiveSlot slot) {
    if (!s_cache_ready.load(std::memory_order_acquire)) {
        LoadAllFromNvsIntoCache();
    }
    SetSlotCache(slot, "");
    MarkCacheReady();
    PersistSlotToNvs(slot, nullptr);
    ESP_LOGI(kLogTag, "cleared %s wallpaper (user)", SlotTag(slot));
}

static void ClearSlotIfMatches(WallpaperActiveSlot slot, const char* filename, bool* any_cleared) {
    if (filename == nullptr || filename[0] == '\0') {
        return;
    }

    bool match = false;
    if (s_cache_ready.load(std::memory_order_acquire)) {
        match = (std::strcmp(s_names[static_cast<size_t>(slot)], filename) == 0);
    }
    // 再对一下 NVS 原文，避免只清缓存漏清 NVS
    {
        Settings settings(kNvsNs, false);
        if (settings.GetString(NvsKeyFor(slot), "") == filename) {
            match = true;
        }
    }
    if (!match) {
        return;
    }

    SetSlotCache(slot, "");
    PersistSlotToNvs(slot, nullptr);
    ESP_LOGI(kLogTag, "cleared %s wallpaper (%s)", SlotTag(slot), filename);
    if (any_cleared != nullptr) {
        *any_cleared = true;
    }
}

static bool TryResolveSlotPath(WallpaperActiveSlot slot, char* out_path, size_t out_len) {
    if (out_path == nullptr || out_len < 8) {
        return false;
    }
    out_path[0] = '\0';

    std::string name;
    const char* cached = s_names[static_cast<size_t>(slot)];
    if (s_cache_ready.load(std::memory_order_acquire) && cached[0] != '\0' &&
        IsSafeWallpaperBasename(cached)) {
        name = cached;
    } else {
        bool erase = false;
        name = ReadSlotNameFromNvs(slot, &erase);
        // 解析路径：不在这里 erase（瞬时失败不应丢配置）；仅 hydrate 清脏
        (void)erase;
    }
    if (name.empty() || !IsSafeWallpaperBasename(name.c_str())) {
        return false;
    }

    const std::string path = AbsPathForName(name);
    if (path.size() + 1 > out_len) {
        ESP_LOGW(kLogTag, "%s path too long", SlotTag(slot));
        return false;
    }
    if (!FileExistsRegular(path.c_str())) {
        ESP_LOGW(kLogTag, "%s path missing: %s", SlotTag(slot), path.c_str());
        return false;
    }
    std::snprintf(out_path, out_len, "%s", path.c_str());
    return true;
}


std::string Wallpaper_GetActiveFilename() {
    return GetSlotFilename(WallpaperActiveSlot::Shutdown);
}

std::string Wallpaper_GetStandbyFilename() {
    return GetSlotFilename(WallpaperActiveSlot::Standby);
}

bool Wallpaper_IsActiveFilename(const char* filename) {
    if (filename == nullptr || filename[0] == '\0') {
        return false;
    }
    const std::string active = Wallpaper_GetActiveFilename();
    return !active.empty() && active == filename;
}

bool Wallpaper_IsStandbyFilename(const char* filename) {
    if (filename == nullptr || filename[0] == '\0') {
        return false;
    }
    const std::string active = Wallpaper_GetStandbyFilename();
    return !active.empty() && active == filename;
}

bool Wallpaper_SetActiveFromFile(const char* path_or_name, std::string& err_out) {
    return SetSlotFromFile(WallpaperActiveSlot::Shutdown, path_or_name, err_out);
}

bool Wallpaper_SetStandbyFromFile(const char* path_or_name, std::string& err_out) {
    return SetSlotFromFile(WallpaperActiveSlot::Standby, path_or_name, err_out);
}

void Wallpaper_ClearShutdownWallpaper() {
    ClearSlot(WallpaperActiveSlot::Shutdown);
}

void Wallpaper_ClearStandbyWallpaper() {
    ClearSlot(WallpaperActiveSlot::Standby);
}

void Wallpaper_ClearActiveIfMatches(const char* filename) {
    // 删除文件后，同步清理所有指向它的启用项。
    const bool was_ready = s_cache_ready.load(std::memory_order_acquire);
    bool any = false;
    ClearSlotIfMatches(WallpaperActiveSlot::Shutdown, filename, &any);
    ClearSlotIfMatches(WallpaperActiveSlot::Standby, filename, &any);
    // 未 hydrate 时不要标记 ready，以免把另一槽状态覆盖为空。
    if (any && was_ready) {
        MarkCacheReady();
    }
}

bool Wallpaper_TryResolveActiveWallpaperPath(char* out_path, size_t out_len) {
    return TryResolveSlotPath(WallpaperActiveSlot::Shutdown, out_path, out_len);
}

bool Wallpaper_TryResolveStandbyWallpaperPath(char* out_path, size_t out_len) {
    return TryResolveSlotPath(WallpaperActiveSlot::Standby, out_path, out_len);
}

void Wallpaper_RequestHydrateFromNvs(Wallpaper_HydrateDoneFn done, void* user) {
    if (s_cache_ready.load(std::memory_order_acquire)) {
        InvokeDoneAsync(done, user);
        return;
    }

    auto* work = new WallpaperActiveHydrateWork{done, user};
    if (s_hydrate_busy.exchange(true, std::memory_order_acq_rel)) {
        if (xTaskCreate(WaitThenDoneTask, "wp_hyd_wait", 3072, work, 5, nullptr) != pdPASS) {
            delete work;
            InvokeDoneAsync(done, user);
        }
        return;
    }

    if (xTaskCreate(HydrateTask, "wp_hydrate", 4096, work, 5, nullptr) != pdPASS) {
        delete work;
        s_hydrate_busy.store(false, std::memory_order_release);
        ESP_LOGE(kLogTag, "hydrate task create failed");
        InvokeDoneAsync(done, user);
    }
}

void Wallpaper_HydrateFromNvsNow() {
    if (s_cache_ready.load(std::memory_order_acquire)) {
        return;
    }
    if (s_hydrate_busy.exchange(true, std::memory_order_acq_rel)) {
        for (int i = 0; i < 200 && !s_cache_ready.load(std::memory_order_acquire); ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        return;
    }
    LoadAllFromNvsIntoCache();
    s_hydrate_busy.store(false, std::memory_order_release);
}

