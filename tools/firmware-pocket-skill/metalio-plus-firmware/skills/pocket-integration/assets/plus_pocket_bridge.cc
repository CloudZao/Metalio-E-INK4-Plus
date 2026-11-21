#include "plus_pocket_bridge.h"
#include "plus_hal.h"
#include "pocket/pocket_boot_layout.h"
#include "pocket/pocket_handoff.h"
#include "pocket/pocket_hash.h"
#include "pocket/pocket_image.h"
#include "pocket_build.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <ff.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mutex>

namespace plus_pocket {
namespace {
static_assert(POCKET_STORAGE_MODE >= 0 && POCKET_STORAGE_MODE <= 2, "Unsupported storage mode");
Hooks hooks{};
#ifndef POCKET_TF_MOUNT
#define POCKET_TF_MOUNT "/sdcard"
#endif
constexpr char root[] = POCKET_TF_MOUNT "/firmware-pocket";
constexpr char status_dir[] = POCKET_TF_MOUNT "/firmware-pocket/status";
std::mutex gate;
Error last_error = Error::None;
bool initialized = false, confirmed = false, integrated = false, return_uncertain = false,
     control_invalid = false;
std::string user_id;
pocket::RunContext run;
pocket::AppBootLayout layout = pocket::AppBootLayout::Invalid;
bool Fail(std::string *error, const std::string &message, Error code = Error::Corrupt) {
    last_error = code;
    if (error)
        *error = message;
    return false;
}
void Clear(std::string *error) {
    last_error = Error::None;
    if (error)
        error->clear();
}
Error RecordError(pocket::RecordStatus s) {
    return s == pocket::RecordStatus::IoError             ? Error::IoError
           : s == pocket::RecordStatus::UnsupportedSchema ? Error::Unsupported
           : s == pocket::RecordStatus::Missing           ? Error::Unavailable
                                                          : Error::Corrupt;
}
class FlashPause {
    bool paused_;

  public:
    FlashPause() : paused_(plus_hal::PauseForFlash(30000)) {}
    ~FlashPause() { Release(); }
    void Release() {
        if (paused_) {
            plus_hal::ResumeAfterFlash();
            paused_ = false;
        }
    }
    bool ok() const { return paused_; }
};
bool Mounted() {
    FATFS *fs = nullptr;
    DWORD clusters = 0;
    return f_getfree("0:", &clusters, &fs) == FR_OK && fs;
}
pocket::AppBootLayout Classify() {
    auto get = [](esp_partition_type_t type, esp_partition_subtype_t subtype, const char *name) {
        const auto *p = esp_partition_find_first(type, subtype, name);
        return p ? pocket::BootPartition{p->address, p->size} : pocket::BootPartition{};
    };
    const pocket::AppBootPartitions p{
        get(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs"),
        get(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, "otadata"),
        get(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_PHY, "phy_init"),
        get(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr),
        get(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr),
        get(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr)};
    const auto *running = esp_ota_get_running_partition();
    return running ? pocket::ClassifyAppBootLayout(p, running->subtype, running->address)
                   : pocket::AppBootLayout::Invalid;
}
bool ReadPartition(const esp_partition_t *partition, pocket::CompactImageInfo *result, std::string *error) {
    if (!partition)
        return Fail(error, "运行分区不存在", Error::Unsupported);
    struct Reader {
        const esp_partition_t *partition;
        bool io_error = false;
    } reader{partition};
    auto read = [](void *arg, uint32_t offset, void *bytes, size_t length) {
        auto *r = static_cast<Reader *>(arg);
        if (esp_partition_read(r->partition, offset, bytes, length) == ESP_OK)
            return true;
        r->io_error = true;
        return false;
    };
    std::string why;
    if (!pocket::ValidateRunningImage(read, &reader, partition->size, result, &why))
        return Fail(error, why, reader.io_error ? Error::IoError : Error::Corrupt);
    return true;
}
const esp_partition_t *Manager(std::string *error) {
    if (layout != pocket::AppBootLayout::PocketSlots) {
        Fail(error, "不是口袋布局", Error::Unsupported);
        return nullptr;
    }
    const auto *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    if (!p || p->address != pocket::kManagerAddress || p->size != pocket::kManagerSize) {
        Fail(error, "管理器分区不匹配", Error::Unsupported);
        return nullptr;
    }
    pocket::CompactImageInfo image;
    // The standard reserved identity is mandatory; product version is not.
    // ValidateRunningImage compares real ESP project/version to embedded metadata.
    if (!ReadPartition(p, &image, error))
        return nullptr;
    if (!image.manager || image.manifest.id != "pocket_manager") {
        Fail(error, "返回目标不是标准管理器");
        return nullptr;
    }
    return p;
}
bool ReadIdentity(std::string *id, std::string *error) {
    const auto record = pocket::RecordStore(std::string(root) + "/shared").Load("identity", 1);
    if (!record.ok())
        return Fail(error, record.error, RecordError(record.status));
    if (!pocket::DecodeIdentity(record.payload, id))
        return Fail(error, "公共身份格式损坏");
    return true;
}
bool CheckControl(std::string *error) {
    control_invalid = true;
    if (!Mounted())
        return Fail(error, "TF 卡不可用，请恢复原卡后重试", Error::Unavailable);
    std::string identity;
    if (!ReadIdentity(&identity, error))
        return false;
    if (identity != user_id)
        return Fail(error, "TF 用户身份改变，请恢复原卡");
    const auto current = pocket::RecordStore(status_dir).Load("run", 1);
    pocket::RunContext next;
    if (!current.ok())
        return Fail(error, current.error, RecordError(current.status));
    if (!pocket::DecodeRun(current.payload, &next) || pocket::EncodeRun(next) != pocket::EncodeRun(run))
        return Fail(error, "运行交接改变，停止编辑并保留原记录");
    control_invalid = false;
    return true;
}
bool WithdrawSaved(const std::string &reason, std::string *error) {
    std::string ignored;
    return_uncertain = true;
    if (CheckControl(&ignored) && pocket::WriteHandoffReceipt(status_dir, run, false, &ignored))
        return_uncertain = false;
    return Fail(error,
                reason +
                    (return_uncertain ? "；交接未确认，请保留当前页面并重试返回" : "；仍停留在应用，可重试"),
                Error::ReturnFailed);
}
} // namespace

bool Initialize(const Hooks &supplied, std::string *error) {
    std::lock_guard<std::mutex> lock(gate);
    Clear(error);
    initialized = confirmed = integrated = return_uncertain = control_invalid = false;
    hooks = supplied;
    if (POCKET_STORAGE_MODE != 0 && (!hooks.restore || !hooks.save))
        return Fail(error, "存储模式缺少真实restore/save实现", Error::InvalidArgument);
    FlashPause pause;
    if (!pause.ok())
        return Fail(error, "屏幕暂停失败，尚未初始化交接，请重试", Error::Unavailable);
    layout = Classify();
    integrated = layout == pocket::AppBootLayout::PocketSlots;
    if (layout == pocket::AppBootLayout::Invalid)
        return Fail(error, "固件分区布局不匹配，禁止返回切换", Error::Unsupported);
    pocket::CompactImageInfo actual;
    if (!ReadPartition(esp_ota_get_running_partition(), &actual, error))
        return false;
    const auto &m = actual.manifest;
    if (actual.manager || m.id != POCKET_APP_ID || m.version != POCKET_VERSION ||
        m.storage_mode != POCKET_STORAGE_MODE || m.state_version != POCKET_STATE_SCHEMA ||
        m.exit_method != POCKET_EXIT_METHOD ||
        m.tf_required_standalone != bool(POCKET_TF_REQUIRED_STANDALONE))
        return Fail(error, "实际应用声明与编译配置不一致");
    if (!integrated) {
        user_id.clear();
        run = {};
        if (POCKET_TF_REQUIRED_STANDALONE && !Mounted())
            return Fail(error, "独立模式需要TF卡", Error::Unavailable);
        pause.Release(); // Business Flash writes acquire their own lease.
        if (hooks.restore && !hooks.restore(nullptr, error)) {
            last_error = Error::Corrupt;
            return false;
        }
        initialized = true;
        ESP_LOGI("PlusBridge", "RESTORE_OK mode=standalone");
        return true;
    }
    if (!Manager(error))
        return false;
    if (!Mounted())
        return Fail(error, "集成模式需要原TF卡", Error::Unavailable);
    std::string identity;
    if (!ReadIdentity(&identity, error))
        return false;
    if (!user_id.empty() && identity != user_id)
        return Fail(error, "本次运行的 TF 用户身份改变，请恢复原卡");
    const auto record = pocket::RecordStore(status_dir).Load("run", 1);
    pocket::RunContext next;
    if (!record.ok())
        return Fail(error, record.error, RecordError(record.status));
    if (!pocket::DecodeRun(record.payload, &next) || next.phase != "active" || next.user_id != identity ||
        next.app_id != POCKET_APP_ID || next.version != POCKET_VERSION || next.image_sha256 != m.sha256)
        return Fail(error, "运行交接与应用不匹配，未写入保存回执");
    if (!run.run_id.empty() && run.run_id != next.run_id)
        return Fail(error, "本次运行交接身份改变，请重新启动");
    next.image_bytes = m.size;
    next.state_schema = m.state_version;
    next.storage_mode = m.storage_mode;
    next.app_dir = std::string(root) + "/users/" + next.user_id + "/apps/" + next.app_id;
    pause.Release(); // Keep EPD/Flash lock ownership out of business callbacks.
    if (hooks.restore && !hooks.restore(&next, error)) {
        last_error = Error::Corrupt;
        return false;
    }
    run = std::move(next);
    user_id = std::move(identity);
    initialized = true;
    ESP_LOGI("PlusBridge", "RESTORE_OK mode=pocket storage=%u", unsigned(POCKET_STORAGE_MODE));
    return true;
}

bool ConfirmStartup(uint32_t first_frame, std::string *error) {
    std::lock_guard<std::mutex> lock(gate);
    Clear(error);
    if (!initialized)
        return Fail(error, "应用尚未初始化，不能确认启动");
    if (!first_frame || !plus_hal::DisplayHealthy() || plus_hal::DisplayedRevision() < first_frame)
        return Fail(error, "首个业务画面尚未物理完成，不能确认启动");
    if (!integrated) {
        confirmed = true;
        ESP_LOGI("PlusBridge", "STARTUP_CONFIRMED mode=standalone");
        return true;
    }
    FlashPause pause;
    if (!pause.ok())
        return Fail(error, "屏幕暂停失败，尚未确认启动");
    if (!CheckControl(error))
        return false;
    const auto *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK ||
        (state != ESP_OTA_IMG_VALID && state != ESP_OTA_IMG_PENDING_VERIFY))
        return Fail(error, "应用启动状态异常，尚未确认");
    if (state == ESP_OTA_IMG_PENDING_VERIFY && esp_ota_mark_app_valid_cancel_rollback() != ESP_OK)
        return Fail(error, "启动确认失败，请重新进入应用");
    if (!pocket::WriteHandoffReceipt(status_dir, run, false, error)) {
        last_error = Error::IoError;
        return false;
    }
    confirmed = true;
    ESP_LOGI("PlusBridge", "STARTUP_CONFIRMED mode=pocket receipt=started");
    return true;
}
bool Save(std::string *error) {
    std::lock_guard<std::mutex> lock(gate);
    Clear(error);
    if (!initialized || !confirmed)
        return Fail(error, "首屏尚未确认，请稍后重试");
    if (return_uncertain)
        return Fail(error, "返回交接尚未确认，请重试返回口袋");
    if (integrated && !CheckControl(error))
        return false;
    // Called without an EPD lease: save hooks writing internal Flash must use
    // PauseForFlash/ResumeAfterFlash. TF-only hooks need no Flash lease.
    if (hooks.save && !hooks.save(SaveReason::Checkpoint, integrated ? &run : nullptr, error)) {
        last_error = Error::SaveFailed;
        return false;
    }
    ESP_LOGI("PlusBridge", "BUSINESS_SAVE_OK storage=%u", unsigned(POCKET_STORAGE_MODE));
    return true;
}
bool ReturnToPocket(std::string *error) {
    std::lock_guard<std::mutex> lock(gate);
    Clear(error);
    if (!initialized || !confirmed)
        return Fail(error, "应用尚未确认启动，不能切换");
    if (!integrated) {
        if (hooks.save && !hooks.save(SaveReason::Exit, nullptr, error)) {
            last_error = Error::SaveFailed;
            return false;
        }
        return true; // Saved locally, no pocket writes or reboot.
    }
    if (!CheckControl(error))
        return false;
    if (hooks.save && !hooks.save(SaveReason::Exit, &run, error)) {
        last_error = Error::SaveFailed;
        return false;
    }
    FlashPause pause;
    if (!pause.ok())
        return Fail(error, "屏幕暂停失败，尚未返回口袋");
    if (!CheckControl(error))
        return false;
    if (return_uncertain) {
        if (!pocket::WriteHandoffReceipt(status_dir, run, false, error)) {
            last_error = Error::IoError;
            return false;
        }
        return_uncertain = false;
    }
    // Business save succeeded; NONE has no checkpoint placeholder.
    // Recheck identity/control immediately before writing the saved receipt.
    if (!pocket::WriteHandoffReceipt(status_dir, run, true, error))
        return WithdrawSaved("返回回执写入失败", error);
    return_uncertain = true;
    const auto *manager = Manager(error);
    if (!manager)
        return WithdrawSaved("返回目标不可确认", error);
    if (esp_ota_set_boot_partition(manager) != ESP_OK)
        return WithdrawSaved("口袋启动切换失败", error);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return true;
}
Error LastError() {
    std::lock_guard<std::mutex> lock(gate);
    return last_error;
}
const char *ErrorMessage(Error error) {
    switch (error) {
    case Error::None:
        return "";
    case Error::InvalidArgument:
        return "应用初始化参数不完整";
    case Error::Unsupported:
        return "布局或接口版本不支持";
    case Error::Unavailable:
        return "必要设备或记录暂不可用";
    case Error::Corrupt:
        return "镜像或交接数据校验失败";
    case Error::IoError:
        return "读取或写入失败，请保留原资料";
    case Error::SaveFailed:
        return "业务保存失败，请重试";
    case Error::ReturnFailed:
        return "返回失败，请保持暂停并重试";
    }
    return "未知错误";
}
bool CanEdit() {
    std::lock_guard<std::mutex> lock(gate);
    return initialized && confirmed && !return_uncertain && !control_invalid;
}
bool Integrated() {
    std::lock_guard<std::mutex> lock(gate);
    return integrated;
}
bool CanReturn() {
    std::lock_guard<std::mutex> lock(gate);
    return initialized && integrated && !control_invalid;
}
} // namespace plus_pocket
