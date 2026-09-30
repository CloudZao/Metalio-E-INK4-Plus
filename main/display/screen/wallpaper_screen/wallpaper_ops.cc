#include "wallpaper_screen/wallpaper_ops.h"
#include "wallpaper_screen/wallpaper_screen_priv.h"
#include "wallpaper_screen/wallpaper_active.h"
#include "wallpaper_screen/wallpaper_list.h"
#include "wallpaper_screen/preview/wallpaper_preview.h"
#include "wallpaper_screen/wallpaper_ui_helpers.h"

#include <freertos/FreeRTOS.h>
#include <lvgl.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <esp_log.h>
#include <freertos/task.h>

#include "assets/lang_config.h"
#include "reader/reader.h"
#include "cloud_screen/push/wallpaper_cloud_library.h"
#include "sd_paths.h"

struct EnableWork {
    uint32_t epoch = 0;
    bool for_standby = false;
    bool clear = false;
    char path[192] = {};
    char name[96] = {};
};

struct EnableResultMsg {
    uint32_t epoch = 0;
    bool ok = false;
    bool for_standby = false;
    char err[80] = {};
    char name[96] = {};
};

void ApplyEnableAsync(void* p) {
    auto* msg = static_cast<EnableResultMsg*>(p);
    Wallpaper_State().workers.enable_busy.store(false);
    Wallpaper_State().workers.enable_task = nullptr;
    if (msg == nullptr) {
        return;
    }
    if (!Wallpaper_State().screen_alive || msg->epoch != Wallpaper_State().epoch) {
        delete msg;
        return;
    }
    if (msg->ok) {
        if (msg->for_standby) {
            Wallpaper_State().standby_name = msg->name[0] != '\0' ? msg->name : "";
        } else {
            Wallpaper_State().shutdown_name = msg->name[0] != '\0' ? msg->name : "";
        }
        char preview_name[sizeof(FileEntry::name)] = {};
        if (Wallpaper_State().preview.idx >= 0 && Wallpaper_State().preview.idx < static_cast<int>(Wallpaper_State().list.files.size())) {
            std::snprintf(preview_name, sizeof(preview_name), "%s", Wallpaper_State().list.files[Wallpaper_State().preview.idx].name);
        }
        SortFilesActiveFirst();
        if (preview_name[0] != '\0') {
            Wallpaper_State().preview.idx = -1;
            for (size_t i = 0; i < Wallpaper_State().list.files.size(); ++i) {
                if (std::strcmp(Wallpaper_State().list.files[i].name, preview_name) == 0) {
                    Wallpaper_State().preview.idx = static_cast<int>(i);
                    break;
                }
            }
        }
        if (Wallpaper_State().mode == UiMode::kList) {
            RebuildListPage();
        }
    } else {
        ESP_LOGW(TAG, "enable failed: %s", msg->err[0] != '\0' ? msg->err : Lang::Strings::WALLPAPER_ENABLE_FAIL);
    }
    UpdateEnableButtonUi();
    UpdateDeleteButtonUi();
    delete msg;
}

void EnableTask(void* arg) {
    auto* work = static_cast<EnableWork*>(arg);
    auto* msg = new EnableResultMsg{};
    if (work == nullptr) {
        std::snprintf(msg->err, sizeof(msg->err), "%s", Lang::Strings::WALLPAPER_FILE_INVALID);
    } else {
        msg->epoch = work->epoch;
        msg->for_standby = work->for_standby;
        std::snprintf(msg->name, sizeof(msg->name), "%s", work->name);
        if (work->clear) {
            if (work->for_standby) {
                Wallpaper_ClearStandbyWallpaper();
            } else {
                Wallpaper_ClearShutdownWallpaper();
            }
            msg->ok = true;
            msg->name[0] = '\0';
        } else {
            std::string err;
            msg->ok = work->for_standby ? Wallpaper_SetStandbyFromFile(work->path, err)
                                        : Wallpaper_SetActiveFromFile(work->path, err);
            if (!msg->ok) {
                std::snprintf(msg->err, sizeof(msg->err), "%s",
                              err.empty() ? Lang::Strings::WALLPAPER_ENABLE_FAIL : err.c_str());
            }
        }
        delete work;
    }
    if (lv_async_call(ApplyEnableAsync, msg) != LV_RESULT_OK) {
        delete msg;
        Wallpaper_State().workers.enable_busy.store(false);
        Wallpaper_State().workers.enable_task = nullptr;
    }
    vTaskDelete(nullptr);
}

void ScheduleEnable(int index, bool for_standby, bool clear) {
    if (index < 0 || index >= static_cast<int>(Wallpaper_State().list.files.size())) {
        return;
    }
    if (Wallpaper_State().workers.enable_busy.exchange(true)) {
        return;
    }
    UpdateEnableButtonUi();
    auto* work = new EnableWork{};
    work->epoch = Wallpaper_State().epoch;
    work->for_standby = for_standby;
    work->clear = clear;
    std::snprintf(work->path, sizeof(work->path), "%s",
                  Wallpaper_State().list.files[static_cast<size_t>(index)].path);
    std::snprintf(work->name, sizeof(work->name), "%s",
                  Wallpaper_State().list.files[static_cast<size_t>(index)].name);
    if (xTaskCreatePinnedToCore(EnableTask, "wp_enable", 8 * 1024, work, 5, &Wallpaper_State().workers.enable_task, 0) !=
        pdPASS) {
        delete work;
        Wallpaper_State().workers.enable_busy.store(false);
        Wallpaper_State().workers.enable_task = nullptr;
        ESP_LOGW(TAG, "enable task create failed");
        UpdateEnableButtonUi();
        UpdateDeleteButtonUi();
    }
}

void OnEnableShutdownClicked(lv_event_t* /*e*/) {
    if (Wallpaper_State().mode != UiMode::kPreview || Wallpaper_State().preview.idx < 0 || Wallpaper_State().preview.adjust_open) {
        return;
    }
    if (Wallpaper_State().workers.enable_busy.load() || Wallpaper_State().workers.load_busy.load() || Wallpaper_State().workers.delete_busy.load() || Wallpaper_State().workers.save_busy.load()) {
        return;
    }
    const char* name = Wallpaper_State().list.files[static_cast<size_t>(Wallpaper_State().preview.idx)].name;
    const bool is_on = !Wallpaper_State().shutdown_name.empty() && Wallpaper_State().shutdown_name == name;
    ScheduleEnable(Wallpaper_State().preview.idx, false, is_on);
}

void OnEnableStandbyClicked(lv_event_t* /*e*/) {
    if (Wallpaper_State().mode != UiMode::kPreview || Wallpaper_State().preview.idx < 0 || Wallpaper_State().preview.adjust_open) {
        return;
    }
    if (Wallpaper_State().workers.enable_busy.load() || Wallpaper_State().workers.load_busy.load() || Wallpaper_State().workers.delete_busy.load() || Wallpaper_State().workers.save_busy.load()) {
        return;
    }
    const char* name = Wallpaper_State().list.files[static_cast<size_t>(Wallpaper_State().preview.idx)].name;
    const bool is_on = !Wallpaper_State().standby_name.empty() && Wallpaper_State().standby_name == name;
    ScheduleEnable(Wallpaper_State().preview.idx, true, is_on);
}

struct DeleteWork {
    uint32_t epoch = 0;
    char path[192] = {};
    char name[96] = {};
};

struct DeleteResultMsg {
    uint32_t epoch = 0;
    bool ok = false;
    char err[80] = {};
};

bool DeleteWallpaperFileOnSd(const char* path, const char* name, std::string& err_out) {
    err_out.clear();
    if (path == nullptr || path[0] == '\0' || name == nullptr || name[0] == '\0') {
        err_out = Lang::Strings::WALLPAPER_FILE_INVALID;
        return false;
    }
    const std::string prefix = std::string(SD_PATH_WALLPAPER) + "/";
    if (std::strncmp(path, prefix.c_str(), prefix.size()) != 0) {
        err_out = Lang::Strings::WALLPAPER_PATH_INVALID;
        return false;
    }
    if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0 || std::strchr(name, '/') != nullptr) {
        err_out = Lang::Strings::WALLPAPER_NAME_INVALID;
        return false;
    }

    if (unlink(path) != 0 && errno != ENOENT) {
        err_out = Lang::Strings::WALLPAPER_DELETE_FAIL;
        return false;
    }
    reader::DeleteWallpaperMeta(path);
    unlink((std::string(path) + ".tmp").c_str());

    // ClearActiveIfMatches / NVS：须在本 worker（内部 RAM 栈），勿在 LVGL 任务调用
    Wallpaper_ClearActiveIfMatches(name);
    ESP_LOGI(TAG, "deleted wallpaper %s", path);
    return true;
}

void ApplyDeleteAsync(void* p) {
    auto* msg = static_cast<DeleteResultMsg*>(p);
    Wallpaper_State().workers.delete_busy.store(false);
    Wallpaper_State().workers.delete_task = nullptr;
    if (msg == nullptr) {
        return;
    }
    if (!Wallpaper_State().screen_alive || msg->epoch != Wallpaper_State().epoch) {
        delete msg;
        return;
    }
    if (!msg->ok) {
        std::snprintf(Wallpaper_State().preview.delete_meta_hint, sizeof(Wallpaper_State().preview.delete_meta_hint), "%s",
                      msg->err[0] != '\0' ? msg->err : Lang::Strings::WALLPAPER_DELETE_FAIL);
        UpdateEnableButtonUi();
        UpdateDeleteButtonUi();
        delete msg;
        return;
    }
    Wallpaper_State().preview.delete_meta_hint[0] = '\0';
    ClearPreviewImage();
    RefreshActiveName();
    CollectWallpapers();
    Wallpaper_State().preview.idx = -1;
    ShowMode(UiMode::kList);
    RebuildListPage();
    delete msg;
}

void DeleteTask(void* arg) {
    auto* work = static_cast<DeleteWork*>(arg);
    auto* msg = new DeleteResultMsg{};
    if (work == nullptr) {
        std::snprintf(msg->err, sizeof(msg->err), "%s", Lang::Strings::WALLPAPER_FILE_INVALID);
    } else {
        msg->epoch = work->epoch;
        std::string err;
        msg->ok = DeleteWallpaperFileOnSd(work->path, work->name, err);
        if (!msg->ok) {
            std::snprintf(msg->err, sizeof(msg->err), "%s",
                          err.empty() ? Lang::Strings::WALLPAPER_DELETE_FAIL : err.c_str());
        }
        delete work;
    }
    if (lv_async_call(ApplyDeleteAsync, msg) != LV_RESULT_OK) {
        delete msg;
        Wallpaper_State().workers.delete_busy.store(false);
        Wallpaper_State().workers.delete_task = nullptr;
    }
    vTaskDelete(nullptr);
}

void ScheduleDelete(int index) {
    if (index < 0 || index >= static_cast<int>(Wallpaper_State().list.files.size())) {
        return;
    }
    if (Wallpaper_State().workers.delete_busy.exchange(true)) {
        return;
    }
    Wallpaper_State().preview.delete_meta_hint[0] = '\0';
    UpdateEnableButtonUi();
    UpdateDeleteButtonUi();
    auto* work = new DeleteWork{};
    work->epoch = Wallpaper_State().epoch;
    std::snprintf(work->path, sizeof(work->path), "%s",
                  Wallpaper_State().list.files[static_cast<size_t>(index)].path);
    std::snprintf(work->name, sizeof(work->name), "%s",
                  Wallpaper_State().list.files[static_cast<size_t>(index)].name);
    if (xTaskCreatePinnedToCore(DeleteTask, "wp_delete", 8 * 1024, work, 5, &Wallpaper_State().workers.delete_task, 0) !=
        pdPASS) {
        delete work;
        Wallpaper_State().workers.delete_busy.store(false);
        Wallpaper_State().workers.delete_task = nullptr;
        std::snprintf(Wallpaper_State().preview.delete_meta_hint, sizeof(Wallpaper_State().preview.delete_meta_hint), "%s", Lang::Strings::WALLPAPER_DELETE_START_FAIL);
        UpdateEnableButtonUi();
        UpdateDeleteButtonUi();
    }
}

void Wallpaper_OnDeleteClicked(lv_event_t* /*e*/) {
    if (Wallpaper_State().mode != UiMode::kPreview || Wallpaper_State().preview.idx < 0 || Wallpaper_State().preview.adjust_open) {
        return;
    }
    if (Wallpaper_State().workers.delete_busy.load() || Wallpaper_State().workers.enable_busy.load() || Wallpaper_State().workers.load_busy.load() || Wallpaper_State().workers.save_busy.load()) {
        return;
    }
    ScheduleDelete(Wallpaper_State().preview.idx);
}

