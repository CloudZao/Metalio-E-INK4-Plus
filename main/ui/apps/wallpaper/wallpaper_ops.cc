#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <esp_log.h>
#include <lvgl.h>
#include "ui/apps/wallpaper/wallpaper_ops.h"
#include "assets/lang_config.h"
#include "assets/lang_wallpaper.h"
#include "infra/storage/sd_paths.h"
#include "business/wallpaper/wallpaper_active.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "business/reader/book_reader_prefs.h"
#include "ui/apps/wallpaper/preview/wallpaper_preview.h"
#include "ui/apps/wallpaper/wallpaper_list.h"
#include "ui/apps/wallpaper/wallpaper_screen_priv.h"
#include "ui/apps/wallpaper/wallpaper_ui_helpers.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/runtime/modal.h"

static UiModal s_window_modal;

static void ApplyEnableResult(const WallpaperResult& result) {
    if (!Wallpaper_RequestCurrent()) {
        return;
    }
    if (result.succeeded > 0) {
        RefreshActiveName();
        if (Wallpaper_State().mode == UiMode::kList) {
            RebuildListPage();
        }
    } else {
        ScreenShowToast(result.error[0] ? result.error : LangString_WALLPAPER_ENABLE_FAIL);
    }
    UpdateEnableButtonUi();
    UpdateDeleteButtonUi();
    Wallpaper_ReturnToReading(false);
}

static void ScheduleEnable(int index, bool for_standby, bool clear, bool overlay, bool for_reading = false) {
    auto& state = Wallpaper_State();
    if (index < 0 || index >= static_cast<int>(state.list.files.size())) {
        return;
    }
    bool accepted = false;
    try {
        const auto target = for_reading ? WallpaperTarget::Reading :
                            for_standby ? WallpaperTarget::Standby : WallpaperTarget::Shutdown;
        accepted = Wallpaper_TrackRequest(WallpaperSelect(target, state.list.files[index].path, clear, overlay),
                                           ApplyEnableResult);
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "operation=select stage=allocate result=no_memory");
    }
    if (!accepted) {
        ScreenShowToast(LangString_WALLPAPER_ENABLE_FAIL);
    }
    UpdateEnableButtonUi();
    UpdateDeleteButtonUi();
}

static bool CanSelectDisplayMode() {
    const auto& st = Wallpaper_State();
    return st.screen_alive && !st.decode_paused && st.mode == UiMode::kPreview && st.preview.idx >= 0 &&
           st.preview.idx < static_cast<int>(st.list.files.size()) && !st.preview.adjust_open &&
           !WallpaperIsBusy() && !st.request && !st.workers.load_busy;
}

static void ApplyDisplayMode(bool overlay, bool clear) {
    const auto& st = Wallpaper_State();
    if (!st.ui.dialog_mask || !CanSelectDisplayMode()) {
        return;
    }
    const bool for_standby = st.preview.enable_for_standby;
    const char* name = st.list.files[static_cast<size_t>(st.preview.idx)].name;
    const bool selected = (for_standby ? st.standby_name : st.shutdown_name) == name;
    const bool current_overlay = Wallpaper_IsOverlay(for_standby);
    Wallpaper_CloseDialog();
    if (!clear && selected && current_overlay == overlay) {
        return;
    }
    ScheduleEnable(st.preview.idx, for_standby, clear, overlay);
}

static void ShowDisplayModeDialog(bool for_standby) {
    auto& st = Wallpaper_State();
    if (!CanSelectDisplayMode() || st.ui.dialog_mask) {
        return;
    }
    const char* name = st.list.files[static_cast<size_t>(st.preview.idx)].name;
    const bool selected = (for_standby ? st.standby_name : st.shutdown_name) == name;
    const bool decode_failed = st.preview.decode_error != ImageDecodeError::kNone;
    if (decode_failed) {
        char message[80] = {};
        Wallpaper_FormatDecodeFailure(message, sizeof(message), st.preview.decode_error);
        ScreenShowToast(message);
        if (!selected) {
            return;
        }
    }
    st.preview.enable_for_standby = for_standby;
    const bool overlay = Wallpaper_IsOverlay(for_standby);
    const lv_coord_t card_w = Wallpaper_ContentWidth();

    lv_obj_t* mask = lv_obj_create(st.ui.screen);
    st.ui.dialog_mask = mask;
    s_window_modal.Bind(mask, [](void*) { Wallpaper_CloseDialog(); });
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
    lv_obj_set_style_pad_row(card, kPreviewGap, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ScreenStyleDialogCard(card, kPad);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, [](lv_event_t* e) { lv_event_stop_bubbling(e); }, LV_EVENT_CLICKED, nullptr);

    const lv_coord_t width = card_w - 2 * (kPad + kBorderW);
    lv_obj_t* title = lv_label_create(card);
    lv_obj_set_width(title, width);
    lv_obj_set_style_text_font(title, Wallpaper_UiFont(), 0);
    lv_obj_set_style_text_color(title, UiColorForeground(), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_label_set_text(title, LangString_WALLPAPER_DISPLAY_MODE);

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
        option(LangString_WALLPAPER_MODE_FULLSCREEN, selected && !overlay,
               [](lv_event_t*) { ApplyDisplayMode(false, false); });
        option(LangString_WALLPAPER_MODE_OVERLAY, selected && overlay,
               [](lv_event_t*) { ApplyDisplayMode(true, false); });
    }
    if (selected) {
        option(LangString_WALLPAPER_CLEAR_SELECTION, false, [](lv_event_t*) { ApplyDisplayMode(false, true); });
    }
    lv_obj_center(card);
}

static void ApplyDeleteResult(const WallpaperResult& result) {
    if (!Wallpaper_RequestCurrent()) {
        return;
    }
    if (result.succeeded == 0) {
        ScreenShowToast(result.error[0] ? result.error : LangString_WALLPAPER_DELETE_FAIL);
        UpdateEnableButtonUi();
        UpdateDeleteButtonUi();
        return;
    }
    ClearPreviewImage();
    RefreshActiveName();
    CollectWallpapers();
    Wallpaper_State().preview.idx = -1;
    ShowMode(UiMode::kList);
    RebuildListPage();
    Wallpaper_RevealAfterWhite();
}

void OnEnableShutdownClicked(lv_event_t* /*e*/) {
    ShowDisplayModeDialog(false);
}

void OnEnableStandbyClicked(lv_event_t* /*e*/) {
    ShowDisplayModeDialog(true);
}

void OnEnableReadingClicked(lv_event_t* /*e*/) {
    const auto& st = Wallpaper_State();
    if (!CanSelectDisplayMode() || st.ui.dialog_mask) {
        return;
    }
    if (st.preview.decode_error != ImageDecodeError::kNone) {
        char message[80] = {};
        Wallpaper_FormatDecodeFailure(message, sizeof(message), st.preview.decode_error);
        ScreenShowToast(message);
        return;
    }
    const bool selected = BookReaderPrefsBackground() == 3 &&
                          Wallpaper_GetReadingFilename() == st.list.files[static_cast<size_t>(st.preview.idx)].name;
    ScheduleEnable(st.preview.idx, false, selected, false, true);
}



void ScheduleDelete(int index) {
    auto& state = Wallpaper_State();
    if (index < 0 || index >= static_cast<int>(state.list.files.size()) || WallpaperIsBusy() || state.request) {
        return;
    }
    bool accepted = false;
    try {
        accepted = Wallpaper_TrackRequest(WallpaperDelete({state.list.files[index].name}), ApplyDeleteResult);
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "operation=delete stage=allocate result=no_memory");
    }
    if (!accepted) {
        ScreenShowToast(LangString_WALLPAPER_DELETE_START_FAIL);
    }
    UpdateEnableButtonUi();
    UpdateDeleteButtonUi();
}

void Wallpaper_OnDeleteClicked(lv_event_t* /*e*/) {
    if (Wallpaper_State().mode != UiMode::kPreview || Wallpaper_State().preview.idx < 0 ||
        Wallpaper_State().preview.adjust_open) {
        return;
    }
    if (Wallpaper_OperationPending(WallpaperOperation::Delete) || Wallpaper_OperationPending(WallpaperOperation::Select) ||
        Wallpaper_State().workers.load_busy.load() || Wallpaper_OperationPending(WallpaperOperation::Save)) {
        return;
    }
    const std::string name = Wallpaper_State().list.files[Wallpaper_State().preview.idx].name;
    ScreenConfirmDelete(Wallpaper_State().ui.screen, [name] {
        const auto& files = Wallpaper_State().list.files;
        const auto found = std::find_if(files.begin(), files.end(),
                                        [&](const FileEntry& entry) { return name == entry.name; });
        if (found != files.end()) {
            ScheduleDelete(static_cast<int>(found - files.begin()));
        }
    });
}
