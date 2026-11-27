#include <atomic>
#include "ui/apps/settings/settings_test/settings_test_common.h"
#include "boards/common/board/board.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/apps/settings/settings_common.h"
#include "ui/common/drawing/ui_theme.h"

static SettingsTestUi s_ui;
static std::atomic<uint32_t> s_generation{0};
static std::atomic<bool> s_live{false};

SettingsTestUi& SettingsTest_Ui() {
    return s_ui;
}

bool SettingsTest_IsLive() {
    return s_live.load(std::memory_order_acquire);
}

uint32_t SettingsTest_Generation() {
    return s_generation.load();
}

void SettingsTest_BeginSession() {
    s_generation.fetch_add(1);
    s_live.store(true, std::memory_order_release);
}

void SettingsTest_BeginShutdown() {
    s_live.store(false, std::memory_order_release);
    s_generation.fetch_add(1);
}

void SettingsTest_SetRowStatus(SettingsTestRow& row, bool pass) {
    if (!SettingsTest_IsLive() || row.icon == nullptr) {
        return;
    }
    lv_image_set_src(row.icon, pass ? kSettingsTestIconPass : kSettingsTestIconFail);
    lv_obj_clear_flag(row.icon, LV_OBJ_FLAG_HIDDEN);
}

void SettingsTest_SetRowValue(SettingsTestRow& row, const char* text) {
    if (!SettingsTest_IsLive() || row.value == nullptr) {
        return;
    }
    lv_label_set_text(row.value, text != nullptr ? text : "");
    lv_obj_set_style_text_color(row.value, UiColorForeground(), 0);
}

SettingsTestRow SettingsTest_CreateRow(lv_obj_t* parent, const char* title, lv_event_cb_t row_click_cb) {
    SettingsTestRow out;

    lv_obj_t* row = lv_obj_create(parent);
    out.row = row;
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, kSettingsTestRowH, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, UiColorForeground(), 0);
    lv_obj_set_style_pad_hor(row, 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    if (row_click_cb != nullptr) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(row);
        lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, nullptr);
    } else {
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t* title_lbl = lv_label_create(row);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_width(title_lbl, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(title_lbl, lv_pct(54), 0);
    lv_obj_set_height(title_lbl, LV_SIZE_CONTENT);
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(title_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(title_lbl, UiColorForeground(), 0);
    lv_obj_clear_flag(title_lbl, LV_OBJ_FLAG_CLICKABLE);

    out.value = lv_label_create(row);
    lv_label_set_text(out.value, "…");
    lv_obj_set_width(out.value, 0);
    lv_obj_set_flex_grow(out.value, 1);
    lv_obj_set_height(out.value, LV_SIZE_CONTENT);
    lv_label_set_long_mode(out.value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(out.value, UiFont(), 0);
    lv_obj_set_style_text_color(out.value, UiColorForeground(), 0);
    lv_obj_set_style_text_align(out.value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_clear_flag(out.value, LV_OBJ_FLAG_CLICKABLE);

    out.icon = lv_image_create(row);
    lv_obj_set_size(out.icon, kSettingsTestIconSz, kSettingsTestIconSz);
    lv_obj_clear_flag(out.icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(out.icon, LV_OBJ_FLAG_HIDDEN);

    return out;
}

DualNetworkBoard* SettingsTest_GetDualBoard() {
    return dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
}

Nt26Board* SettingsTest_GetNt26Board() {
    auto& board = Board::GetInstance();
    if (auto* dual = dynamic_cast<DualNetworkBoard*>(&board)) {
        return dynamic_cast<Nt26Board*>(&dual->GetCurrentBoard());
    }
    return dynamic_cast<Nt26Board*>(&board);
}

bool SettingsTest_IsCellNetwork() {
    if (auto* dual = SettingsTest_GetDualBoard()) {
        return dual->GetNetworkType() == NetworkType::CELLULAR;
    }
    return DualNetworkBoard::LoadNetworkTypeFromSettings(1) == NetworkType::CELLULAR;
}
