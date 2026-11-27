#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <new>
#include <utility>
#include <esp_log.h>
#include "assets/lang_config.h"
#include "boards/common/storage/sd_access.h"
#include "infra/storage/key_bindings.h"
#include "sysmgmt/tasks/task_executor.h"
#include "ui/apps/settings/settings_keys.h"
#include "ui/apps/settings/settings_common.h"
#include "ui/apps/settings/settings_screen.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/drawing/ui_scale.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/drawing/screen_draw.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/runtime/input/binding_dispatch.h"
#include "ui/common/runtime/input/screen_gesture.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"
#include "ui/common/runtime/modal.h"
#include "ui/common/runtime/screen_common_nav.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/widgets/screen_options.h"
#include "ui/common/widgets/screen_text.h"
#include "ui/common/widgets/screen_scrollbar.h"

struct SettingsKeyActionText {
    BindingAction action;       // 固定功能标识
    const char *chinese;        // 中文名称
    const char *english;        // 英文名称
    const char *description;    // 中文用途
    const char *description_en; // 英文用途
};

static constexpr SettingsKeyActionText kActions[] = {
    {BindingAction::None, "不执行操作", "No action", "不执行操作。", "Do nothing."},
    {BindingAction::FullRefresh, "全刷", "Full refresh", "全屏刷新一次，清除残影。",
     "Refresh the screen to clear ghosting."},
    {BindingAction::Back, "返回", "Back", "返回上一层或关闭当前弹窗。",
     "Go back one level or close the dialog."},
    {BindingAction::Home, "回到首页", "Go to Home", "回到系统首页。", "Return to Home."},
    {BindingAction::VolumeUp, "音量＋", "Volume up", "增加媒体音量。", "Increase media volume."},
    {BindingAction::VolumeDown, "音量－", "Volume down", "降低媒体音量。", "Decrease media volume."},
    {BindingAction::VolumeMax, "最大音量", "Maximum volume", "设为最大音量。", "Set media volume to maximum."},
    {BindingAction::VolumeMin, "最小音量", "Minimum volume", "设为最小音量。", "Set media volume to minimum."},
    {BindingAction::LightUp, "前光调亮", "Brighter", "提高前光亮度。", "Increase frontlight brightness."},
    {BindingAction::LightDown, "前光调暗", "Dimmer", "降低前光亮度。", "Decrease frontlight brightness."},
    {BindingAction::Standby, "进入待机", "Enter standby", "让设备进入待机。", "Put the device into standby."},
    {BindingAction::Wake, "退出待机", "Exit standby", "退出待机，恢复使用。", "Wake the device from standby."},
    {BindingAction::Mute, "静音开关", "Toggle mute", "切换静音，关闭静音时恢复原音量。",
     "Mute or restore the previous volume."},
    {BindingAction::Light, "前光开关", "Toggle frontlight", "切换前光，开启时恢复原亮度。",
     "Turn the frontlight off or restore its brightness."},
    {BindingAction::Shutdown, "关机", "Power off", "关闭设备；再次开机使用电源键。",
     "Power off; turn on with the power key."},
    {BindingAction::Previous, "上一页", "Previous page", "查看上一页，按住可连续翻页。",
     "Go to the previous page; hold to repeat."},
    {BindingAction::Next, "下一页", "Next page", "查看下一页，按住可连续翻页。", "Go to the next page; hold to repeat."},
    {BindingAction::OpenAi, "快捷进入AI应用", "Open AI app", "从首页或阅读页打开 AI 应用。",
     "Open AI from Home or reading."},
    {BindingAction::Record, "按住录音", "Hold to record", "在 AI 页面按住说话，松开结束。",
     "Hold to speak on an AI page; release to finish."},
    {BindingAction::ReaderSettings, "阅读设置", "Reading settings", "在正文中展开或收起阅读设置。",
     "Show or hide settings while reading a book."},
    {BindingAction::Interrupt, "打断播报", "Stop speaking", "停止助手当前播报，回到待命。",
     "Stop assistant speech and return to idle."}};
static constexpr BindingKey kKeyOrder[] = {BindingKey::VolumeUp, BindingKey::VolumeDown, BindingKey::Boot,
                                           BindingKey::Power,    BindingKey::Previous,   BindingKey::Next,
                                           BindingKey::Home};
static constexpr const char *kKeyNames[][2] = {{"BOOT键", "BOOT key"},
                                               {"电源键", "Power key"},
                                               {"音量加键", "Volume up key"},
                                               {"音量减键", "Volume down key"},
                                               {"底部上页键", "Bottom previous key"},
                                               {"底部下页键", "Bottom next key"},
                                               {"Home键", "Home key"}};
static BindingKey s_key = BindingKey::Boot;
static bool s_long = false;
static lv_obj_t *s_editor = nullptr;
static lv_obj_t *s_body = nullptr;
static lv_obj_t *s_help = nullptr;
static UiModal s_help_modal;
static bool s_saving = false;
static lv_obj_t *s_gesture_toggle = nullptr;
struct SettingsKeysSaveCompletion : ScreenUiCompletion {
    BindingSaveResult result = BindingSaveResult::IoError; // 后台结果，经完成队列交给 UI
    uint32_t epoch = 0; // 提交时所在页面，离页后不再显示提示
};
static SettingsKeysSaveCompletion s_save_completion;
struct SettingsKeysHelpPages {
    lv_obj_t *body = nullptr; // 固定高度的分页视口
    int stride = 1; // 一页的像素高度
    int count = 1; // 总页数
    int page = 0; // 当前页，从零开始
};
static SettingsKeysHelpPages s_help_pages;

const char *SettingsKeysText(const char *chinese, const char *english) {
    return std::strncmp(LangCurrent(), "en", 2) == 0 ? english : chinese;
}

static const char *KeyName(BindingKey key) {
    const auto &names = kKeyNames[static_cast<unsigned>(key)];
    return SettingsKeysText(names[0], names[1]);
}

static const char *ActionName(const SettingsKeyActionText &action) {
    return SettingsKeysText(action.chinese, action.english);
}

static lv_obj_t *Container(lv_obj_t *parent) {
    auto *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_width(object, LV_PCT(100));
    lv_obj_set_height(object, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(object, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(object, UiSy(8), 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

static lv_obj_t *Text(lv_obj_t *parent, const char *text, bool centered = false) {
    auto *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, UiFont(), 0);
    lv_obj_set_style_text_color(label, UiColorForeground(), 0);
    lv_obj_set_style_text_align(label, centered ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
    return label;
}

static void Arrow(lv_obj_t *parent, bool left) {
    static const lv_point_precise_t right_points[] = {{0, 0}, {UiSx(5), UiSy(5)}, {0, UiSy(10)}};
    static const lv_point_precise_t left_points[] = {{UiSx(5), 0}, {0, UiSy(5)}, {UiSx(5), UiSy(10)}};
    auto *arrow = lv_line_create(parent);
    lv_line_set_points(arrow, left ? left_points : right_points, 3);
    lv_obj_set_style_line_color(arrow, UiColorForeground(), 0);
    lv_obj_set_style_line_width(arrow, UiSx(2), 0);
    lv_obj_align(arrow, left ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, left ? UiSx(10) : -UiSx(14), 0);
    lv_obj_clear_flag(arrow, LV_OBJ_FLAG_CLICKABLE);
}

static void BuildEditor();

static void RebuildAsync(void *) {
    if (s_editor && s_body && lv_screen_active() == s_editor) {
        BuildEditor();
    }
}

void SettingsKeysRefresh() {
    if (!s_gesture_toggle) {
        return;
    }
    if (!s_saving) {
        lv_obj_set_state(s_gesture_toggle, LV_STATE_CHECKED, KeyBindingsGesturesEnabled());
    }
    lv_obj_set_state(s_gesture_toggle, LV_STATE_DISABLED, s_saving);
}

static void SaveResult(BindingSaveResult result) {
    if (result == BindingSaveResult::IoError) {
        ScreenShowToast(SettingsKeysText("保存失败，请检查 SD 卡", "Could not save. Check the SD card."));
    } else if (result == BindingSaveResult::Volatile) {
        ScreenShowToast(
            SettingsKeysText("SD 卡不可用，设置仅本次生效", "SD unavailable. Changes apply to this session only."));
    } else if (result == BindingSaveResult::Invalid) {
        ScreenShowToast(SettingsKeysText("绑定已变化，请重新选择", "Assignments changed. Please choose again."));
    }
    BindingDispatchCancelAll();
    SettingsKeysRefresh();
    if (s_editor && lv_screen_active() == s_editor) {
        ScreenLvAsync(RebuildAsync);
    }
}

static void SubmitSave(std::function<BindingSaveResult()> save) try {
    if (s_saving) {
        return;
    }
    s_save_completion.epoch = VkKey_InputEpoch();
    s_save_completion.apply = [](ScreenUiCompletion *completion) {
        auto *saved = static_cast<SettingsKeysSaveCompletion *>(completion);
        s_saving = false;
        if (saved->epoch == VkKey_InputEpoch()) {
            SaveResult(saved->result);
        } else {
            BindingDispatchCancelAll();
            SettingsKeysRefresh();
        }
    };
    const unsigned generation = SdAccessMountGeneration();
    s_saving = true;
    SettingsKeysRefresh();
    if (!TaskExecutor::Schedule([save = std::move(save), generation] {
            s_save_completion.result = BindingSaveResult::IoError;
            try {
                SdAccessTransaction transaction;
                if (!transaction && transaction.Error().code != StorageErrorCode::NotReady) {
                    LogStorageError("SettingsKeys", transaction.Error());
                } else if (generation != SdAccessMountGeneration()) {
                    ESP_LOGW("SettingsKeys", "operation=save result=media_changed");
                } else {
                    s_save_completion.result = save();
                }
            } catch (const std::bad_alloc &) {
                ESP_LOGE("SettingsKeys", "operation=save result=no_memory");
            }
            ScreenCompleteWork(&s_save_completion);
        }, TaskDomain::Storage)) {
        s_saving = false;
        SaveResult(BindingSaveResult::IoError);
    }
} catch (const std::bad_alloc &) {
    s_saving = false;
    ESP_LOGE("SettingsKeys", "operation=save result=no_memory");
    SaveResult(BindingSaveResult::IoError);
}

static void Choose(lv_event_t *event) {
    if (s_saving) {
        return;
    }
    const auto action = static_cast<BindingAction>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    const BindingMask candidate = KeyBindingsCandidate(KeyBindingsGet(s_key, s_long), action);
    if (candidate == KeyBindingsGet(s_key, s_long)) {
        return;
    }
    if (!KeyBindingsCanSet(s_key, s_long, candidate)) {
        if (KeyBindingsCanBind(s_key, s_long, candidate)) {
            const bool removes_wake = (KeyBindingsGet(s_key, s_long) & BindingBit(BindingAction::Wake)) &&
                                      !(candidate & BindingBit(BindingAction::Wake));
            ScreenShowToast(removes_wake
                                ? SettingsKeysText("请先将退出待机绑定到其它实体按键",
                                                   "Assign exit standby to another physical key first.")
                                : SettingsKeysText("请先将关机绑定到其它按键",
                                                   "Assign power off to another key first."));
        }
        return;
    }
    const auto key = s_key;
    const bool long_press = s_long;
    bool duplicate = false;
    if ((KeyBindingsGet(key, long_press) & BindingBit(action)) == 0) {
        for (auto other : kKeyOrder) {
            for (bool gesture : {false, true}) {
                if (other == key && gesture == long_press) {
                    continue;
                }
                duplicate |= (KeyBindingsGet(other, gesture) & BindingBit(action)) != 0;
            }
        }
    }
    if (!duplicate) {
        SubmitSave([key, long_press, candidate] { return KeyBindingsSet(key, long_press, candidate); });
        return;
    }
    ScreenDialogAction choices[] = {{SettingsKeysText("取消", "Cancel"), {}},
                                    {SettingsKeysText("仍然设置", "Assign anyway"), [key, long_press, candidate] {
                                         SubmitSave([key, long_press, candidate] {
                                             return KeyBindingsSet(key, long_press, candidate);
                                         });
                                     }}};
    ScreenShowChoiceDialog(
        s_editor, SettingsKeysText("功能已被其他按键使用，是否共用？", "This function is already assigned. Share it?"),
        choices, 2);
}

static void Gesture(lv_event_t *event) {
    s_long = reinterpret_cast<intptr_t>(lv_event_get_user_data(event)) != 0;
    ScreenLvAsync(RebuildAsync);
}

static void StyleSection(lv_obj_t *section) {
    lv_obj_set_style_border_width(section, UiSx(1), 0);
    lv_obj_set_style_border_color(section, UiColorForeground(), 0);
    lv_obj_set_style_border_opa(section, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(section, UiSx(8), 0);
    lv_obj_set_style_pad_all(section, UiSx(8), 0);
    lv_obj_set_style_shadow_width(section, UiSx(2), 0);
    lv_obj_set_style_shadow_ofs_y(section, UiSy(1), 0);
    lv_obj_set_style_shadow_color(section, UiColorForeground(), 0);
    lv_obj_set_style_shadow_opa(section, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(section, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(section, LV_OPA_COVER, 0);
}

static void BuildEditor() {
    lv_obj_clean(s_body);
    auto *gestures = ScreenCreateOptionGrid(s_body, 2);
    StyleSection(gestures);
    for (bool long_press : {false, true}) {
        const char *title = long_press ? SettingsKeysText("长按", "Long press") : SettingsKeysText("短按", "Short press");
        auto *button = ScreenCreateSelectableOption(gestures, title, Gesture, long_press);
        lv_obj_set_style_pad_all(button, UiSx(4), 0);
        lv_obj_set_style_min_height(button, 0, 0);
        lv_obj_set_height(button, std::max<int>(UiSy(44), ScreenTextHeight(UiFont(UiFontRole::Caption)) + UiSy(12)));
        ScreenStyleSelectable(button, lv_obj_get_child(button, 0), s_long == long_press);
        lv_obj_set_style_border_width(button, UiSx(1), 0);
    }
    for (bool local : {false, true}) {
        auto *group = Container(s_body);
        StyleSection(group);
        Text(group,
             local ? SettingsKeysText("局部功能", "Page functions") : SettingsKeysText("全局功能", "Global functions"),
             true);
        auto *grid = ScreenCreateOptionGrid(group, 2);
        lv_obj_set_style_pad_row(grid, UiSy(5), 0);
        for (const auto &action : kActions) {
            const bool action_local = (BindingBit(action.action) & kBindingLocalActions) != 0;
            if (action_local != local || !KeyBindingsActionSupported(action.action)) {
                continue;
            }
            auto *button =
                ScreenCreateSelectableOption(grid, ActionName(action), Choose, static_cast<intptr_t>(action.action));
            lv_obj_set_style_pad_hor(button, UiSx(4), 0);
            lv_obj_set_style_pad_ver(button, UiSy(2), 0);
            lv_obj_set_style_min_height(button, 0, 0);
            lv_obj_set_height(button, std::max<int>(UiSy(35), ScreenTextHeight(UiFont(UiFontRole::Caption)) +
                                                                  2 * (UiSy(2) + kScreenBorderW)));
            auto *label = lv_obj_get_child(button, 0);
            lv_obj_set_style_text_font(label, UiFont(UiFontRole::Caption), 0);
            const bool selected = action.action == BindingAction::None
                                      ? KeyBindingsGet(s_key, s_long) == 0
                                      : (KeyBindingsGet(s_key, s_long) & BindingBit(action.action)) != 0;
            ScreenStyleSelectable(button, label, selected);
            lv_obj_set_style_border_width(button, UiSx(1), 0);
            if (!KeyBindingsCanBind(s_key, s_long, KeyBindingsCandidate(KeyBindingsGet(s_key, s_long), action.action))) {
                lv_obj_add_state(button, LV_STATE_DISABLED);
                lv_obj_set_style_border_width(button, 0, 0);
                lv_obj_set_style_text_opa(label, LV_OPA_50, 0);
                lv_obj_add_event_cb(button, ScreenDrawDisabledBorder, LV_EVENT_DRAW_MAIN_END, nullptr);
            }
        }
    }
}

static void CloseHelp(void *) {
    if (s_help) {
        lv_obj_delete(lv_obj_get_parent(s_help));
    }
}

static void ChangeHelpPage(int direction) {
    ScreenScrollbarShow(s_help_pages.body);
    s_help_pages.page = std::clamp(s_help_pages.page + direction, 0, s_help_pages.count - 1);
    lv_obj_scroll_to_y(s_help_pages.body, s_help_pages.page * s_help_pages.stride, LV_ANIM_OFF);
    lv_obj_invalidate(s_help_pages.body);
}

static void HelpPageGesture(lv_event_t *event) {
    auto *input = lv_indev_active();
    if (!input) {
        return;
    }
    const auto direction = lv_indev_get_gesture_dir(input);
    if (direction == LV_DIR_TOP || direction == LV_DIR_BOTTOM) {
        lv_event_stop_bubbling(event);
        lv_indev_wait_release(input);
        ChangeHelpPage(direction == LV_DIR_TOP ? 1 : -1);
    }
}

static void DrawHelpScrollbar(lv_event_t *event) {
    if (s_help_pages.count <= 1 || !ScreenScrollbarVisible(s_help_pages.body)) {
        return;
    }
    lv_area_t area;
    lv_obj_get_coords(s_help_pages.body, &area);
    const int inset = UiSy(4);
    const int track = std::max<int>(1, lv_area_get_height(&area) - 2 * inset);
    const int length = std::min(track, std::max<int>(UiSy(12), track / s_help_pages.count));
    area.y1 += inset + (track - length) * s_help_pages.page / (s_help_pages.count - 1);
    area.y2 = area.y1 + length - 1;
    area.x2 -= 1;
    area.x1 = area.x2 - UiSx(5) + 1;
    lv_draw_rect_dsc_t bar;
    lv_draw_rect_dsc_init(&bar);
    bar.bg_color = UiColorForeground();
    bar.bg_opa = LV_OPA_COVER;
    bar.radius = LV_RADIUS_CIRCLE;
    lv_draw_rect(lv_event_get_layer(event), &bar, &area);
}

static void FillHelpPage(lv_obj_t *content, uint32_t first, uint32_t end, int used) {
    if (end - first <= 1) {
        return;
    }
    const int space = s_help_pages.stride - used;
    for (uint32_t index = first + 1; index < end; ++index) {
        auto *item = lv_obj_get_child(content, index);
        lv_obj_set_y(item, lv_obj_get_style_y(item, LV_PART_MAIN) + space * (index - first) / (end - first - 1));
    }
}

static void PaginateHelp(lv_obj_t *content) {
    lv_obj_update_layout(s_help);
    s_help_pages.stride = std::max<int>(1, lv_obj_get_content_height(s_help_pages.body));
    lv_obj_set_layout(content, LV_LAYOUT_NONE);
    int y = 0;
    int page_top = 0;
    uint32_t first = 0;
    for (uint32_t index = 0; index < lv_obj_get_child_count(content); ++index) {
        auto *item = lv_obj_get_child(content, index);
        const int height = lv_obj_get_height(item);
        int required = height;
        if ((index == 0 || lv_obj_get_style_text_font(item, LV_PART_MAIN) == UiFont(UiFontRole::Emphasis)) &&
            index + 1 < lv_obj_get_child_count(content)) {
            required += UiSy(8) + lv_obj_get_height(lv_obj_get_child(content, index + 1));
        }
        if (index > first && y - page_top + required > s_help_pages.stride) {
            FillHelpPage(content, first, index, y - page_top - UiSy(8));
            page_top += s_help_pages.stride;
            first = index;
            y = page_top;
        }
        lv_obj_set_pos(item, 0, y);
        y += height + UiSy(8);
    }
    s_help_pages.count = std::max<int>(1, (y - UiSy(8) + s_help_pages.stride - 1) / s_help_pages.stride);
    lv_obj_set_height(content, s_help_pages.count * s_help_pages.stride);
    lv_obj_update_layout(s_help);
    ChangeHelpPage(0);
}

static const char* KeyCustomizationNotes() {
    return SettingsKeysText("退出待机仅支持实体键短按，底部按键和长按不可设置。\n退出待机和关机须保留按键绑定。\n按住录音仅支持非电源键长按。\n"
                          "更换必留功能前，先绑定其他按键。\n全局与局部功能可同时设置，适用页面优先执行局部功能，其他页面执行全局功能。\n"
                          "再次选择可取消该功能，“不执行操作”清空当前短按或长按绑定。",
                          "Exit standby requires a physical key's short press; bottom keys and long presses are unavailable.\n"
                          "Keep bindings for exit standby and power off.\nHold to record requires a non-power key's long press.\n"
                          "Assign a replacement before removing a required binding.\nGlobal and page functions can coexist. Page functions take priority on supported pages; "
                          "other pages use the global function.\nSelect again to remove a function; No action clears this press assignment.");
}

static void Help(lv_event_t *event) {
    if (s_help) {
        return;
    }
    auto *owner = lv_obj_get_screen(lv_event_get_target_obj(event));
    auto *mask = lv_obj_create(owner);
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(mask, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    s_help = lv_obj_create(mask);
    lv_obj_remove_style_all(s_help);
    lv_obj_set_size(s_help, LV_PCT(94), LV_PCT(92));
    lv_obj_center(s_help);
    lv_obj_set_flex_flow(s_help, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_width(s_help, UiSx(1), 0);
    lv_obj_set_style_border_color(s_help, UiColorForeground(), 0);
    lv_obj_set_style_border_opa(s_help, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_help, UiSx(8), 0);
    lv_obj_set_style_pad_all(s_help, UiSx(16), 0);
    lv_obj_set_style_pad_row(s_help, UiSy(12), 0);
    lv_obj_set_style_bg_color(s_help, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(s_help, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_help, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_help, [](lv_event_t *) {
        s_help = nullptr;
        s_help_pages = {};
    }, LV_EVENT_DELETE, nullptr);
    s_help_modal.Bind(mask, CloseHelp, nullptr);
    auto *head = Container(s_help);
    lv_obj_set_height(head, UiSy(44));
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    auto *heading = Text(head, SettingsKeysText("功能说明", "Function help"));
    lv_obj_set_width(heading, 0);
    lv_obj_set_flex_grow(heading, 1);
    lv_obj_set_style_text_font(heading, UiFont(UiFontRole::Emphasis), 0);
    auto *close = ScreenCreateSelectableOption(
        head, "×", [](lv_event_t *) { CloseHelp(nullptr); }, 0);
    lv_obj_set_style_min_height(close, 0, 0);
    lv_obj_set_size(close, kSettingsRoundButtonSize, kSettingsRoundButtonSize);
    lv_obj_set_style_pad_all(close, 0, 0);
    lv_obj_set_style_radius(close, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_ext_click_area(close, UiSx(6));
    auto *body = Container(s_help);
    lv_obj_set_height(body, 0);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_add_flag(body, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | kTouchUiPagedVerticalDrag));
    lv_obj_clear_flag(body, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_style_pad_right(body, UiSx(12), 0);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(body, HelpPageGesture, LV_EVENT_GESTURE, nullptr);
    lv_obj_add_event_cb(body, DrawHelpScrollbar, LV_EVENT_DRAW_POST_END, nullptr);
    ScreenScrollbarAttach(body);
    s_help_pages.body = body;
    auto *content = Container(body);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_CLICKABLE);
    Text(content, SettingsKeysText("注意", "Note"));
    auto *note = Text(content, KeyCustomizationNotes());
    lv_obj_set_style_text_font(note, UiFont(UiFontRole::Caption), 0);
    lv_obj_set_style_text_line_space(note, UiSy(4), 0);
    for (bool local : {false, true}) {
        auto *section_title = Text(content,
             local ? SettingsKeysText("局部功能", "Page functions") : SettingsKeysText("全局功能", "Global functions"));
        lv_obj_set_style_text_font(section_title, UiFont(UiFontRole::Emphasis), 0);
        lv_obj_set_style_pad_top(section_title, UiSy(12), 0);
        unsigned index = 0;
        for (const auto &action : kActions) {
            const bool action_local = (BindingBit(action.action) & kBindingLocalActions) != 0;
            if (action_local != local || !KeyBindingsActionSupported(action.action)) {
                continue;
            }
            char title[64];
            std::snprintf(title, sizeof(title), "%u. %s", ++index, ActionName(action));
            auto *item = Container(content);
            lv_obj_set_style_pad_ver(item, UiSy(10), 0);
            lv_obj_set_style_border_width(item, UiSx(1), 0);
            lv_obj_set_style_border_side(item, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_color(item, UiColorForeground(), 0);
            lv_obj_set_style_border_opa(item, LV_OPA_COVER, 0);
            auto *name = Text(item, title);
            lv_obj_set_style_text_font(name, UiFont(UiFontRole::CaptionEmphasis), 0);
            auto *description = Text(item, SettingsKeysText(action.description, action.description_en));
            lv_obj_set_style_text_font(description, UiFont(UiFontRole::Caption), 0);
        }
    }
    PaginateHelp(content);
}

static void Reset(lv_event_t *event) {
    if (s_saving) {
        return;
    }
    ScreenDialogAction choices[] = {
        {SettingsKeysText("取消", "Cancel"), {}},
        {SettingsKeysText("恢复默认", "Restore defaults"), [] { SubmitSave([] { return KeyBindingsReset(); }); }}};
    ScreenShowChoiceDialog(lv_obj_get_screen(lv_event_get_target_obj(event)),
                           SettingsKeysText("恢复默认设置", "Restore default settings"), choices, 2);
}

void SettingsKeysBuild(lv_obj_t *page) {
    auto *gestures = SettingsCreateSection(page, SettingsKeysText("手势开关", "Gesture switch"));
    auto *gesture_group = SettingsCreateListGroup(gestures);
    s_gesture_toggle = SettingsCreateToggle(
        gesture_group, SettingsKeysText("手势识别", "Gesture recognition"), [](lv_event_t *event) {
            if (s_saving) {
                return;
            }
            const bool enabled = lv_obj_has_state(lv_event_get_current_target_obj(event), LV_STATE_CHECKED);
            if (enabled != KeyBindingsGesturesEnabled()) {
                SubmitSave([enabled] { return KeyBindingsSetGesturesEnabled(enabled); });
            }
        });
    lv_obj_add_event_cb(s_gesture_toggle, [](lv_event_t *event) {
        if (s_gesture_toggle == lv_event_get_target_obj(event)) {
            s_gesture_toggle = nullptr;
        }
    }, LV_EVENT_DELETE, nullptr);
    SettingsKeysRefresh();

    auto *help = SettingsCreateSection(page, SettingsKeysText("功能自定义说明", "Customization guide"));
    auto *guide = SettingsCreateListGroup(help);
    SettingsCreateListRow(guide, SettingsKeysText("功能说明", "Function help"), "", Help);
    SettingsCreateSectionHint(help, SettingsKeysText("一些按键自定义功能时需要注意的事项",
                                                   "Notes on customizing key functions."));

    auto *custom = SettingsCreateSection(page, SettingsKeysText("自定义功能", "Custom functions"));
    auto *list = SettingsCreateListGroup(custom);
    for (auto key : kKeyOrder) {
        SettingsCreateListRow(list, KeyName(key), "", [](lv_event_t *event) {
            s_key = static_cast<BindingKey>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
            s_long = false;
            ScreenNavigateTo(SettingsKeysCreate, SettingsScreen::CreateKeys);
        }, static_cast<intptr_t>(key));
    }

    auto *defaults = SettingsCreateSection(page, SettingsKeysText("恢复默认设置", "Restore defaults"));
    auto *reset = SettingsCreateListGroup(defaults);
    SettingsCreateListRow(reset, SettingsKeysText("重置按键功能", "Reset key functions"), "", Reset);
}

lv_obj_t *SettingsKeysCreate() {
    BindingDispatchCancelAll();
    s_editor = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_editor, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(s_editor, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_editor, LV_OBJ_FLAG_SCROLLABLE);
    const auto bar = ScreenCreateStatusBar(s_editor);
    auto *back = lv_obj_create(s_editor);
    lv_obj_remove_style_all(back);
    lv_obj_set_pos(back, UiSx(16), bar.height + UiSy(8));
    lv_obj_set_size(back, LV_HOR_RES - UiSx(32), UiSy(48));
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(back);
    lv_obj_add_event_cb(back, [](lv_event_t *) { ScreenNavigateBack(); }, LV_EVENT_CLICKED, nullptr);
    auto *title = lv_label_create(back);
    lv_label_set_text(title, KeyName(s_key));
    lv_obj_set_style_text_font(title, UiFont(UiFontRole::Emphasis), 0);
    lv_obj_set_style_text_color(title, UiColorForeground(), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, UiSx(40), 0);
    Arrow(back, true);
    s_body = Container(s_editor);
    const int top = bar.height + UiSy(68);
    lv_obj_set_pos(s_body, UiSx(16), top);
    lv_obj_set_size(s_body, LV_HOR_RES - UiSx(32), LV_VER_RES - top - UiSy(12));
    lv_obj_add_event_cb(
        s_editor,
        [](lv_event_t *event) {
            if (lv_event_get_target_obj(event) == s_editor) {
                s_editor = nullptr;
                s_body = nullptr;
            }
        },
        LV_EVENT_DELETE, nullptr);
    VkKey_AttachScreen(s_editor, "key_settings", VkKeyScreenDesc{SettingsKeysCreate});
    ScreenSetIsHome(false);
    BuildEditor();
    lv_obj_add_event_cb(s_editor, [](lv_event_t *) { BuildEditor(); }, LV_EVENT_REFRESH, nullptr);
    return s_editor;
}
