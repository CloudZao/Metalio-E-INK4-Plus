#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include "json_format.h"
#include "config.h"
#include "infra/diagnostics/firmware_memory.h"
#include "boards/common/storage/sd_access.h"
#include "infra/storage/file_write.h"
#include "infra/storage/key_bindings.h"
#include "infra/storage/sd_paths.h"

static constexpr const char *TAG = "KeyBindings";
static constexpr const char *kPath = SD_PATH_KEY_BINDINGS;
static constexpr unsigned kKeyCount = static_cast<unsigned>(BindingKey::Count);
static constexpr unsigned kActionCount = static_cast<unsigned>(BindingAction::Count);
static constexpr const char *kKeys[] = {"boot", "power", "volume_up", "volume_down", "previous", "next", "home"};
static constexpr const char *kActions[] = {
    "none",       "full_refresh", "back",     "home",       "volume_up",       "volume_down",
    "volume_max", "volume_min",   "light_up", "light_down", "standby",         "wake",
    "mute",       "light",        "open_ai",  "record",     "reader_settings", "interrupt",
    "previous",   "next",         "shutdown"};
static_assert(sizeof(kActions) / sizeof(kActions[0]) == kActionCount);
static_assert(kActionCount < 32);
struct KeyBindingConfig {
    BindingMask values[kKeyCount][2]; // 各按键的短按、长按功能掩码
    bool gestures_enabled = true; // 左右滑动识别开关
};
static constexpr BindingMask Bit(BindingAction action) {
    return BindingBit(action);
}
static constexpr KeyBindingConfig kDefaults{
    {{Bit(BindingAction::ReaderSettings) | Bit(BindingAction::Interrupt),
      Bit(BindingAction::OpenAi) | Bit(BindingAction::Record)},
     {Bit(BindingAction::Standby) | Bit(BindingAction::Wake), Bit(BindingAction::Shutdown)},
     {Bit(BindingAction::VolumeUp), Bit(BindingAction::VolumeMax)},
     {Bit(BindingAction::VolumeDown), Bit(BindingAction::VolumeMin)},
     {Bit(BindingAction::Back) | Bit(BindingAction::Previous), 0},
     {Bit(BindingAction::Next), 0},
     {Bit(BindingAction::Back), Bit(BindingAction::Home)}}};
static std::atomic<BindingMask> s_values[kKeyCount][2];
static std::atomic<bool> s_gestures_enabled{true};
static std::once_flag s_defaults_once;
static std::mutex s_write_mutex;
static unsigned s_generation = 0;
static bool s_loaded = false;

static void Publish(const KeyBindingConfig &config) {
    s_gestures_enabled.store(config.gestures_enabled, std::memory_order_release);
    for (unsigned key = 0; key < kKeyCount; ++key) {
        for (unsigned gesture = 0; gesture < 2; ++gesture) {
            s_values[key][gesture].store(config.values[key][gesture], std::memory_order_release);
        }
    }
}

static void EnsureDefaults() {
    std::call_once(s_defaults_once, [] { Publish(kDefaults); });
}

static KeyBindingConfig Snapshot() {
    EnsureDefaults();
    KeyBindingConfig config{};
    config.gestures_enabled = s_gestures_enabled.load(std::memory_order_acquire);
    for (unsigned key = 0; key < kKeyCount; ++key) {
        for (unsigned gesture = 0; gesture < 2; ++gesture) {
            config.values[key][gesture] = s_values[key][gesture].load(std::memory_order_acquire);
        }
    }
    return config;
}

static BindingMask ConflictingActions(BindingAction action) {
    const BindingMask bit = Bit(action);
    BindingMask compatible = bit;
    constexpr BindingMask pairs[] = {Bit(BindingAction::Standby) | Bit(BindingAction::Wake),
                                     Bit(BindingAction::OpenAi) | Bit(BindingAction::Record),
                                     Bit(BindingAction::ReaderSettings) | Bit(BindingAction::Interrupt)};
    for (BindingMask pair : pairs) {
        if (pair & bit) {
            compatible |= pair;
        }
    }
    const BindingMask group = (bit & kBindingLocalActions) ? kBindingLocalActions :
        ((BindingMask{1} << kActionCount) - 2) & ~kBindingLocalActions;
    return group & ~compatible;
}

static bool ValidateBinding(BindingKey key, bool long_press, BindingMask mask) {
    if (static_cast<unsigned>(key) >= kKeyCount || (mask & ~((BindingMask{1} << kActionCount) - 2)) != 0) {
        return false;
    }
    for (unsigned action = 1; action < kActionCount; ++action) {
        if ((mask & (BindingMask{1} << action)) &&
            (!KeyBindingsActionSupported(static_cast<BindingAction>(action)) ||
             (mask & ConflictingActions(static_cast<BindingAction>(action))))) {
            return false;
        }
    }
    if ((mask & Bit(BindingAction::Record)) && (!long_press || key == BindingKey::Power)) {
        return false;
    }
    return !(mask & Bit(BindingAction::Wake)) || (!long_press && key < BindingKey::Previous);
}

static bool Validate(const KeyBindingConfig &config) {
    bool wake = false;
    bool shutdown = false;
    for (unsigned key = 0; key < kKeyCount; ++key) {
        for (unsigned gesture = 0; gesture < 2; ++gesture) {
            const BindingMask mask = config.values[key][gesture];
            if (!ValidateBinding(static_cast<BindingKey>(key), gesture != 0, mask)) {
                return false;
            }
            wake |= (mask & Bit(BindingAction::Wake)) != 0;
            shutdown |= (mask & Bit(BindingAction::Shutdown)) != 0;
        }
    }
    return wake && shutdown;
}

static bool JsonShapeBounded(const char *text, size_t length) {
    unsigned depth = 0;
    unsigned separators = 0;
    bool quoted = false;
    bool escaped = false;
    for (size_t index = 0; index < length; ++index) {
        const char value = text[index];
        if (quoted) {
            if (escaped) {
                escaped = false;
            } else if (value == '\\') {
                escaped = true;
            } else if (value == '"') {
                quoted = false;
            }
            continue;
        }
        if (value == '"') {
            quoted = true;
        } else if (value == '{' || value == '[') {
            if (++depth > 3) {
                return false;
            }
        } else if (value == '}' || value == ']') {
            if (depth == 0) {
                return false;
            }
            --depth;
        } else if (value == ',' || value == ':') {
            if (++separators > 96) {
                return false;
            }
        }
    }
    return depth == 0 && !quoted;
}

static bool JsonMemoryAvailable() {
    #ifndef EBOOK_EMULATOR
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < kFirmwareInternalFloorBytes + 16 * 1024) {
        ESP_LOGE(TAG, "operation=config stage=json result=internal_budget");
        return false;
    }
    #endif
    return true;
}

static BindingSaveResult Save(const KeyBindingConfig &config) {
    SdAccessTransaction transaction;
    if (!transaction) {
        if (transaction.Error().code == StorageErrorCode::NotReady) {
            return BindingSaveResult::Volatile;
        }
        LogStorageError(TAG, transaction.Error());
        return BindingSaveResult::IoError;
    }
    if (!SdEnsureAppLayout() || !SdEnsureDirectory(SD_PATH_SETTINGS)) {
        ESP_LOGE(TAG, "operation=save stage=directory errno=%d", errno);
        return BindingSaveResult::IoError;
    }
    if (!JsonMemoryAvailable()) {
        return BindingSaveResult::IoError;
    }
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(cJSON_CreateObject(), cJSON_Delete);
    if (!json || !cJSON_AddNumberToObject(json.get(), "version", 1) ||
        !cJSON_AddBoolToObject(json.get(), "gestures_enabled", config.gestures_enabled)) {
        ESP_LOGE(TAG, "operation=save stage=root result=no_memory");
        return BindingSaveResult::IoError;
    }
    for (unsigned key = 0; key < kKeyCount; ++key) {
        cJSON *entry = cJSON_AddObjectToObject(json.get(), kKeys[key]);
        if (!entry) {
            ESP_LOGE(TAG, "operation=save stage=key result=no_memory");
            return BindingSaveResult::IoError;
        }
        for (unsigned gesture = 0; gesture < 2; ++gesture) {
            cJSON *actions = cJSON_AddArrayToObject(entry, gesture ? "long" : "short");
            if (!actions) {
                ESP_LOGE(TAG, "operation=save stage=gesture result=no_memory");
                return BindingSaveResult::IoError;
            }
            for (unsigned action = 1; action < kActionCount; ++action) {
                if ((config.values[key][gesture] & (BindingMask{1} << action)) == 0) {
                    continue;
                }
                cJSON *value = cJSON_CreateString(kActions[action]);
                if (!value || !cJSON_AddItemToArray(actions, value)) {
                    cJSON_Delete(value);
                    ESP_LOGE(TAG, "operation=save stage=action result=no_memory");
                    return BindingSaveResult::IoError;
                }
            }
        }
    }
    std::unique_ptr<char, decltype(&cJSON_free)> text(cJSON_PrintUnformatted(json.get()), cJSON_free);
    if (!text) {
        ESP_LOGE(TAG, "operation=save stage=json result=no_memory");
        return BindingSaveResult::IoError;
    }
    try {
        const std::string formatted = JsonFormatIndented(text.get());
        return WriteFileAtomic(kPath, reinterpret_cast<const uint8_t *>(formatted.data()), formatted.size())
                   ? BindingSaveResult::Saved
                   : BindingSaveResult::IoError;
    } catch (const std::bad_alloc &) {
        ESP_LOGE(TAG, "operation=save stage=format result=no_memory");
        return BindingSaveResult::IoError;
    }
}

BindingMask KeyBindingsCandidate(BindingMask current, BindingAction action) {
    if (action == BindingAction::None) {
        return 0;
    }
    if (static_cast<unsigned>(action) >= kActionCount) {
        return current;
    }
    const BindingMask bit = Bit(action);
    return (current & bit) ? current & ~bit : (current & ~ConflictingActions(action)) | bit;
}

BindingMask KeyBindingsGet(BindingKey key, bool long_press) {
    EnsureDefaults();
    if (static_cast<unsigned>(key) >= kKeyCount) {
        return 0;
    }
    return s_values[static_cast<unsigned>(key)][long_press].load(std::memory_order_acquire);
}

bool KeyBindingsCanBind(BindingKey key, bool long_press, BindingMask actions) {
    return ValidateBinding(key, long_press, actions);
}

bool KeyBindingsCanSet(BindingKey key, bool long_press, BindingMask actions) {
    if (static_cast<unsigned>(key) >= kKeyCount) {
        return false;
    }
    auto config = Snapshot();
    config.values[static_cast<unsigned>(key)][long_press] = actions;
    return Validate(config);
}

BindingSaveResult KeyBindingsSet(BindingKey key, bool long_press, BindingMask actions) {
    if (!KeyBindingsPollStorage()) {
        return BindingSaveResult::IoError;
    }
    std::lock_guard<std::mutex> lock(s_write_mutex);
    if (static_cast<unsigned>(key) >= kKeyCount) {
        return BindingSaveResult::Invalid;
    }
    auto config = Snapshot();
    config.values[static_cast<unsigned>(key)][long_press] = actions;
    if (!Validate(config)) {
        return BindingSaveResult::Invalid;
    }
    const auto result = Save(config);
    if (result == BindingSaveResult::Saved || result == BindingSaveResult::Volatile) {
        Publish(config);
        ESP_LOGI(TAG, "operation=assign key=%u long=%d stored=%d", unsigned(key), long_press,
                 result == BindingSaveResult::Saved);
    }
    return result;
}

BindingSaveResult KeyBindingsReset() {
    if (!KeyBindingsPollStorage()) {
        return BindingSaveResult::IoError;
    }
    std::lock_guard<std::mutex> lock(s_write_mutex);
    auto config = kDefaults;
    config.gestures_enabled = KeyBindingsGesturesEnabled();
    const auto result = Save(config);
    if (result == BindingSaveResult::Saved || result == BindingSaveResult::Volatile) {
        Publish(config);
        ESP_LOGI(TAG, "operation=reset stored=%d", result == BindingSaveResult::Saved);
    }
    return result;
}

bool KeyBindingsGesturesEnabled() {
    EnsureDefaults();
    return s_gestures_enabled.load(std::memory_order_acquire);
}

BindingSaveResult KeyBindingsSetGesturesEnabled(bool enabled) {
    if (!KeyBindingsPollStorage()) {
        return BindingSaveResult::IoError;
    }
    std::lock_guard<std::mutex> lock(s_write_mutex);
    auto config = Snapshot();
    config.gestures_enabled = enabled;
    const auto result = Save(config);
    if (result == BindingSaveResult::Saved || result == BindingSaveResult::Volatile) {
        Publish(config);
        ESP_LOGI(TAG, "operation=gestures enabled=%d stored=%d", enabled, result == BindingSaveResult::Saved);
    }
    return result;
}

bool KeyBindingsPollStorage() {
    EnsureDefaults();
    const unsigned generation = SdAccessMountGeneration();
    if (s_loaded && generation == s_generation && !SdAccessStatus()) {
        return true;
    }
    SdAccessTransaction transaction;
    if (!transaction) {
        if (transaction.Error().code == StorageErrorCode::NotReady && s_loaded) {
            std::lock_guard<std::mutex> lock(s_write_mutex);
            Publish(kDefaults);
            s_loaded = false;
        }
        return transaction.Error().code == StorageErrorCode::NotReady;
    }
    std::lock_guard<std::mutex> lock(s_write_mutex);
    s_generation = generation;
    s_loaded = true;
    FILE *file = std::fopen(kPath, "rb");
    bool recovered = false;
    if (!file && errno == ENOENT) {
        file = std::fopen((std::string(kPath) + ".backup").c_str(), "rb");
        recovered = file != nullptr;
    }
    if (!file) {
        if (errno == ENOENT) {
            Publish(kDefaults);
            if (Save(kDefaults) == BindingSaveResult::IoError) {
                ESP_LOGE(TAG, "operation=create_defaults result=write_failed");
            }
        } else {
            ESP_LOGE(TAG, "operation=load stage=open errno=%d; defaults used", errno);
            Publish(kDefaults);
        }
        return true;
    }
    std::unique_ptr<char, decltype(&heap_caps_free)> buffer(
        static_cast<char *>(heap_caps_malloc(4097, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), heap_caps_free);
    if (!buffer) {
        std::fclose(file);
        ESP_LOGE(TAG, "operation=load stage=buffer result=no_memory; defaults used");
        Publish(kDefaults);
        return true;
    }
    const size_t length = std::fread(buffer.get(), 1, 4096, file);
    const bool io_error = std::ferror(file) != 0;
    const int read_error = errno;
    const int extra = std::fgetc(file);
    const int close_result = std::fclose(file);
    if (io_error || close_result != 0) {
        ESP_LOGE(TAG, "operation=load stage=%s errno=%d; defaults used", io_error ? "read" : "close",
                 io_error ? read_error : errno);
        Publish(kDefaults);
        return true;
    }
    buffer.get()[length] = '\0';
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(extra == EOF && JsonShapeBounded(buffer.get(), length) &&
                                                                 JsonMemoryAvailable()
                                                             ? cJSON_ParseWithOpts(buffer.get(), nullptr, true)
                                                             : nullptr,
                                                         cJSON_Delete);
    KeyBindingConfig config{};
    const auto *version = cJSON_GetObjectItemCaseSensitive(json.get(), "version");
    bool valid = cJSON_IsObject(json.get()) && cJSON_IsNumber(version) && version->valuedouble == 1;
    const auto *gestures = cJSON_GetObjectItemCaseSensitive(json.get(), "gestures_enabled");
    if (gestures) {
        valid &= cJSON_IsBool(gestures);
        config.gestures_enabled = cJSON_IsTrue(gestures);
    }
    for (unsigned key = 0; valid && key < kKeyCount; ++key) {
        const auto *entry = cJSON_GetObjectItemCaseSensitive(json.get(), kKeys[key]);
        for (unsigned gesture = 0; valid && gesture < 2; ++gesture) {
            const auto *actions = cJSON_GetObjectItemCaseSensitive(entry, gesture ? "long" : "short");
            valid = cJSON_IsArray(actions) && cJSON_GetArraySize(actions) < static_cast<int>(kActionCount);
            const cJSON *value = nullptr;
            cJSON_ArrayForEach(value, actions) {
                bool found = false;
                for (unsigned action = 1; action < kActionCount; ++action) {
                    if (cJSON_IsString(value) && value->valuestring &&
                        std::strcmp(value->valuestring, kActions[action]) == 0) {
                        const BindingMask bit = BindingMask{1} << action;
                        if (config.values[key][gesture] & bit) {
                            break;
                        }
                        config.values[key][gesture] |= bit;
                        found = true;
                        break;
                    }
                }
                valid &= found;
            }
        }
    }
    if (!valid || !Validate(config)) {
        ESP_LOGE(TAG, "operation=load stage=validate result=invalid; defaults used, file retained");
        Publish(kDefaults);
        return true;
    }
    if (recovered && Save(config) != BindingSaveResult::Saved) {
        ESP_LOGW(TAG, "operation=load stage=recover_backup result=not_committed");
    }
    Publish(config);
    ESP_LOGI(TAG, "operation=load result=ok version=1");
    return true;
}

static bool KeyBindingsHasStandbyAction(BindingKey key, bool long_press, BindingAction action) {
    return static_cast<unsigned>(key) < static_cast<unsigned>(BindingKey::Previous) &&
           (KeyBindingsGet(key, long_press) & Bit(action)) != 0;
}

bool KeyBindingsNeedsWake(BindingKey key) {
    return KeyBindingsHasStandbyAction(key, false, BindingAction::Wake) ||
           KeyBindingsHasStandbyAction(key, false, BindingAction::Shutdown) ||
           KeyBindingsHasStandbyAction(key, true, BindingAction::Shutdown);
}

bool KeyBindingsActionSupported(BindingAction action) {
    if (action == BindingAction::Light || action == BindingAction::LightUp || action == BindingAction::LightDown) {
        return BOARD_HAS_FRONTLIGHT;
    }
    return static_cast<unsigned>(action) < kActionCount;
}

BindingAction KeyBindingsStandbyAction(BindingKey key, bool long_press) {
    if (KeyBindingsHasStandbyAction(key, long_press, BindingAction::Shutdown)) {
        return BindingAction::Shutdown;
    }
    if (!long_press && KeyBindingsHasStandbyAction(key, false, BindingAction::Wake)) {
        return BindingAction::Wake;
    }
    return BindingAction::None;
}
