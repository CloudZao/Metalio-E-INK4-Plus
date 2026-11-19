#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <utility>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include "settings_test_aging_sd.h"
#include "SdCardManager.hpp"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "epd_content_policy.h"
#include "fontpack_lvgl.h"
#include "reader/image_util.h"
#include "screen_common.h"
#include "sd_access.h"
#include "sd_paths.h"
#include "ui_scale.h"
#include "usb_virtual_disk.h"

static constexpr const char* TAG = "AgingSd";
enum class AgingSdStatus {
    kReady, // 图片读取成功
    kEmpty, // 没有受支持的壁纸文件
    kNotReady, // 尚未挂载且没有初始化错误
    kInitTimeout, // 初始化超时，不据此推断物理插卡状态
    kMountFailed, // 底层返回分区挂载失败
    kInitNoMemory, // 初始化资源不足，区别于壁纸解码内存不足
    kInitFailed, // 其他初始化错误
    kUsbExported, // 文件系统由电脑占用
    kBusy, // 介质切换、待机或事务资源暂不可用
    kDirectoryMissing, // 壁纸目录不存在
    kDirectoryOpenFailed, // 壁纸目录打开失败
    kDirectoryReadFailed, // 遍历目录失败
    kFileStatFailed, // 文件信息读取失败
    kFileReadFailed, // 文件内容读取失败
    kImageInvalid, // 图片损坏或不完整
    kImageUnsupported, // 图片格式、编码或尺寸不支持
    kDecodeFailed, // 解码失败，具体错误保留在日志
    kNoMemory, // 解码内存不足
    kTaskFailed, // 读取任务创建失败
    kCancelled, // 页面退出或重建，正常取消
};

struct AgingSdWork {
    uint32_t generation; // 预览区代次
    uint32_t session; // 老化循环代次
    int side; // 本次解码尺寸上限
    void (*done)(uint32_t); // 界面回收结果后的调度钩子
    reader::RasterImage image; // 工作任务独占，结果交给界面
    AgingSdStatus status = AgingSdStatus::kEmpty; // 本轮结果
    const char* stage = "access"; // 固定阶段名，不包含文件路径
    esp_err_t init_error = ESP_OK; // SD 初始化原始错误码
    int system_error = 0; // 失败操作的 errno
    reader::ImageDecodeError decode_error = reader::ImageDecodeError::kNone; // 解码原始错误码
};

static std::atomic<uint32_t> s_generation{0}; // 退出或重建后作废旧结果
static std::atomic<unsigned> s_reading{0}; // SD 事务在途数量，自动重启前等待归零
static bool s_stopping = true; // 界面线程控制是否接收本轮读取
static lv_obj_t* s_image = nullptr;
static lv_obj_t* s_notice = nullptr;
static reader::RasterImage s_pixels; // 仅持有当前显示图片，不供下一轮复用
static int s_image_side = 0;

static bool AgingSdCancelled(void* context) {
    return static_cast<AgingSdWork*>(context)->generation != s_generation.load() || sd_access::IsQuiescing();
}

static bool AgingSdImageName(const char* name) {
    if (name[0] == '.' || !SdIsValidFileName(name)) return false;
    const char* extension = std::strrchr(name, '.');
    return extension && (!strcasecmp(extension, ".png") || !strcasecmp(extension, ".jpg") ||
        !strcasecmp(extension, ".jpeg") || !strcasecmp(extension, ".a2i1") ||
        !strcasecmp(extension, ".a2l8") || !strcasecmp(extension, ".ebgr"));
}

static AgingSdStatus AgingSdAvailability(esp_err_t* init_error = nullptr) {
    if (init_error) *init_error = ESP_OK;
    auto& usb = UsbVirtualDisk::GetInstance();
    if (usb.IsSdExportedToHost()) return AgingSdStatus::kUsbExported;
    if (usb.IsBusy() || sd_access::IsQuiescing()) return AgingSdStatus::kBusy;
    auto& sd = SdCardManager::GetInstance();
    if (!sd.IsMounted()) {
        bool mount_failed = false;
        const esp_err_t error = sd.GetLastInitError(&mount_failed);
        if (init_error) *init_error = error;
        switch (error) {
            case ESP_OK: return AgingSdStatus::kNotReady;
            case ESP_ERR_TIMEOUT: return AgingSdStatus::kInitTimeout;
            case ESP_FAIL: return mount_failed ? AgingSdStatus::kMountFailed : AgingSdStatus::kInitFailed;
            case ESP_ERR_NO_MEM: return AgingSdStatus::kInitNoMemory;
            default: return AgingSdStatus::kInitFailed;
        }
    }
    return AgingSdStatus::kReady;
}

static AgingSdStatus AgingSdDecodeStatus(reader::ImageDecodeError error) {
    switch (error) {
        case reader::ImageDecodeError::kFileIo: return AgingSdStatus::kFileReadFailed;
        case reader::ImageDecodeError::kInvalidData: return AgingSdStatus::kImageInvalid;
        case reader::ImageDecodeError::kUnsupported: return AgingSdStatus::kImageUnsupported;
        case reader::ImageDecodeError::kNoMemory: return AgingSdStatus::kNoMemory;
        default: return AgingSdStatus::kDecodeFailed;
    }
}

static const char* AgingSdStatusText(AgingSdStatus status) {
    switch (status) {
        case AgingSdStatus::kNotReady: return Lang::Strings::SETTINGS_TEST_AGING_SD_NOT_READY;
        case AgingSdStatus::kInitTimeout: return Lang::Strings::SETTINGS_TEST_AGING_SD_INIT_TIMEOUT;
        case AgingSdStatus::kMountFailed: return Lang::Strings::SETTINGS_TEST_AGING_SD_MOUNT_FAILED;
        case AgingSdStatus::kInitNoMemory: return Lang::Strings::SETTINGS_TEST_AGING_SD_INIT_MEMORY;
        case AgingSdStatus::kInitFailed: return Lang::Strings::SETTINGS_TEST_AGING_SD_INIT_FAILED;
        case AgingSdStatus::kUsbExported: return Lang::Strings::SETTINGS_TEST_AGING_SD_USB;
        case AgingSdStatus::kBusy: return Lang::Strings::SETTINGS_TEST_AGING_SD_BUSY;
        case AgingSdStatus::kDirectoryMissing: return Lang::Strings::SETTINGS_TEST_AGING_SD_DIR_MISSING;
        case AgingSdStatus::kDirectoryOpenFailed: return Lang::Strings::SETTINGS_TEST_AGING_SD_DIR_OPEN;
        case AgingSdStatus::kDirectoryReadFailed: return Lang::Strings::SETTINGS_TEST_AGING_SD_DIR_READ;
        case AgingSdStatus::kFileStatFailed: return Lang::Strings::SETTINGS_TEST_AGING_SD_FILE_STAT;
        case AgingSdStatus::kFileReadFailed: return Lang::Strings::SETTINGS_TEST_AGING_SD_FILE_READ;
        case AgingSdStatus::kImageInvalid: return Lang::Strings::SETTINGS_TEST_AGING_SD_IMAGE_INVALID;
        case AgingSdStatus::kImageUnsupported: return Lang::Strings::SETTINGS_TEST_AGING_SD_IMAGE_UNSUPPORTED;
        case AgingSdStatus::kDecodeFailed: return Lang::Strings::SETTINGS_TEST_AGING_SD_DECODE;
        case AgingSdStatus::kNoMemory: return Lang::Strings::SETTINGS_TEST_AGING_SD_MEMORY;
        case AgingSdStatus::kTaskFailed: return Lang::Strings::SETTINGS_TEST_AGING_SD_TASK;
        default: return Lang::Strings::SETTINGS_TEST_AGING_NO_WALLPAPER;
    }
}

static void AgingSdLogResult(const AgingSdWork& work) {
    static uint32_t generation = UINT32_MAX;
    static AgingSdStatus previous = AgingSdStatus::kEmpty;
    static esp_err_t previous_init = ESP_OK;
    static int previous_error = 0;
    static reader::ImageDecodeError previous_decode = reader::ImageDecodeError::kNone;
    const bool same_session = generation == work.generation;
    if (same_session && previous == work.status && previous_init == work.init_error && previous_error == work.system_error &&
        previous_decode == work.decode_error) return;
    if (work.status == AgingSdStatus::kReady) {
        if (same_session && previous != AgingSdStatus::kReady && previous != AgingSdStatus::kEmpty) {
            ESP_LOGI(TAG, "wallpaper read recovered");
        }
    } else if (work.status == AgingSdStatus::kNotReady || work.status == AgingSdStatus::kInitTimeout ||
               work.status == AgingSdStatus::kDirectoryMissing) {
        ESP_LOGW(TAG, "wallpaper unavailable stage=%s status=%u init=0x%x errno=%d", work.stage,
                 static_cast<unsigned>(work.status), static_cast<unsigned>(work.init_error), work.system_error);
    } else if (work.status != AgingSdStatus::kEmpty && work.status != AgingSdStatus::kBusy &&
               work.status != AgingSdStatus::kUsbExported && work.status != AgingSdStatus::kCancelled) {
        ESP_LOGE(TAG, "wallpaper failed stage=%s status=%u init=0x%x errno=%d decode=%u", work.stage,
                 static_cast<unsigned>(work.status), static_cast<unsigned>(work.init_error), work.system_error, static_cast<unsigned>(work.decode_error));
    }
    generation = work.generation;
    previous = work.status;
    previous_init = work.init_error;
    previous_error = work.system_error;
    previous_decode = work.decode_error;
}

static void AgingSdReadFirst(AgingSdWork& work) {
    if (work.generation != s_generation.load()) {
        work.status = AgingSdStatus::kCancelled;
        return;
    }
    work.status = AgingSdAvailability(&work.init_error);
    if (work.status != AgingSdStatus::kReady) return;
    sd_access::Transaction access;
    if (!access) {
        work.status = AgingSdAvailability(&work.init_error);
        if (work.status == AgingSdStatus::kReady) work.status = AgingSdStatus::kBusy;
        return;
    }
    work.stage = "directory_open";
    errno = 0;
    std::unique_ptr<DIR, decltype(&closedir)> directory(opendir(SD_PATH_WALLPAPER), closedir);
    if (!directory) {
        work.system_error = errno;
        work.status = errno == ENOENT ? AgingSdStatus::kDirectoryMissing : AgingSdStatus::kDirectoryOpenFailed;
        return;
    }
    unsigned visited = 0;
    work.stage = "directory_read";
    while (!AgingSdCancelled(&work)) {
        errno = 0;
        const dirent* entry = readdir(directory.get());
        if (!entry) {
            work.system_error = errno;
            work.status = errno ? AgingSdStatus::kDirectoryReadFailed : AgingSdStatus::kEmpty;
            return;
        }
        if ((++visited & 15u) == 0) vTaskDelay(1);
        if (!AgingSdImageName(entry->d_name)) continue;
        char path[SD_FILE_PATH_CAPACITY];
        const int length = std::snprintf(path, sizeof(path), "%s/%s", SD_PATH_WALLPAPER, entry->d_name);
        work.stage = "file_stat";
        if (length < 0 || size_t(length) >= sizeof(path)) {
            work.status = AgingSdStatus::kFileStatFailed;
            work.system_error = ENAMETOOLONG;
            return;
        }
        struct stat status{};
        if (stat(path, &status) != 0) {
            work.system_error = errno;
            work.status = AgingSdStatus::kFileStatFailed;
            return;
        }
        if (!S_ISREG(status.st_mode)) {
            work.stage = "directory_read";
            continue;
        }
        work.stage = "decode";
        errno = 0;
        const bool decoded = reader::DecodeImageFileToL8(path, work.side, work.side, work.image,
                                                        AgingSdCancelled, &work, &work.decode_error);
        if (AgingSdCancelled(&work)) {
            work.status = work.generation != s_generation.load() ? AgingSdStatus::kCancelled : AgingSdStatus::kBusy;
        } else if (decoded && !work.image.empty()) {
            work.status = AgingSdStatus::kReady;
        } else {
            work.status = AgingSdDecodeStatus(work.decode_error);
            if (work.decode_error == reader::ImageDecodeError::kFileIo) work.system_error = errno;
        }
        return;
    }
    work.status = work.generation != s_generation.load() ? AgingSdStatus::kCancelled : AgingSdStatus::kBusy;
}

static void AgingSdApply(void* context) {
    std::unique_ptr<AgingSdWork> work(static_cast<AgingSdWork*>(context));
    if (work->generation != s_generation.load() || !s_image || !s_notice) return;
    lv_image_set_src(s_image, static_cast<const void*>(nullptr));
    AgingSdLogResult(*work);
    if (work->status != AgingSdStatus::kReady) work->image = {};
    s_pixels = std::move(work->image);
    if (!s_pixels.empty()) {
        s_pixels.BindDsc();
        lv_image_set_src(s_image, &s_pixels.dsc);
        lv_obj_set_size(s_image, s_pixels.width, s_pixels.height);
        lv_obj_center(s_image);
        lv_obj_clear_flag(s_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_notice, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_image, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_notice, AgingSdStatusText(work->status));
        lv_obj_clear_flag(s_notice, LV_OBJ_FLAG_HIDDEN);
        lv_obj_center(s_notice);
    }
    if (work->done) work->done(work->session);
}

static void AgingSdWorker(void* context) {
    auto* work = static_cast<AgingSdWork*>(context);
    try {
        AgingSdReadFirst(*work);
    } catch (const std::bad_alloc&) {
        work->status = AgingSdStatus::kNoMemory;
        work->decode_error = reader::ImageDecodeError::kNoMemory;
    }
    s_reading.fetch_sub(1);
    if (!ScreenLvAsync(AgingSdApply, work)) {
        ESP_LOGE(TAG, "wallpaper result delivery failed stage=%s status=%u init=0x%x errno=%d decode=%u", work->stage,
                 static_cast<unsigned>(work->status), static_cast<unsigned>(work->init_error), work->system_error, static_cast<unsigned>(work->decode_error));
        const auto done = work->done;
        const uint32_t session = work->session;
        delete work;
        if (done) done(session);
    }
    vTaskDeleteWithCaps(nullptr);
}

lv_obj_t* SettingsTestAgingSd_Build(lv_obj_t* parent, lv_coord_t side) {
    SettingsTestAgingSd_Reset();
    s_stopping = false;
    auto* frame = lv_obj_create(parent);
    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, side, side);
    lv_obj_set_style_border_width(frame, UiSx(2), 0);
    lv_obj_set_style_border_color(frame, lv_color_black(), 0);
    lv_obj_set_style_bg_color(frame, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(frame, UiSx(8), 0);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_CLICKABLE);
    s_image_side = side - 2 * (UiSx(8) + UiSx(2));
    s_image = lv_image_create(frame);
    EpdSetContentRole(s_image, EpdContentRole::Photo);
    lv_obj_clear_flag(s_image, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_image, LV_OBJ_FLAG_HIDDEN);
    s_notice = lv_label_create(frame);
    lv_obj_set_width(s_notice, s_image_side);
    lv_label_set_long_mode(s_notice, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_notice, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_align(s_notice, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_notice, Lang::Strings::WALLPAPER_ENTER_LOADING);
    lv_obj_center(s_notice);
    return frame;
}

void SettingsTestAgingSd_ReadCycle(void (*done)(uint32_t), uint32_t session) {
    if (s_stopping) {
        if (done) done(session);
        return;
    }
    auto* work = new (std::nothrow) AgingSdWork{s_generation.load(), session, s_image_side, done};
    if (work) {
        s_reading.fetch_add(1);
        if (xTaskCreatePinnedToCoreWithCaps(AgingSdWorker, "aging_sd", 6144, work,
            tskIDLE_PRIORITY + 1, nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) return;
        s_reading.fetch_sub(1);
    }
    delete work;
    AgingSdWork failure{s_generation.load(), session, s_image_side, done};
    failure.status = AgingSdStatus::kTaskFailed;
    failure.stage = "task_create";
    AgingSdLogResult(failure);
    if (s_image) {
        lv_image_set_src(s_image, static_cast<const void*>(nullptr));
        lv_obj_add_flag(s_image, LV_OBJ_FLAG_HIDDEN);
    }
    s_pixels = {};
    if (s_notice) {
        lv_label_set_text(s_notice, Lang::Strings::SETTINGS_TEST_AGING_SD_TASK);
        lv_obj_clear_flag(s_notice, LV_OBJ_FLAG_HIDDEN);
        lv_obj_center(s_notice);
    }
    if (done) done(session);
}

bool SettingsTestAgingSd_StopReading() {
    if (!s_stopping) {
        s_stopping = true;
        s_generation.fetch_add(1);
    }
    return s_reading.load() == 0;
}

void SettingsTestAgingSd_Reset() {
    SettingsTestAgingSd_StopReading();
    if (s_image) lv_image_set_src(s_image, static_cast<const void*>(nullptr));
    s_image = nullptr;
    s_notice = nullptr;
    s_pixels = {};
}
