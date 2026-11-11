#include "wallpaper_screen/wallpaper_ops.h"
#include "wallpaper_screen/wallpaper_screen_priv.h"
#include "wallpaper_screen/wallpaper_active.h"
#include "wallpaper_screen/wallpaper_list.h"
#include "wallpaper_screen/preview/wallpaper_preview.h"
#include "wallpaper_screen/wallpaper_ui_helpers.h"

#include <freertos/FreeRTOS.h>
#include <lvgl.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <esp_log.h>
#include <freertos/task.h>

#include "assets/lang_config.h"
#include "assets/lang_wallpaper.h"
#include "haptic_feedback.h"
#include "reader/reader.h"
#include "reader/image_file_write.h"
#include "cloud_screen/push/wallpaper_cloud_library.h"
#include "sd_paths.h"
#include "book_screen/reader/book_reader_prefs.h"

struct EnableWork {
    uint32_t epoch = 0;
    bool for_standby = false;
    bool for_reading = false; // 阅读背景独立于关机和待机壁纸
    bool overlay = false; // 当前图片叠加到原界面
    bool clear = false;
    char path[SD_FILE_PATH_CAPACITY] = {};
};

struct EnableResultMsg {
    uint32_t epoch = 0;
    bool ok = false;
    bool for_reading = false, clear = false; // UI 任务更新阅读偏好
    char err[80] = {};
};

void ApplyEnableAsync(void* p) {
    auto* msg = static_cast<EnableResultMsg*>(p);
    Wallpaper_State().workers.enable_busy.store(false);
    Wallpaper_State().workers.enable_task = nullptr;
    if (msg == nullptr) {
        return;
    }
    if (msg->ok && msg->for_reading) BookReaderPrefsSetBackground(msg->clear ? 0 : 3);
    if (!Wallpaper_State().screen_alive || msg->epoch != Wallpaper_State().epoch) {
        delete msg;
        return;
    }
    if (msg->ok) {
        RefreshActiveName();
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
        ScreenShowToast(msg->err[0] != '\0' ? msg->err : Lang::Strings::WALLPAPER_ENABLE_FAIL);
    }
    UpdateEnableButtonUi();
    UpdateDeleteButtonUi();
    delete msg;
    Wallpaper_ReturnToReading(false);
}

void EnableTask(void* arg) {
    auto* work = static_cast<EnableWork*>(arg);
    auto* msg = new EnableResultMsg{};
    if (work == nullptr) {
        std::snprintf(msg->err, sizeof(msg->err), "%s", Lang::Strings::WALLPAPER_FILE_INVALID);
    } else {
        msg->epoch = work->epoch;
        msg->for_reading = work->for_reading;
        msg->clear = work->clear;
        if (work->clear) {
            if (work->for_reading) {
                Wallpaper_ClearReadingWallpaper();
            } else if (work->for_standby) {
                Wallpaper_ClearStandbyWallpaper();
            } else {
                Wallpaper_ClearShutdownWallpaper();
            }
            msg->ok = true;
        } else {
            std::string err;
            msg->ok = work->for_reading ? Wallpaper_SetReadingFromFile(work->path, err)
                        : work->for_standby ? Wallpaper_SetStandbyFromFile(work->path, err, work->overlay)
                                        : Wallpaper_SetActiveFromFile(work->path, err, work->overlay);
            if (!msg->ok) {
                std::snprintf(msg->err, sizeof(msg->err), "%s",
                              err.empty() ? Lang::Strings::WALLPAPER_ENABLE_FAIL : err.c_str());
            }
        }
        delete work;
    }
    if (!ScreenLvAsync(ApplyEnableAsync, msg)) {
        delete msg;
        Wallpaper_State().workers.enable_busy.store(false);
        Wallpaper_State().workers.enable_task = nullptr;
    }
    vTaskDelete(nullptr);
}

static void ScheduleEnable(int index, bool for_standby, bool clear, bool overlay, bool for_reading = false) {
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
    work->for_reading = for_reading;
    work->overlay = overlay;
    work->clear = clear;
    std::snprintf(work->path, sizeof(work->path), "%s",
                  Wallpaper_State().list.files[static_cast<size_t>(index)].path);
    if (xTaskCreatePinnedToCore(EnableTask, "wp_enable", 8 * 1024, work, 5, &Wallpaper_State().workers.enable_task, 0) !=
        pdPASS) {
        delete work;
        Wallpaper_State().workers.enable_busy.store(false);
        Wallpaper_State().workers.enable_task = nullptr;
        ESP_LOGW(TAG, "enable task create failed");
        ScreenShowToast(Lang::Strings::WALLPAPER_ENABLE_FAIL);
        UpdateEnableButtonUi();
        UpdateDeleteButtonUi();
    }
}

static bool CanSelectDisplayMode() {
    const auto& st = Wallpaper_State();
    return st.screen_alive && !st.decode_paused && st.mode == UiMode::kPreview &&
        st.preview.idx >= 0 && st.preview.idx < static_cast<int>(st.list.files.size()) &&
        !st.preview.adjust_open && !st.workers.enable_busy && !st.workers.load_busy &&
        !st.workers.delete_busy && !st.workers.save_busy;
}

static void ApplyDisplayMode(bool overlay, bool clear) {
    const auto& st = Wallpaper_State();
    if (!st.ui.dialog_mask || !CanSelectDisplayMode()) return;
    const bool for_standby = st.preview.enable_for_standby;
    const char* name = st.list.files[static_cast<size_t>(st.preview.idx)].name;
    const bool selected = (for_standby ? st.standby_name : st.shutdown_name) == name;
    const bool current_overlay = Wallpaper_IsOverlay(for_standby);
    Wallpaper_CloseDialog();
    if (!clear && selected && current_overlay == overlay) return;
    ScheduleEnable(st.preview.idx, for_standby, clear, overlay);
}

static void ShowDisplayModeDialog(bool for_standby) {
    auto& st = Wallpaper_State();
    if (!CanSelectDisplayMode() || st.ui.dialog_mask) return;
    const char* name = st.list.files[static_cast<size_t>(st.preview.idx)].name;
    const bool selected = (for_standby ? st.standby_name : st.shutdown_name) == name;
    const bool decode_failed = st.preview.decode_error != reader::ImageDecodeError::kNone;
    if (decode_failed) {
        char message[80] = {};
        Wallpaper_FormatDecodeFailure(message, sizeof(message), st.preview.decode_error);
        ScreenShowToast(message);
        if (!selected) return;
    }
    st.preview.enable_for_standby = for_standby;
    const bool overlay = Wallpaper_IsOverlay(for_standby);
    const lv_coord_t card_w = Wallpaper_ContentWidth();

    lv_obj_t* mask = lv_obj_create(st.ui.screen);
    st.ui.dialog_mask = mask;
    lv_obj_remove_style_all(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(mask, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(mask, 0, 0);
    Wallpaper_DisableScroll(mask);
    lv_obj_set_style_bg_opa(mask, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(mask, [](lv_event_t*) { Wallpaper_CloseDialog(); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, card_w, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, kBorderW, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, kPad, 0);
    lv_obj_set_style_pad_row(card, kPreviewGap, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    Wallpaper_DisableScroll(card);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, [](lv_event_t* e) { lv_event_stop_bubbling(e); }, LV_EVENT_CLICKED, nullptr);

    const lv_coord_t width = card_w - 2 * (kPad + kBorderW);
    lv_obj_t* title = lv_label_create(card);
    lv_obj_set_width(title, width);
    lv_obj_set_style_text_font(title, Wallpaper_UiFont(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_label_set_text(title, Lang::Strings::WALLPAPER_DISPLAY_MODE);

    auto option = [&](const char* text, bool active, lv_event_cb_t callback) {
        lv_obj_t* button = lv_obj_create(card);
        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, width * 2 / 3, kActionH);
        Wallpaper_DisableScroll(button);
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
        HapticAttachClick(button);
        lv_obj_t* label = lv_label_create(button);
        lv_label_set_text(label, text);
        StyleActionBtn(button, label, active, true);
        lv_obj_center(label);
    };
    if (!decode_failed) {
        option(Lang::Strings::WALLPAPER_MODE_FULLSCREEN, selected && !overlay,
               [](lv_event_t*) { ApplyDisplayMode(false, false); });
        option(Lang::Strings::WALLPAPER_MODE_OVERLAY, selected && overlay,
               [](lv_event_t*) { ApplyDisplayMode(true, false); });
    }
    if (selected) option(Lang::Strings::WALLPAPER_CLEAR_SELECTION, false,
                          [](lv_event_t*) { ApplyDisplayMode(false, true); });
    lv_obj_center(card);
}

void OnEnableShutdownClicked(lv_event_t* /*e*/) { ShowDisplayModeDialog(false); }
void OnEnableStandbyClicked(lv_event_t* /*e*/) { ShowDisplayModeDialog(true); }

void OnEnableReadingClicked(lv_event_t* /*e*/) {
    const auto& st = Wallpaper_State();
    if (!CanSelectDisplayMode() || st.ui.dialog_mask) return;
    if (st.preview.decode_error != reader::ImageDecodeError::kNone) {
        char message[80] = {};
        Wallpaper_FormatDecodeFailure(message, sizeof(message), st.preview.decode_error);
        ScreenShowToast(message);
        return;
    }
    const bool selected = BookReaderPrefsBackground() == 3 &&
        Wallpaper_GetReadingFilename() == st.list.files[static_cast<size_t>(st.preview.idx)].name;
    ScheduleEnable(st.preview.idx, false, selected, false, true);
}

struct DeleteWork {
    uint32_t epoch = 0;
    char path[SD_FILE_PATH_CAPACITY] = {};
    char name[SD_FILE_NAME_CAPACITY] = {};
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
    if (!SdIsValidFileName(name) || std::strcmp(path + prefix.size(), name) != 0) {
        err_out = Lang::Strings::WALLPAPER_NAME_INVALID;
        return false;
    }

    if (!reader::DeleteWallpaperFile(path)) {
        err_out = Lang::Strings::WALLPAPER_DELETE_FAIL;
        return false;
    }
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
        ScreenShowToast(msg->err[0] != '\0' ? msg->err : Lang::Strings::WALLPAPER_DELETE_FAIL);
        UpdateEnableButtonUi();
        UpdateDeleteButtonUi();
        delete msg;
        return;
    }
    ClearPreviewImage();
    RefreshActiveName();
    CollectWallpapers();
    Wallpaper_State().preview.idx = -1;
    ShowMode(UiMode::kList);
    RebuildListPage();
    Wallpaper_RevealAfterWhite();
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
    if (!ScreenLvAsync(ApplyDeleteAsync, msg)) {
        delete msg;
        Wallpaper_State().workers.delete_busy.store(false);
        Wallpaper_State().workers.delete_task = nullptr;
    }
    vTaskDelete(nullptr);
}

void ScheduleDelete(int index) {
    if (Wallpaper_State().workers.save_busy.load()) return;
    if (index < 0 || index >= static_cast<int>(Wallpaper_State().list.files.size())) {
        return;
    }
    if (Wallpaper_State().workers.delete_busy.exchange(true)) {
        return;
    }
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
        ScreenShowToast(Lang::Strings::WALLPAPER_DELETE_START_FAIL);
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

