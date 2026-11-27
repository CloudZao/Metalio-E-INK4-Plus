#include <algorithm>
#include <new>
#include <string>
#include <esp_log.h>
#include "ui/common/widgets/binding_panel.h"
#include "assets/lang_screen.h"
#include "business/yunxin_binding/yunxin_binding.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/widgets/ui_qrcode.h"
#include "ui/common/drawing/ui_scale.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"

static const char* TAG = "BindingPanel";

struct BindingPanel {
    lv_obj_t* root = nullptr;             // 页面内容区内的二维码容器
    lv_obj_t* qr = nullptr;               // 当前页面的二维码控件
    lv_obj_t* hint = nullptr;             // 中英文绑定提示
    lv_timer_t* timer = nullptr;          // 跟随容器销毁的 UI 状态检查
    void (*on_bound)() = nullptr;         // 完成绑定后恢复页面业务
    std::string rendered_qr;              // 已成功编码的二维码内容
    const char* rendered_hint = nullptr;  // 已显示的语言资源
    uint32_t revision = UINT32_MAX;       // 已应用的状态版本
};

static void UpdatePanel(lv_timer_t* timer) {
    auto* panel = static_cast<BindingPanel*>(lv_timer_get_user_data(timer));
    if (!YunxinBinding::NeedsBinding()) {
        auto completed = panel->on_bound;
        lv_obj_delete(panel->root);
        if (completed) {
            completed();
        }
        return;
    }
    const auto snapshot = YunxinBinding::GetSnapshot();
    const uint32_t current_revision = snapshot.revision;
    if (current_revision == panel->revision) {
        return;
    }
    const std::string& content = snapshot.qr_content;
    bool failed = snapshot.request_failed;
    if (!content.empty() && content != panel->rendered_qr) {
        if (lv_qrcode_update(panel->qr, content.data(), content.size()) == LV_RESULT_OK) {
            panel->rendered_qr = content;
        } else {
            ESP_LOGE(TAG, "QR encoding failed bytes=%u", static_cast<unsigned>(content.size()));
            failed = true;
        }
    }
    const char* text = failed            ? LangString_YUNXIN_BINDING_RETRY
                       : content.empty() ? LangString_YUNXIN_BINDING_CHECKING
                                         : LangString_YUNXIN_BINDING_SCAN;
    if (text != panel->rendered_hint) {
        lv_label_set_text(panel->hint, text);
        panel->rendered_hint = text;
    }
    panel->revision = current_revision;
}

static void NotifyUnlinked() {
    static bool shown = false;
    if (!shown && YunxinBinding::NeedsBinding()) {
        ScreenShowToast(LangString_YUNXIN_BINDING_OPEN_AI);
        shown = true;
    }
}

static ScreenPaintCoalesce notification{.paint = NotifyUnlinked};

lv_obj_t* YunxinBindingCreatePanel(lv_obj_t* parent, void (*on_bound)()) {
    if (!parent || !YunxinBinding::NeedsBinding()) {
        return nullptr;
    }
    auto* panel = new (std::nothrow) BindingPanel;
    if (!panel) {
        ESP_LOGE(TAG, "binding panel allocation failed");
        return nullptr;
    }
    panel->on_bound = on_bound;
    panel->root = lv_obj_create(parent);
    lv_obj_remove_style_all(panel->root);
    lv_obj_set_size(panel->root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(panel->root, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(panel->root, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(panel->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel->root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(panel->root, UiSy(20), 0);
    lv_obj_clear_flag(panel->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel->root, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_FLOATING));
    lv_obj_update_layout(panel->root);
    const int height = lv_obj_get_content_height(panel->root);
    const int side =
        std::max<int>(112, std::min<int>({UiSx(280), LV_HOR_RES - UiSx(96),
                                          height - 3 * UiFont()->line_height - UiSy(20) - 2 * kUiQrFrameInset}));
    lv_obj_add_flag(panel->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(panel->root, LV_DIR_VER);
    panel->qr = UiCreateQrCode(panel->root, side);
    panel->hint = lv_label_create(panel->root);
    lv_obj_set_width(panel->hint, lv_pct(90));
    lv_obj_set_style_text_font(panel->hint, UiFont(), 0);
    lv_obj_set_style_text_color(panel->hint, UiColorForeground(), 0);
    lv_obj_set_style_text_align(panel->hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(panel->hint, LV_LABEL_LONG_WRAP);
    lv_obj_add_event_cb(
        panel->root,
        [](lv_event_t* event) {
            auto* panel = static_cast<BindingPanel*>(lv_event_get_user_data(event));
            if (panel->timer) {
                lv_timer_delete(panel->timer);
            }
            delete panel;
        },
        LV_EVENT_DELETE, panel);
    panel->timer = lv_timer_create(UpdatePanel, 250, panel);
    if (!panel->timer) {
        ESP_LOGE(TAG, "binding panel timer allocation failed");
        lv_obj_delete(panel->root);
        return nullptr;
    }
    lv_timer_ready(panel->timer);
    return panel->root;
}

void YunxinBindingNotifyUnlinked() {
    ScreenPaintCoalesceRequest(&notification);
}
