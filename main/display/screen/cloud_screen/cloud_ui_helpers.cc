#include "cloud_screen/cloud_ui_helpers.h"
#include "cloud_screen/cloud_screen_priv.h"
#include "cloud_screen/cloud_dialogs.h"
#include "cloud_screen/cloud_list.h"
#include "cloud_screen/push/push_resources_library.h"

#include <lvgl.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "reader/reader.h"
#include "reader/text_encoding.h"
#include "power_policy.h"

const lv_font_t* Cloud_UiFont() {
    const lv_font_t* f = fontpack_lv_font_get(30, 2);
    return f != nullptr ? f : fontpack_lv_font_ui();
}

const lv_font_t* Cloud_ItemFont() {
    return fontpack_lv_font_ui();
}

lv_coord_t Cloud_ContentWidth() {
    return LV_HOR_RES - kPad * 2;
}

void Cloud_DisableScroll(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(obj, LV_DIR_NONE);
}

lv_coord_t Cloud_MeasureTextWidth(const lv_font_t* font, const char* text) {
    if (font == nullptr || text == nullptr || text[0] == '\0') {
        return 0;
    }
    lv_point_t sz = {};
    lv_text_get_size(&sz, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return static_cast<lv_coord_t>(sz.x);
}

std::string EllipsizeText(const std::string& text, const lv_font_t* font, lv_coord_t max_w) {
    if (font == nullptr || max_w <= 0 || Cloud_MeasureTextWidth(font, text.c_str()) <= max_w) {
        return text;
    }
    constexpr const char* kEllipsis = "…";
    const lv_coord_t suffix_w = Cloud_MeasureTextWidth(font, kEllipsis);
    if (suffix_w >= max_w) {
        return kEllipsis;
    }
    const lv_coord_t budget = max_w - suffix_w;
    std::string out;
    out.reserve(text.size());
    const uint8_t* d = reinterpret_cast<const uint8_t*>(text.data());
    size_t i = 0;
    lv_coord_t used = 0;
    while (i < text.size()) {
        uint32_t cp = 0;
        const size_t n = reader::Utf8Next(d + i, text.size() - i, &cp);
        if (n == 0) {
            break;
        }
        lv_coord_t gw = static_cast<lv_coord_t>(lv_font_get_glyph_width(font, cp, 0));
        if (gw <= 0) {
            gw = 1;
        }
        if (used + gw > budget) {
            break;
        }
        out.append(text, i, n);
        used += gw;
        i += n;
    }
    out.append(kEllipsis);
    return out;
}

// 按像素逐字排满最多两行（末行可省略）。 绕开 LVGL 在 LONG_WRAP 时「非行首英文整词不拆」导致的首行留白。
std::string Cloud_LayoutTitleTwoLines(const char* text, const lv_font_t* font, lv_coord_t max_w) {
    if (text == nullptr || text[0] == '\0') {
        return {};
    }
    if (font == nullptr || max_w <= 0) {
        return text;
    }

    const uint8_t* d = reinterpret_cast<const uint8_t*>(text);
    const size_t len = std::strlen(text);
    auto glyph_w = [font](uint32_t cp) -> lv_coord_t {
        lv_coord_t gw = static_cast<lv_coord_t>(lv_font_get_glyph_width(font, cp, 0));
        return gw > 0 ? gw : 1;
    };

    std::string line1;
    size_t i = 0;
    lv_coord_t used = 0;
    while (i < len) {
        uint32_t cp = 0;
        const size_t n = reader::Utf8Next(d + i, len - i, &cp);
        if (n == 0) {
            break;
        }
        const lv_coord_t gw = glyph_w(cp);
        if (used + gw > max_w) {
            break;
        }
        line1.append(text + i, n);
        used += gw;
        i += n;
    }
    if (i >= len) {
        return line1;
    }

    lv_coord_t rest_w = 0;
    for (size_t j = i; j < len;) {
        uint32_t cp = 0;
        const size_t n = reader::Utf8Next(d + j, len - j, &cp);
        if (n == 0) {
            break;
        }
        rest_w += glyph_w(cp);
        j += n;
    }
    if (rest_w <= max_w) {
        return line1 + "\n" + std::string(text + i);
    }

    constexpr const char* kEllipsis = "…";
    uint32_t ell_cp = 0;
    reader::Utf8Next(reinterpret_cast<const uint8_t*>(kEllipsis), 3, &ell_cp);
    const lv_coord_t budget = max_w - glyph_w(ell_cp);

    std::string line2;
    used = 0;
    while (i < len && budget > 0) {
        uint32_t cp = 0;
        const size_t n = reader::Utf8Next(d + i, len - i, &cp);
        if (n == 0) {
            break;
        }
        const lv_coord_t gw = glyph_w(cp);
        if (used + gw > budget) {
            break;
        }
        line2.append(text + i, n);
        used += gw;
        i += n;
    }
    line2.append(kEllipsis);
    return line1 + "\n" + line2;
}

void Cloud_ShowMessage(lv_obj_t* parent, const char* msg) {
    if (parent == nullptr) {
        return;
    }
    lv_obj_clean(parent);
    lv_obj_set_style_layout(parent, LV_LAYOUT_NONE, 0);
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, msg != nullptr ? msg : "");
    lv_obj_set_style_text_font(label, Cloud_UiFont(), 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, Cloud_ContentWidth());
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_center(label);
    Cloud_DisableScroll(label);
}

void CancelStatusClearTimer() {
    if (s_status_clear_timer == nullptr) {
        return;
    }
    lv_timer_delete(s_status_clear_timer);
    s_status_clear_timer = nullptr;
}

static void StatusClearTimerCb(lv_timer_t* /*t*/) {
    s_status_clear_timer = nullptr;
    auto& st = Cloud_State();
    st.data.status_text[0] = '\0';
    if (st.chrome.status_lbl != nullptr) {
        lv_label_set_text(st.chrome.status_lbl, "");
        lv_obj_add_flag(st.chrome.status_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

void ApplyStatusTipUi() {
    auto& st = Cloud_State();
    if (st.chrome.status_lbl == nullptr) {
        return;
    }
    if (st.data.status_text[0] == '\0') {
        lv_label_set_text(st.chrome.status_lbl, "");
        lv_obj_add_flag(st.chrome.status_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(st.chrome.status_lbl, st.data.status_text);
    lv_obj_clear_flag(st.chrome.status_lbl, LV_OBJ_FLAG_HIDDEN);
}

void SetStatusTip(const char* text, bool auto_clear) {
    CancelStatusClearTimer();
    auto& st = Cloud_State();
    if (text == nullptr || text[0] == '\0') {
        st.data.status_text[0] = '\0';
    } else {
        std::snprintf(st.data.status_text, sizeof(st.data.status_text), "%s", text);
    }
    ApplyStatusTipUi();
    if (auto_clear && st.data.status_text[0] != '\0') {
        s_status_clear_timer = lv_timer_create(StatusClearTimerCb, kStatusClearMs, nullptr);
        if (s_status_clear_timer != nullptr) {
            lv_timer_set_repeat_count(s_status_clear_timer, 1);
        }
    }
}

bool ItemMatchesTab(const reader::CloudPushResource& item, int tab) {
    if (tab <= 0) {
        return true;
    }
    if (tab == 1) {
        return item.type == reader::PushResourceType::kBadge;
    }
    if (tab == 2) {
        return item.type == reader::PushResourceType::kBook;
    }
    if (tab == 3) {
        return item.type == reader::PushResourceType::kFont;
    }
    return true;
}

void RebuildFiltered() {
    auto& st = Cloud_State();
    st.data.filtered.clear();
    st.data.filtered.reserve(st.data.items.size());
    for (int i = 0; i < static_cast<int>(st.data.items.size()); ++i) {
        if (ItemMatchesTab(st.data.items[static_cast<size_t>(i)], st.data.filter_tab)) {
            st.data.filtered.push_back(i);
        }
    }
}

void RefreshSectionTitle() {
    auto& st = Cloud_State();
    if (st.chrome.section_title == nullptr) {
        return;
    }
    const char* title = Lang::Strings::CLOUD_PENDING;
    if (st.data.filter_tab == 1) {
        title = Lang::Strings::CLOUD_PUSH_WALLPAPER;
    } else if (st.data.filter_tab == 2) {
        title = Lang::Strings::CLOUD_PUSH_BOOK;
    } else if (st.data.filter_tab == 3) {
        title = Lang::Strings::CLOUD_PUSH_FONT;
    }
    lv_label_set_text(st.chrome.section_title, title);
}

void StyleTabBtn(lv_obj_t* btn, lv_obj_t* lbl, bool on) {
    if (btn == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(btn, on ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    if (lbl != nullptr) {
        lv_obj_set_style_text_color(lbl, on ? lv_color_white() : lv_color_black(), 0);
    }
}

void RefreshTabUi() {
    auto& st = Cloud_State();
    for (int i = 0; i < kTabCount; ++i) {
        StyleTabBtn(st.chrome.tab_btns[i], st.chrome.tab_lbls[i], st.data.filter_tab == i);
    }
    RefreshSectionTitle();
}

void SyncSelectedSize() {
    auto& st = Cloud_State();
    if (st.data.selected.size() != st.data.items.size()) {
        st.data.selected.assign(st.data.items.size(), 0);
    }
}

int SelectedCount() {
    SyncSelectedSize();
    auto& st = Cloud_State();
    int n = 0;
    for (uint8_t v : st.data.selected) {
        if (v != 0) {
            ++n;
        }
    }
    return n;
}

bool ItemSelected(int idx) {
    SyncSelectedSize();
    auto& st = Cloud_State();
    return idx >= 0 && idx < static_cast<int>(st.data.selected.size()) &&
           st.data.selected[static_cast<size_t>(idx)] != 0;
}

void ToggleItemSelected(int idx) {
    SyncSelectedSize();
    auto& st = Cloud_State();
    if (idx < 0 || idx >= static_cast<int>(st.data.selected.size())) {
        return;
    }
    st.data.selected[static_cast<size_t>(idx)] = st.data.selected[static_cast<size_t>(idx)] ? 0 : 1;
}

void RefreshFooterMode() {
    auto& st = Cloud_State();
    if (st.data.multi) {
        if (st.chrome.sync_btn != nullptr) {
            lv_obj_add_flag(st.chrome.sync_btn, LV_OBJ_FLAG_HIDDEN);
        }
        if (st.chrome.multi_bar != nullptr) {
            lv_obj_clear_flag(st.chrome.multi_bar, LV_OBJ_FLAG_HIDDEN);
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), Lang::Strings::CLOUD_SELECTED_FMT, SelectedCount());
        SetStatusTip(buf, false);
    } else {
        if (st.chrome.multi_bar != nullptr) {
            lv_obj_add_flag(st.chrome.multi_bar, LV_OBJ_FLAG_HIDDEN);
        }
        if (st.chrome.sync_btn != nullptr) {
            lv_obj_clear_flag(st.chrome.sync_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ExitMultiModeEx(bool rebuild, bool clear_status) {
    auto& st = Cloud_State();
    if (!st.data.multi && st.data.selected.empty()) {
        if (rebuild) {
            RequestCloudRender();
        }
        return;
    }
    st.data.multi = false;
    st.data.suppress_row_click_until_us = 0;
    st.data.suppress_row_click_idx = -1;
    st.data.selected.assign(st.data.items.size(), 0);
    if (clear_status) {
        SetStatusTip("", false);
    }
    RefreshFooterMode();
    if (rebuild) {
        RequestCloudRender();
    }
}

void ExitMultiMode(bool rebuild) {
    ExitMultiModeEx(rebuild, true);
}

void EnterMultiModeSelect(int idx) {
    auto& st = Cloud_State();
    CloseActionDialog();
    CloseDialog();
    st.data.multi = true;
    SyncSelectedSize();
    st.data.selected.assign(st.data.items.size(), 0);
    if (idx >= 0 && idx < static_cast<int>(st.data.selected.size())) {
        st.data.selected[static_cast<size_t>(idx)] = 1;
    }
    RefreshFooterMode();
    RequestCloudRender();
}

void FillPreviewMeta(const reader::CloudPushResource* item) {
    auto& st = Cloud_State();
    if (st.preview.meta == nullptr) {
        return;
    }
    lv_obj_clean(st.preview.meta);
    if (item == nullptr) {
        return;
    }
    auto make_chip = [&](const char* text) {
        lv_obj_t* chip = lv_obj_create(st.preview.meta);
        lv_obj_remove_style_all(chip);
        lv_obj_set_height(chip, kChipH);
        lv_obj_set_width(chip, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(chip, kChipPadH, 0);
        lv_obj_set_style_pad_ver(chip, 4, 0);
        lv_obj_set_style_border_width(chip, kRowBorderW, 0);
        lv_obj_set_style_border_color(chip, lv_color_black(), 0);
        lv_obj_set_style_radius(chip, kChipRadius, 0);
        lv_obj_set_style_bg_color(chip, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        Cloud_DisableScroll(chip);
        lv_obj_t* lbl = lv_label_create(chip);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, Cloud_UiFont(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    };
    make_chip(item->TypeLabel());
    make_chip(reader::FormatCloudFileSize(item->file_size).c_str());
}

bool ScreenAlive() {
    // 仅认指针：Delete/离页会置空。工作线程禁止 lv_obj_is_valid（无锁会 LoadProhibited）
    return Cloud_State().chrome.screen != nullptr;
}

bool OpStillValid(uint32_t op_gen) {
    return op_gen != 0 && ScreenAlive() && Cloud_State().xfer.op_generation == op_gen;
}

bool SyncBusy() {
    auto& st = Cloud_State();
    return st.data.waiting_net || st.data.loading;
}

void ReleaseUiKeepNet() {
    if (s_ui_net_held.exchange(false)) {
        PowerPolicy::GetInstance().Release(PowerNeed::UiKeepNet);
    }
}

int PageCount() {
    auto& st = Cloud_State();
    if (st.data.filtered.empty() || st.data.page_size <= 0) {
        return 1;
    }
    return static_cast<int>((st.data.filtered.size() + static_cast<size_t>(st.data.page_size) - 1) /
                            static_cast<size_t>(st.data.page_size));
}

void ClampPage() {
    auto& st = Cloud_State();
    const int pages = PageCount();
    if (st.data.page < 0) {
        st.data.page = 0;
    }
    if (st.data.page >= pages) {
        st.data.page = pages - 1;
    }
}

void SetPageFooterText(const char* text) {
    auto& st = Cloud_State();
    if (st.chrome.page_lbl != nullptr) {
        lv_label_set_text(st.chrome.page_lbl, text != nullptr ? text : "");
    }
}

