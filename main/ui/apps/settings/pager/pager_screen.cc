#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include <esp_timer.h>
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/apps/settings/pager/pager_screen.h"
#include "assets/lang_config.h"
#include "boards/common/bluetooth/ble_page_turner.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/runtime/input/pager_key_handler.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/runtime/modal.h"
#include "ui/common/widgets/screen_text.h"
#include "ui/common/widgets/screen_options.h"
#include "ui/apps/standby/standby_screen.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/runtime/screen_common_nav.h"

using PagerScreenState = BlePageTurnerState;
using PagerScreenAction = BlePageTurnerAction;
using PagerScreenSnapshot = BlePageTurnerSnapshot;
static constexpr int64_t kRescanIntervalUs = 3000000;  // 后台扫描轮次间隔
enum class PagerScreenView { kList, kDevice, kForget, kKeys, kLearn };

struct PagerScreenRowContext {
    char address[18] = {};  // 按身份地址解析点击，避免扫描或忘记后索引变化
    bool saved = false;     // 已保存行进入管理，未保存行发起连接
};

static std::vector<PagerScreenRowContext> s_rows;
static lv_obj_t* s_screen = nullptr;
static lv_obj_t* s_body = nullptr;
static lv_obj_t* s_modal = nullptr;
static lv_obj_t* s_card = nullptr;
static lv_obj_t* s_modal_body = nullptr;
static lv_obj_t* s_learn_message = nullptr;
static lv_obj_t* s_save = nullptr;
static lv_timer_t* s_timer = nullptr;
static PagerScreenView s_view = PagerScreenView::kList;
static char s_device[18] = {};
static PagerScreenAction s_action = PagerScreenAction::kPrevious;
static uint8_t s_candidate = 0;
static int64_t s_learning_since = 0;
static uint32_t s_revision = UINT32_MAX;
static int s_gap = 0;
static int s_learn_height = 0;
static int s_learn_y = 0;
static bool s_scan_requested = false;
static bool s_background_scan = false;
static bool s_initial_scan = false;
static bool s_scan_was_running = false;
static int64_t s_next_scan_at = 0;
static PagerScreenSnapshot s_displayed;
static const char* s_progress = nullptr;
static const char* s_device_status = nullptr;
static char s_attempt_address[18] = {};
static int64_t s_connecting_until = 0;
static PagerScreenState s_presented_state = PagerScreenState::kIdle;

static UiModal s_window_modal;

static PagerScreenSnapshot UiSnapshot() {
    auto state = BlePageTurnerGetSnapshot();
    // 快速连接也保留一次可见过渡，避免墨水屏将连接中与已连接合并。
    if (state.state == PagerScreenState::kConnected && esp_timer_get_time() < s_connecting_until &&
        !std::strcmp(state.target_address, s_attempt_address)) {
        state.state = PagerScreenState::kConnecting;
        state.active_saved = -1;
    }
    return state;
}

static void Connecting(const char* address) {
    std::snprintf(s_attempt_address, sizeof(s_attempt_address), "%s", address);
    s_connecting_until = esp_timer_get_time() + 1000000;
}

static void Strip(lv_obj_t* object) {
    lv_obj_remove_style_all(object);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_CLICKABLE);
}

static lv_obj_t* Container(lv_obj_t* parent, int width, int height) {
    auto* object = lv_obj_create(parent);
    Strip(object);
    lv_obj_set_size(object, width, height);
    lv_obj_update_layout(object);
    return object;
}

static lv_obj_t* Label(lv_obj_t* parent, const char* text, int y, int height, UiFontRole role = UiFontRole::Body) {
    auto* label = lv_label_create(parent);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(label, text);
    ScreenFitLabel(label, lv_obj_get_width(parent), height, UiFont(role));
    lv_obj_set_pos(label, 0, y);
    return label;
}

static lv_obj_t* Button(lv_obj_t* parent, const char* text, int x, int y, int width, int height, lv_event_cb_t callback,
                        intptr_t value = 0, bool selected = false) {
    auto* button = ScreenCreateSelectableOption(parent, text, callback, value);
    lv_obj_set_size(button, width, height);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_pad_all(button, UiSx(5), 0);
    lv_obj_update_layout(button);
    auto* label = lv_obj_get_child(button, 0);
    ScreenFitLabel(label, lv_obj_get_content_width(button), lv_obj_get_content_height(button),
                   UiFont(UiFontRole::Caption));
    lv_obj_center(label);
    ScreenStyleSelectable(button, label, selected);
    return button;
}

static const char* ActionName() {
    return s_action == PagerScreenAction::kPrevious ? LangString_SETTINGS_PAGER_PREVIOUS
                                                    : LangString_SETTINGS_PAGER_NEXT;
}

static int DeviceIndex(const PagerScreenSnapshot& state) {
    for (int i = 0; i < state.saved_count; ++i) {
        if (!std::strcmp(state.Saved()[i].address, s_device)) {
            return i;
        }
    }
    return -1;
}

static const char* DeviceStatus(const PagerScreenSnapshot& state, int index) {
    if (state.state == PagerScreenState::kConnected && state.active_saved == index) {
        return LangString_PAGER_CONNECTED_TAG;
    }
    if (state.state == PagerScreenState::kConnecting &&
        !std::strcmp(state.target_address, state.Saved()[index].address)) {
        return LangString_PAGER_CONNECTING_TAG;
    }
    if (state.state == PagerScreenState::kError && !std::strcmp(state.target_address, state.Saved()[index].address)) {
        return LangString_PAGER_FAILED_TAG;
    }
    return state.Saved()[index].nearby ? LangString_PAGER_NEARBY : LangString_PAGER_AWAY;
}

static bool SelectedConnected(const PagerScreenSnapshot& state) {
    return state.enabled && state.state == PagerScreenState::kConnected && state.active_saved >= 0 &&
           !std::strcmp(state.Saved()[state.active_saved].address, s_device);
}

static void Show(PagerScreenView view);
static void Refresh(lv_timer_t*);
static void LearnText(const char* text);

static void Error() {
    ScreenShowToast(LangString_SETTINGS_PAGER_SAVE_ERROR);
}

static const char* Progress(const PagerScreenSnapshot& state) {
    if (state.state == PagerScreenState::kConnecting) {
        return nullptr;
    }
    if (state.state == PagerScreenState::kStarting) {
        return LangString_SETTINGS_PAGER_STARTING;
    }
    if (s_initial_scan && (s_scan_requested || state.scanning || state.searching)) {
        return LangString_PAGER_SCANNING_SHORT;
    }
    return nullptr;
}

static void StopScanning() {
    s_background_scan = false;
    s_scan_requested = false;
    s_initial_scan = false;
    s_scan_was_running = false;
    s_next_scan_at = 0;
    BlePageTurnerStopScan();
}

static void PollScan(const PagerScreenSnapshot& state) {
    if (!s_background_scan) {
        return;
    }
    const auto now = esp_timer_get_time();
    if (state.state == PagerScreenState::kError || state.state == PagerScreenState::kConnecting) {
        StopScanning();
    } else if (state.scanning || s_scan_requested) {
        s_scan_was_running = true;
    } else if (s_scan_was_running) {
        s_scan_was_running = false;
        s_initial_scan = false;
        s_next_scan_at = now + kRescanIntervalUs;
    } else if (s_view == PagerScreenView::kList && s_next_scan_at && now >= s_next_scan_at && !TouchUiFingerIsDown()) {
        s_scan_requested = BlePageTurnerScan(PagerKey_Receive, true);
        s_scan_was_running = s_scan_requested;
        s_next_scan_at = s_scan_requested ? 0 : now + kRescanIntervalUs;
    }
}

static bool ListChanged(const PagerScreenSnapshot& state) {
    if (state.state != s_displayed.state || state.count != s_displayed.count ||
        state.saved_count != s_displayed.saved_count || state.active_saved != s_displayed.active_saved ||
        std::strcmp(state.target_address, s_displayed.target_address)) {
        return true;
    }
    for (int i = 0; i < state.saved_count; ++i) {
        const auto& a = state.Saved()[i];
        const auto& b = s_displayed.Saved()[i];
        if (a.model != b.model || a.nearby != b.nearby || std::strcmp(a.address, b.address)) {
            return true;
        }
    }
    for (int i = 0; i < state.count; ++i) {
        const auto& a = state.devices[i];
        const auto& b = s_displayed.devices[i];
        if (a.model != b.model || a.saved_index != b.saved_index || std::strcmp(a.address, b.address)) {
            return true;
        }
    }
    return false;
}

static void Back(lv_event_t*) {
    if (s_view == PagerScreenView::kList) {
        ScreenNavigateBack();
        return;
    }
    Show(s_view == PagerScreenView::kLearn                                        ? PagerScreenView::kKeys
         : s_view == PagerScreenView::kForget || s_view == PagerScreenView::kKeys ? PagerScreenView::kDevice
                                                                                  : PagerScreenView::kList);
}

static void Learn(lv_event_t* event) {
    const auto state = UiSnapshot();
    if (!SelectedConnected(state)) {
        ScreenShowToast(LangString_SETTINGS_PAGER_NEED_CONNECTION);
        return;
    }
    if (!BlePageTurnerSupportsButtonLearning(state.Saved()[state.active_saved].model)) {
        return;
    }
    if (event) {
        s_action = static_cast<PagerScreenAction>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    }
    s_candidate = 0;
    s_learning_since = esp_timer_get_time();
    if (!BlePageTurnerSetLearning(true)) {
        Error();
        return;
    }
    Show(PagerScreenView::kLearn);
}

static void Save(lv_event_t*) {
    const auto state = UiSnapshot();
    if (!s_candidate || !SelectedConnected(state)) {
        Error();
        return;
    }
    if (!BlePageTurnerBind(s_action, s_candidate)) {
        Error();
        return;
    }
    LearnText(LangString_PAGER_SAVING);
    lv_obj_add_state(s_save, LV_STATE_DISABLED);
}

static void Clear(lv_event_t*) {
    if (!SelectedConnected(UiSnapshot()) || !BlePageTurnerBind(s_action, 0)) {
        Error();
        return;
    }
    LearnText(LangString_PAGER_SAVING);
    lv_obj_add_state(s_save, LV_STATE_DISABLED);
}

static void RowClicked(lv_event_t* event) {
    const auto* row = static_cast<const PagerScreenRowContext*>(lv_event_get_user_data(event));
    const auto state = UiSnapshot();
    if (row->saved) {
        std::snprintf(s_device, sizeof(s_device), "%s", row->address);
        Show(PagerScreenView::kDevice);
        return;
    }
    if (state.state == PagerScreenState::kConnecting || state.state == PagerScreenState::kStarting) {
        return;
    }
    for (int i = 0; i < state.count; ++i) {
        if (!std::strcmp(state.devices[i].address, row->address)) {
            StopScanning();
            if (!BlePageTurnerConnect(row->address)) {
                Error();
            } else {
                Connecting(row->address);
                Show(PagerScreenView::kList);
            }
            return;
        }
    }
    ScreenShowToast(LangString_SETTINGS_PAGER_EMPTY);
}

static void DeviceRow(lv_obj_t* parent, const char* address, BlePageTurnerModel model, const char* status, bool saved,
                      bool connected, int height) {
    auto* row = lv_obj_create(parent);
    Strip(row);
    lv_obj_set_size(row, LV_PCT(100), height);
    lv_obj_set_style_pad_hor(row, UiSx(8), 0);
    lv_obj_set_style_pad_column(row, UiSx(8), 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, UiColorForeground(), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(row);
    auto& context = s_rows.emplace_back();
    std::snprintf(context.address, sizeof(context.address), "%s", address);
    context.saved = saved;
    lv_obj_add_event_cb(row, RowClicked, LV_EVENT_CLICKED, &context);
    auto* name = lv_label_create(row);
    lv_obj_set_width(name, 1);
    lv_obj_set_flex_grow(name, 1);
    lv_label_set_text_fmt(name, "%s\n%s", BlePageTurnerModelName(model), address);
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, height, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(name, UiFont(UiFontRole::Caption), 0);
    lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);
    auto* tag = lv_label_create(row);
    lv_label_set_text(tag, status);
    lv_obj_set_style_max_width(tag, std::max<int>(1, lv_obj_get_width(parent) / 3), 0);
    lv_label_set_long_mode(tag, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(tag, UiFont(UiFontRole::Caption), 0);
    lv_obj_clear_flag(tag, LV_OBJ_FLAG_CLICKABLE);
    if (connected) {
        lv_obj_set_style_bg_color(row, UiColorForeground(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(name, UiColorBackground(), 0);
        lv_obj_set_style_text_color(tag, UiColorBackground(), 0);
    }
}

static void List(const PagerScreenSnapshot& state, int width, int height) {
    s_progress = Progress(state);
    if (s_progress) {
        auto* message = Label(s_body, s_progress, 0, height, UiFontRole::Body);
        lv_obj_center(message);
        return;
    }
    int unknown = 0;
    for (int i = 0; i < state.count; ++i) {
        if (state.devices[i].saved_index < 0) {
            ++unknown;
        }
    }
    const int line = UiFont(UiFontRole::Caption)->line_height;
    const int heading = line + s_gap;
    const int row_h = std::max<int>(UiSy(68), 2 * line + UiSy(10));
    const int pad = UiSx(6);
    const int available = height - 2 * heading - 2 * s_gap;
    int boxes[2] = {std::max(2 * line, state.saved_count * row_h) + 2 * pad,
                    std::max(2 * line, unknown * row_h) + 2 * pad};
    if (boxes[0] + boxes[1] > available) {
        const int saved_h = std::min(boxes[0], available / 2);
        boxes[1] = std::min(boxes[1], available - saved_h);
        boxes[0] = std::min(boxes[0], available - boxes[1]);
    }
    s_rows.clear();
    s_rows.reserve(state.saved_count + unknown);
    int y = 0;
    for (int group = 0; group < 2; ++group) {
        char title[96];
        std::snprintf(title, sizeof(title), group == 0 ? LangString_PAGER_SAVED_FMT : LangString_PAGER_UNSAVED_FMT,
                      group == 0 ? state.saved_count : unknown);
        Label(s_body, title, y, heading);
        y += heading;
        auto* box = Container(s_body, width, boxes[group]);
        lv_obj_set_pos(box, 0, y);
        lv_obj_set_style_border_width(box, kScreenBorderW, 0);
        lv_obj_set_style_border_color(box, UiColorForeground(), 0);
        lv_obj_set_style_radius(box, kScreenOptionRadius, 0);
        lv_obj_set_style_pad_all(box, pad, 0);
        lv_obj_update_layout(box);
        auto* rows = Container(box, lv_obj_get_content_width(box), lv_obj_get_content_height(box));
        lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
        const int items = group == 0 ? state.saved_count : unknown;
        const bool overflow = items * row_h > lv_obj_get_content_height(box);
        lv_obj_set_flag(rows, LV_OBJ_FLAG_SCROLLABLE, overflow);
        lv_obj_set_scroll_dir(rows, overflow ? LV_DIR_VER : LV_DIR_NONE);
        lv_obj_set_scrollbar_mode(rows, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(rows, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
        lv_obj_clear_flag(rows, LV_OBJ_FLAG_SCROLL_MOMENTUM);
        lv_obj_clear_flag(rows, LV_OBJ_FLAG_SCROLL_ELASTIC);
        int count = 0;
        if (group == 0) {
            // 已连接、附近已保存、离线已保存保持清晰顺序。
            for (int category = 0; category < 3; ++category) {
                for (int i = 0; i < state.saved_count; ++i) {
                    const bool connected = state.state == PagerScreenState::kConnected && state.active_saved == i;
                    const int kind = connected ? 0 : state.Saved()[i].nearby ? 1 : 2;
                    if (kind != category) {
                        continue;
                    }
                    DeviceRow(rows, state.Saved()[i].address, state.Saved()[i].model, DeviceStatus(state, i), true,
                              connected, row_h);
                    ++count;
                }
            }
        } else {
            for (int i = 0; i < state.count; ++i) {
                if (state.devices[i].saved_index < 0) {
                    const bool connecting = state.state == PagerScreenState::kConnecting &&
                                            !std::strcmp(state.target_address, state.devices[i].address);
                    const bool failed = state.state == PagerScreenState::kError &&
                                        !std::strcmp(state.target_address, state.devices[i].address);
                    DeviceRow(rows, state.devices[i].address, state.devices[i].model,
                              connecting ? LangString_PAGER_CONNECTING_TAG
                              : failed   ? LangString_PAGER_FAILED_TAG
                                         : LangString_PAGER_NEARBY,
                              false, false, row_h);
                    ++count;
                }
            }
        }
        if (!count) {
            lv_obj_set_flex_align(rows, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            auto* hint = Label(rows, group == 0 ? LangString_SETTINGS_PAGER_NO_SAVED : LangString_PAGER_NO_UNSAVED, 0,
                               lv_obj_get_height(rows));
            lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        }
        y += boxes[group] + s_gap;
    }
}

static int HeaderHeight() {
    return UiFont()->line_height + UiFont(UiFontRole::Caption)->line_height + 3 * s_gap;
}

static int TextHeight(const char* text, int width, const lv_font_t* font) {
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, width, LV_TEXT_FLAG_NONE);
    return size.y;
}

static int DeviceHeader(lv_obj_t* content, const PagerScreenSnapshot& state, int index) {
    auto* header = Container(content, lv_obj_get_width(content), HeaderHeight());
    auto* tag = lv_label_create(header);
    lv_label_set_text(tag, DeviceStatus(state, index));
    lv_obj_set_style_max_width(tag, std::max<int>(1, lv_obj_get_width(content) / 3), 0);
    lv_label_set_long_mode(tag, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(tag, UiFont(UiFontRole::Caption), 0);
    lv_obj_clear_flag(tag, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_update_layout(tag);
    lv_obj_align(tag, LV_ALIGN_RIGHT_MID, 0, 0);
    const int info_width = std::max<int>(1, lv_obj_get_width(header) - lv_obj_get_width(tag) - s_gap);
    const int name_h = TextHeight(BlePageTurnerModelName(state.Saved()[index].model), info_width, UiFont());
    const int address_h = TextHeight(state.Saved()[index].address, info_width, UiFont(UiFontRole::Caption));
    const int height = std::max<int>(name_h + s_gap + address_h, lv_obj_get_height(tag));
    lv_obj_set_height(header, height);
    auto* name = lv_label_create(header);
    lv_label_set_text(name, BlePageTurnerModelName(state.Saved()[index].model));
    ScreenFitLabel(name, info_width, name_h, UiFont());
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);
    auto* address = lv_label_create(header);
    lv_label_set_text(address, state.Saved()[index].address);
    ScreenFitLabel(address, info_width, address_h, UiFont(UiFontRole::Caption));
    lv_obj_set_pos(address, 0, name_h + s_gap);
    lv_obj_clear_flag(address, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_align(address, LV_TEXT_ALIGN_LEFT, 0);
    s_device_status = DeviceStatus(state, index);
    return height + s_gap;
}

static void CloseModal() {
    if (s_modal) {
        lv_obj_delete(s_modal);
    }
    s_modal = s_card = s_modal_body = nullptr;
    s_learn_message = s_save = nullptr;
}

static lv_obj_t* ModalBody(PagerScreenView view, const PagerScreenSnapshot& state) {
    if (!s_modal) {
        s_modal = Container(s_screen, LV_HOR_RES, LV_VER_RES);
        s_window_modal.Bind(s_modal, [](void*) { Back(nullptr); }, nullptr, UiModalPolicy::Dismissible,
                   [](void*) { CloseModal(); });
        lv_obj_add_flag(s_modal, LV_OBJ_FLAG_FLOATING);
        lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(s_modal, LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(
            s_modal,
            [](lv_event_t* event) {
                if (lv_event_get_target(event) == s_modal) {
                    Show(PagerScreenView::kList);
                }
            },
            LV_EVENT_CLICKED, nullptr);
    }
    if (s_card) {
        lv_obj_delete(s_card);
    }
    const int pad = UiSx(14);
    const int row = kScreenOptionH;
    const int header = HeaderHeight();
    const bool connected = SelectedConnected(state);
    const bool custom = connected && BlePageTurnerSupportsButtonLearning(state.target_model);
    const int width = std::min<int>(LV_HOR_RES * 84 / 100, lv_obj_get_width(s_body));
    const int inner_width = width - 2 * (pad + kScreenBorderW);
    int height;
    if (view == PagerScreenView::kDevice) {
        height = header + (custom ? 3 : 2) * row + (custom ? 2 : 1) * s_gap + 2 * pad;
    } else if (view == PagerScreenView::kForget) {
        height =
            header + TextHeight(LangString_SETTINGS_PAGER_FORGET_HINT, inner_width, UiFont()) + s_gap + row + 2 * pad;
    } else if (view == PagerScreenView::kKeys) {
        height = header + TextHeight(LangString_SETTINGS_PAGER_PAGE_HINT, inner_width, UiFont(UiFontRole::Caption)) +
                 2 * row + 2 * s_gap + 2 * pad;
    } else {
        height = header + 2 * UiFont(UiFontRole::Caption)->line_height + 5 * UiFont()->line_height + 2 * row +
                 5 * s_gap + 2 * pad;
    }
    height += 2 * kScreenBorderW;
    height = std::min<int>(height, LV_VER_RES - 2 * UiSy(32));
    s_card = Container(s_modal, width, height);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_card, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_card, kScreenBorderW, 0);
    lv_obj_set_style_border_color(s_card, UiColorForeground(), 0);
    lv_obj_set_style_radius(s_card, kScreenOptionRadius, 0);
    lv_obj_set_style_pad_all(s_card, pad, 0);
    lv_obj_center(s_card);
    lv_obj_update_layout(s_card);
    s_modal_body = Container(s_card, lv_obj_get_content_width(s_card), lv_obj_get_content_height(s_card));
    lv_obj_add_flag(s_modal_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_modal_body, LV_DIR_VER);
    return s_modal_body;
}

static void LearnText(const char* text) {
    lv_label_set_text(s_learn_message, text);
    ScreenFitLabel(s_learn_message, lv_obj_get_width(s_modal_body), s_learn_height, UiFont());
    lv_obj_update_layout(s_learn_message);
    lv_obj_set_pos(s_learn_message, 0,
                   s_learn_y + std::max<int>(0, (s_learn_height - lv_obj_get_height(s_learn_message)) / 2));
}

static void Show(PagerScreenView view) {
    if (!s_screen) {
        return;
    }
    if (view != PagerScreenView::kList) {
        StopScanning();
    }
    if (s_view == PagerScreenView::kLearn && view != PagerScreenView::kLearn) {
        BlePageTurnerSetLearning(false);
    }
    s_view = view;
    s_revision = UINT32_MAX;
    const auto state = UiSnapshot();
    s_presented_state = state.state;
    if (view == PagerScreenView::kList) {
        s_displayed = state;
        CloseModal();
        lv_obj_clean(s_body);
        List(state, lv_obj_get_width(s_body), lv_obj_get_height(s_body));
    } else {
        const int index = DeviceIndex(state);
        if (index < 0) {
            Show(PagerScreenView::kList);
            return;
        }
        auto* content = ModalBody(view, state);
        s_learn_message = s_save = nullptr;
        const int width = lv_obj_get_width(content);
        const int height = lv_obj_get_height(content);
        const int row = kScreenOptionH;
        const int line = UiFont(UiFontRole::Caption)->line_height;
        const int half = (width - s_gap) / 2;
        const auto& device = state.Saved()[index];
        const bool connected = SelectedConnected(state);
        const bool custom = connected && BlePageTurnerSupportsButtonLearning(device.model);
        const int top = DeviceHeader(content, state, index);
        if (view == PagerScreenView::kDevice) {
            int y = top;
            auto* connect =
                Button(content, connected ? LangString_SETTINGS_PAGER_DISCONNECT : LangString_SETTINGS_PAGER_CONNECT, 0,
                       y, width, row, [](lv_event_t*) {
                           const auto current = UiSnapshot();
                           const int target = DeviceIndex(current);
                           if (target < 0) {
                               return;
                           }
                           if (SelectedConnected(current)) {
                               BlePageTurnerDisconnect();
                           } else if (!current.Saved()[target].nearby) {
                               ScreenShowToast(LangString_PAGER_NOT_NEARBY);
                           } else if (!BlePageTurnerConnectSaved(s_device)) {
                               Error();
                           } else {
                               Connecting(s_device);
                               Show(PagerScreenView::kList);
                           }
                       });
            lv_obj_set_state(
                connect, LV_STATE_DISABLED,
                state.state == PagerScreenState::kConnecting || state.state == PagerScreenState::kStarting);
            y += row + s_gap;
            auto* forget = Button(content, LangString_SETTINGS_PAGER_FORGET, 0, y, width, row,
                                  [](lv_event_t*) { Show(PagerScreenView::kForget); });
            lv_obj_set_state(
                forget, LV_STATE_DISABLED,
                state.state == PagerScreenState::kConnecting || state.state == PagerScreenState::kStarting);
            if (custom) {
                Button(content, LangString_SETTINGS_PAGER_CUSTOMIZE, 0, y + row + s_gap, width, row,
                       [](lv_event_t*) { Show(PagerScreenView::kKeys); });
            }
        } else if (view == PagerScreenView::kForget) {
            Label(content, LangString_SETTINGS_PAGER_FORGET_HINT, top, height - top - row - s_gap, UiFontRole::Body);
            Button(content, LangString_COMMON_CANCEL, 0, height - row, half, row, Back);
            Button(
                content, LangString_SETTINGS_PAGER_FORGET, half + s_gap, height - row, half, row,
                [](lv_event_t*) {
                    if (!BlePageTurnerForget(s_device)) {
                        Error();
                        return;
                    }
                    Show(PagerScreenView::kList);
                },
                0, true);
        } else if (view == PagerScreenView::kKeys) {
            if (!custom) {
                Show(PagerScreenView::kDevice);
                return;
            }
            const int hint_h = TextHeight(LangString_SETTINGS_PAGER_PAGE_HINT, width, UiFont(UiFontRole::Caption));
            Label(content, LangString_SETTINGS_PAGER_PAGE_HINT, top, hint_h);
            for (int i = 0; i < 2; ++i) {
                char title[160];
                const char* action = i == 0 ? LangString_SETTINGS_PAGER_PREVIOUS : LangString_SETTINGS_PAGER_NEXT;
                const uint8_t key = device.keys[i];
                if (key) {
                    std::snprintf(title, sizeof(title), LangString_SETTINGS_PAGER_BINDING, action, "  ·  ",
                                  'A' + key - 1);
                } else {
                    std::snprintf(title, sizeof(title), "%s  ·  %s", action, LangString_SETTINGS_PAGER_UNSET);
                }
                Button(content, title, 0, top + hint_h + s_gap + i * (row + s_gap), width, row, Learn, i);
            }
        } else if (view == PagerScreenView::kLearn) {
            if (!custom) {
                Show(PagerScreenView::kDevice);
                return;
            }
            char title[160];
            std::snprintf(title, sizeof(title), LangString_PAGER_LEARNING_FMT, ActionName());
            Label(content, title, top, 2 * line, UiFontRole::Body);
            s_learn_y = top + 2 * line + s_gap;
            s_learn_height = height - s_learn_y - 2 * row - 4 * s_gap;
            s_learn_message =
                Label(content, LangString_SETTINGS_PAGER_PRESS, s_learn_y, s_learn_height, UiFontRole::Body);
            LearnText(LangString_SETTINGS_PAGER_PRESS);
            Button(content, LangString_SETTINGS_PAGER_RELEARN, 0, height - 2 * row - s_gap, half, row,
                   [](lv_event_t*) { Learn(nullptr); });
            Button(content, LangString_SETTINGS_PAGER_CLEAR, half + s_gap, height - 2 * row - s_gap, half, row, Clear);
            s_save = Button(content, LangString_SETTINGS_PAGER_SAVE, 0, height - row, width, row, Save, 0, true);
            lv_obj_set_state(s_save, LV_STATE_DISABLED, !s_candidate);
        }
    }
    Refresh(nullptr);
}

static void Refresh(lv_timer_t* timer) {
    if (!s_screen) {
        return;
    }
    if (StandbyPresentation::IsActive() || (timer && lv_screen_active() != s_screen)) {
        StopScanning();
        return;
    }
    const auto state = UiSnapshot();
    if (state.scanning || state.state == PagerScreenState::kError || state.state == PagerScreenState::kConnecting) {
        s_scan_requested = false;
    }
    PollScan(state);
    if (s_view == PagerScreenView::kLearn) {
        if (state.binding_pending) {
            return;
        }
        if (state.operation_failed) {
            Error();
            Show(PagerScreenView::kKeys);
            return;
        }
        if (!state.learning && SelectedConnected(state)) {
            Show(PagerScreenView::kKeys);
            return;
        }
        if (!state.learning || !SelectedConnected(state)) {
            Show(PagerScreenView::kDevice);
            return;
        }
        if (!s_candidate && state.learned_key) {
            s_candidate = state.learned_key;
            const int other = s_action == PagerScreenAction::kPrevious ? 1 : 0;
            const bool conflict = state.Saved()[state.active_saved].keys[other] == s_candidate;
            char text[320];
            std::snprintf(text, sizeof(text), LangString_SETTINGS_PAGER_LEARNED, 'A' + s_candidate - 1, ActionName(),
                          conflict ? LangString_SETTINGS_PAGER_REPLACE_HINT : "");
            LearnText(text);
            lv_obj_remove_state(s_save, LV_STATE_DISABLED);
        } else if (!s_candidate && esp_timer_get_time() - s_learning_since >= 20000000) {
            Show(PagerScreenView::kKeys);
            ScreenShowToast(LangString_SETTINGS_PAGER_TIMEOUT);
        }
        return;
    }
    if (state.revision == s_revision && state.state == s_presented_state) {
        return;
    }
    if (s_revision == UINT32_MAX) {
        s_revision = state.revision;
        return;
    }
    if (TouchUiFingerIsDown()) {
        return;
    }
    if (s_view == PagerScreenView::kList && s_progress == Progress(state) && (s_progress || !ListChanged(state))) {
        s_revision = state.revision;
        return;
    }
    const int index = DeviceIndex(state);
    if (s_view != PagerScreenView::kList && state.scanning && index >= 0 &&
        DeviceStatus(state, index) == s_device_status) {
        s_revision = state.revision;
        return;
    }
    Show(s_view);
}

static bool OnKey(const char* key) {
    if (std::strcmp(key, "vk_prev") == 0 || std::strcmp(key, "vk_home") == 0) {
        Back(nullptr);
        return true;
    }
    return false;
}

static void Deleted(lv_event_t* event) {
    if (lv_event_get_target(event) != s_screen) {
        return;
    }
    StopScanning();
    BlePageTurnerSetLearning(false);
    if (s_timer) {
        lv_timer_delete(s_timer);
    }
    s_timer = nullptr;
    s_screen = s_body = s_save = s_learn_message = nullptr;
    s_modal = s_card = s_modal_body = nullptr;
    std::vector<PagerScreenRowContext>().swap(s_rows);
}

lv_obj_t* PagerScreen::Create() {
    if (s_timer) {
        lv_timer_delete(s_timer);
    }
    s_timer = nullptr;
    BlePageTurnerInit(PagerKey_Receive);
    BlePageTurnerSetLearning(false);
    auto* root = lv_obj_create(nullptr);
    Strip(root);
    s_screen = root;
    s_modal = s_card = s_modal_body = nullptr;
    s_scan_requested = s_background_scan = s_initial_scan = s_scan_was_running = false;
    s_next_scan_at = 0;
    s_progress = s_device_status = nullptr;
    s_attempt_address[0] = '\0';
    s_connecting_until = 0;
    lv_obj_set_style_bg_color(root, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    auto status = ScreenCreateStatusBar(root);
    const int pad = UiSx(12);
    s_gap = UiSy(8);
    const int width = LV_HOR_RES - 2 * pad;
    const int title_y = status.height + s_gap;
    auto* heading =
        Label(root, LangString_SETTINGS_PAGER_MANAGE, title_y, UiFont()->line_height + s_gap, UiFontRole::Body);
    lv_obj_set_x(heading, pad);
    lv_obj_set_width(heading, width);
    const int top = title_y + UiFont()->line_height + 3 * s_gap;
    s_body = Container(root, width, LV_VER_RES - top - pad);
    lv_obj_set_pos(s_body, pad, top);
    lv_obj_add_event_cb(root, Deleted, LV_EVENT_DELETE, nullptr);
    lv_obj_add_event_cb(root, [](lv_event_t*) { StopScanning(); }, LV_EVENT_SCREEN_UNLOADED, nullptr);
    VkKeyScreenDesc keys{};
    keys.factory = PagerScreen::Create;
    keys.on_key = OnKey;
    VkKey_AttachScreen(root, "pager", keys);
    s_view = PagerScreenView::kList;
    s_timer = lv_timer_create(Refresh, 200, nullptr);
    const auto state = UiSnapshot();
    if (state.state != PagerScreenState::kConnecting) {
        s_initial_scan = true;
        s_scan_requested = BlePageTurnerScan(PagerKey_Receive);
        s_background_scan = s_scan_was_running = s_scan_requested;
        if (!s_scan_requested) {
            s_initial_scan = false;
        }
    }
    Show(PagerScreenView::kList);
    return root;
}
