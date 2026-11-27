#pragma once
#include <algorithm>
#include <lvgl.h>

/** @brief 固定底栏与可伸缩内容区；父控件拥有控件树，实例只管理布局和有效引用。 */
class ScreenFixedFooter {
public:
    /** @brief 创建尚未绑定控件树的布局实例。 */
    ScreenFixedFooter() = default;

    /** @brief 撤销实例的删除事件绑定；控件树仍由父控件持有。 */
    ~ScreenFixedFooter() {
        if (root_) {
            lv_obj_remove_event_cb_with_user_data(root_, OnDelete, this);
        }
    }
    ScreenFixedFooter(const ScreenFixedFooter&) = delete;
    ScreenFixedFooter& operator=(const ScreenFixedFooter&) = delete;

    /** @brief 在指定区域创建内容区和固定底栏；须在 UI 上下文调用。 */
    void Create(lv_obj_t* parent, int width, int height, int footer_height, lv_color_t color) {
        if (root_) {
            lv_obj_delete(root_);
        }
        root_ = lv_obj_create(parent);
        lv_obj_remove_style_all(root_);
        lv_obj_set_size(root_, width, height);
        lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_border_width(root_, 2, 0);
        lv_obj_set_style_border_color(root_, color, 0);
        lv_obj_set_style_border_post(root_, true, 0);
        lv_obj_add_event_cb(root_, OnDelete, LV_EVENT_DELETE, this);
        content_ = lv_obj_create(root_);
        lv_obj_remove_style_all(content_);
        lv_obj_set_size(content_, lv_pct(100), 0);
        lv_obj_set_flex_grow(content_, 1);
        lv_obj_clear_flag(content_, LV_OBJ_FLAG_SCROLLABLE);
        footer_ = lv_obj_create(root_);
        lv_obj_remove_style_all(footer_);
        lv_obj_set_size(footer_, lv_pct(100), std::max(1, footer_height));
        lv_obj_clear_flag(footer_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_border_side(footer_, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(footer_, 2, 0);
        lv_obj_set_style_border_color(footer_, color, 0);
        lv_obj_update_layout(root_);
    }

    /** @brief 获取固定外框，外框销毁后返回空指针。 */
    lv_obj_t* Root() const {
        return root_;
    }

    /** @brief 获取仅供页面内容滚动的区域。 */
    lv_obj_t* Content() const {
        return content_;
    }

    /** @brief 获取不参与内容滚动的操作栏。 */
    lv_obj_t* Footer() const {
        return footer_;
    }

private:
    static void OnDelete(lv_event_t* event) {
        auto* self = static_cast<ScreenFixedFooter*>(lv_event_get_user_data(event));
        self->root_ = self->content_ = self->footer_ = nullptr;
    }
    lv_obj_t* root_ = nullptr;     // 父控件持有
    lv_obj_t* content_ = nullptr;  // 剩余空间唯一分配入口
    lv_obj_t* footer_ = nullptr;   // 固定底栏及分隔线
};
