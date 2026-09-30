#include "a2ui_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a2ui_image.h"
#include "a2ui_internal.h"
#include "a2ui_math.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "haptic_feedback.h"
#include "lvgl.h"

#if !LV_USE_SPAN
#error "A2UI RichText needs CONFIG_LV_USE_SPAN=y (sdkconfig)"
#endif

static const char *TAG = "a2ui_render";

static lv_display_t *s_disp;
static const lv_font_t *s_font_regular;
static const lv_font_t *s_font_bold;
static lv_obj_t *s_host; /* if set, updateComponents cleans this instead of active screen */
static lv_obj_t *s_root;
static a2ui_refresh_t s_pending_refresh = A2UI_REFRESH_PARTIAL;

static lv_obj_t *render_parent(void)
{
    if (s_host && lv_obj_is_valid(s_host)) {
        return s_host;
    }
#if LVGL_VERSION_MAJOR >= 9
    return lv_screen_active();
#else
    return lv_scr_act();
#endif
}

typedef struct {
    char id[32];
    lv_obj_t *obj;
} a2ui_node_t;

#define A2UI_MAX_NODES 48
static a2ui_node_t s_nodes[A2UI_MAX_NODES];
static int s_node_count;

static void clear_nodes(void)
{
    s_node_count = 0;
    memset(s_nodes, 0, sizeof(s_nodes));
}

static lv_obj_t *find_node(const char *id)
{
    if (!id) {
        return NULL;
    }
    for (int i = 0; i < s_node_count; i++) {
        if (strcmp(s_nodes[i].id, id) == 0) {
            return s_nodes[i].obj;
        }
    }
    return NULL;
}

static void register_node(const char *id, lv_obj_t *obj)
{
    if (!id || !obj || s_node_count >= A2UI_MAX_NODES) {
        return;
    }
    strncpy(s_nodes[s_node_count].id, id, sizeof(s_nodes[s_node_count].id) - 1);
    s_nodes[s_node_count].obj = obj;
    s_node_count++;
}

static a2ui_refresh_t parse_refresh(const cJSON *parent)
{
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(parent, "refresh");
    if (!cJSON_IsString(r) || !r->valuestring) {
        return A2UI_REFRESH_PARTIAL;
    }
    if (strcmp(r->valuestring, "full") == 0) {
        return A2UI_REFRESH_FULL;
    }
    if (strcmp(r->valuestring, "none") == 0) {
        return A2UI_REFRESH_NONE;
    }
    return A2UI_REFRESH_PARTIAL;
}

static bool parse_bold(const cJSON *comp)
{
    const cJSON *weight = cJSON_GetObjectItemCaseSensitive(comp, "weight");
    if (cJSON_IsString(weight) && weight->valuestring) {
        if (strcmp(weight->valuestring, "bold") == 0 ||
            strcmp(weight->valuestring, "700") == 0) {
            return true;
        }
    }
    const cJSON *bold = cJSON_GetObjectItemCaseSensitive(comp, "bold");
    return cJSON_IsTrue(bold);
}

static void style_text(lv_obj_t *label, const char *variant, bool bold)
{
    const lv_font_t *font = (bold && s_font_bold) ? s_font_bold : s_font_regular;
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);

    int32_t letter_space = 0;
    if (variant && strcmp(variant, "title") == 0) {
        letter_space = 2;
    } else if (variant && strcmp(variant, "subtitle") == 0) {
        letter_space = 1;
    } else if (variant && strcmp(variant, "display") == 0) {
        letter_space = 3;
    }
    lv_obj_set_style_text_letter_space(label, letter_space, 0);
}

static lv_obj_t *create_text_obj(lv_obj_t *parent, const char *text, const char *variant,
                                 bool bold, bool full_width)
{
    if (!text) {
        text = "";
    }
    if (!variant) {
        variant = "body";
    }

    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    style_text(label, variant, bold);
    if (full_width) {
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(label, lv_pct(100));
    }
    return label;
}

static int32_t variant_letter_space(const char *variant)
{
    if (variant && strcmp(variant, "title") == 0) {
        return 2;
    }
    if (variant && strcmp(variant, "subtitle") == 0) {
        return 1;
    }
    if (variant && strcmp(variant, "display") == 0) {
        return 3;
    }
    return 0;
}

/** RichText: inline bold/normal spans that wrap like a paragraph (lv_spangroup). */
static lv_obj_t *create_rich_text(lv_obj_t *parent, const cJSON *comp)
{
    const cJSON *variant = cJSON_GetObjectItemCaseSensitive(comp, "variant");
    const char *var = cJSON_IsString(variant) ? variant->valuestring : "body";
    int32_t letter_space = variant_letter_space(var);

    lv_obj_t *spans = lv_spangroup_create(parent);
    lv_obj_set_width(spans, lv_pct(100));
    lv_obj_set_height(spans, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(spans, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spans, 0, 0);
    lv_obj_set_style_pad_all(spans, 0, 0);
    lv_spangroup_set_align(spans, LV_TEXT_ALIGN_LEFT);
    lv_spangroup_set_overflow(spans, LV_SPAN_OVERFLOW_CLIP);
    lv_spangroup_set_mode(spans, LV_SPAN_MODE_BREAK);

    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(comp, "spans");
    if (cJSON_IsArray(arr)) {
        const cJSON *item;
        cJSON_ArrayForEach(item, arr)
        {
            if (!cJSON_IsObject(item)) {
                continue;
            }
            const cJSON *text = cJSON_GetObjectItemCaseSensitive(item, "text");
            if (!cJSON_IsString(text) || !text->valuestring) {
                continue;
            }
            lv_span_t *span = lv_spangroup_add_span(spans);
            lv_span_set_text(span, text->valuestring);
            lv_style_t *st = lv_span_get_style(span);
            bool bold = parse_bold(item);
            const lv_font_t *font = (bold && s_font_bold) ? s_font_bold : s_font_regular;
            lv_style_set_text_font(st, font);
            lv_style_set_text_color(st, lv_color_black());
            lv_style_set_text_opa(st, LV_OPA_COVER);
            lv_style_set_text_letter_space(st, letter_space);
        }
    }

    lv_spangroup_refresh(spans);
    return spans;
}

static void action_cb(lv_event_t *e)
{
    const char *name = lv_event_get_user_data(e);
    if (!name) {
        return;
    }
    a2ui_dispatch_action(name);
}

static void free_action_name_cb(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static lv_obj_t *create_component(lv_obj_t *parent, const cJSON *comp)
{
    const cJSON *id_j = cJSON_GetObjectItemCaseSensitive(comp, "id");
    const cJSON *type_j = cJSON_GetObjectItemCaseSensitive(comp, "component");
    if (!cJSON_IsString(id_j) || !cJSON_IsString(type_j)) {
        return NULL;
    }
    const char *id = id_j->valuestring;
    const char *type = type_j->valuestring;

    lv_obj_t *obj = NULL;

    if (strcmp(type, "Column") == 0 || strcmp(type, "Row") == 0) {
        obj = lv_obj_create(parent);
        lv_obj_remove_style_all(obj);
        lv_obj_set_width(obj, lv_pct(100));
        lv_obj_set_height(obj, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(obj, strcmp(type, "Row") == 0 ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_column(obj, 8, 0);
        lv_obj_set_style_pad_row(obj, 8, 0);
        const cJSON *gap = cJSON_GetObjectItemCaseSensitive(comp, "gap");
        if (cJSON_IsNumber(gap)) {
            lv_obj_set_style_pad_row(obj, gap->valueint, 0);
            lv_obj_set_style_pad_column(obj, gap->valueint, 0);
        }
        const cJSON *pad = cJSON_GetObjectItemCaseSensitive(comp, "padding");
        if (cJSON_IsNumber(pad)) {
            lv_obj_set_style_pad_all(obj, pad->valueint, 0);
        }
        const cJSON *align = cJSON_GetObjectItemCaseSensitive(comp, "align");
        if (cJSON_IsString(align) && align->valuestring &&
            strcmp(align->valuestring, "center") == 0) {
            lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
        }
    } else if (strcmp(type, "Card") == 0) {
        obj = lv_obj_create(parent);
        lv_obj_set_width(obj, lv_pct(100));
        lv_obj_set_height(obj, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(obj, lv_color_black(), 0);
        lv_obj_set_style_border_width(obj, 2, 0);
        lv_obj_set_style_radius(obj, 8, 0);
        lv_obj_set_style_pad_all(obj, 12, 0);
        lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
        const cJSON *pad = cJSON_GetObjectItemCaseSensitive(comp, "padding");
        if (cJSON_IsNumber(pad)) {
            lv_obj_set_style_pad_all(obj, pad->valueint, 0);
        }
    } else if (strcmp(type, "Text") == 0) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(comp, "text");
        const cJSON *variant = cJSON_GetObjectItemCaseSensitive(comp, "variant");
        const cJSON *wrap = cJSON_GetObjectItemCaseSensitive(comp, "wrap");
        const char *var = cJSON_IsString(variant) ? variant->valuestring : "body";
        bool full_width = !cJSON_IsFalse(wrap);
        obj = create_text_obj(parent,
                              cJSON_IsString(text) ? text->valuestring : "",
                              var, parse_bold(comp), full_width);
    } else if (strcmp(type, "RichText") == 0) {
        obj = create_rich_text(parent, comp);
    } else if (strcmp(type, "Math") == 0 || strcmp(type, "Formula") == 0) {
        const cJSON *latex_j = cJSON_GetObjectItemCaseSensitive(comp, "latex");
        if (!cJSON_IsString(latex_j) || !latex_j->valuestring) {
            latex_j = cJSON_GetObjectItemCaseSensitive(comp, "text");
        }
        const cJSON *disp_j = cJSON_GetObjectItemCaseSensitive(comp, "display");
        bool display = cJSON_IsTrue(disp_j) ||
                       (cJSON_IsString(disp_j) && disp_j->valuestring &&
                        strcmp(disp_j->valuestring, "true") == 0);
        const cJSON *w_j = cJSON_GetObjectItemCaseSensitive(comp, "width");
        int max_w = cJSON_IsNumber(w_j) ? w_j->valueint : 440;
        const char *latex = cJSON_IsString(latex_j) ? latex_j->valuestring : "";
        obj = a2ui_math_create(parent, latex, display, max_w);
        if (obj) {
            lv_obj_set_width(obj, LV_SIZE_CONTENT);
            lv_obj_set_style_pad_ver(obj, 4, 0);
        }
    } else if (strcmp(type, "Image") == 0) {
        obj = lv_image_create(parent);
        /* 透明底：占位勿盖白底；像素由 a2ui_image 以 L8 填入（I1 直显本板不可用） */
        lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
        const cJSON *border = cJSON_GetObjectItemCaseSensitive(comp, "border");
        bool show_border = !cJSON_IsFalse(border);
        if (show_border) {
            lv_obj_set_style_border_color(obj, lv_color_black(), 0);
            lv_obj_set_style_border_width(obj, 2, 0);
        } else {
            lv_obj_set_style_border_width(obj, 0, 0);
        }
        lv_obj_set_style_pad_all(obj, 0, 0);

        const cJSON *w_j = cJSON_GetObjectItemCaseSensitive(comp, "width");
        const cJSON *h_j = cJSON_GetObjectItemCaseSensitive(comp, "height");
        int max_w = cJSON_IsNumber(w_j) ? w_j->valueint : 440;
        int max_h = cJSON_IsNumber(h_j) ? h_j->valueint : 280;
        if (max_w <= 0) {
            max_w = 440;
        }
        if (max_h <= 0) {
            max_h = 280;
        }
        lv_obj_set_size(obj, max_w, max_h);

        const cJSON *url = cJSON_GetObjectItemCaseSensitive(comp, "url");
        if (cJSON_IsString(url) && url->valuestring && url->valuestring[0]) {
            a2ui_image_load_async(obj, url->valuestring, max_w, max_h);
        } else {
            ESP_LOGW(TAG, "Image missing url");
        }
    } else if (strcmp(type, "Header") == 0) {
        obj = lv_obj_create(parent);
        lv_obj_remove_style_all(obj);
        lv_obj_set_width(obj, lv_pct(100));
        lv_obj_set_height(obj, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_bottom(obj, 10, 0);
        lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(obj, 2, 0);
        lv_obj_set_style_border_color(obj, lv_color_black(), 0);

        const cJSON *title = cJSON_GetObjectItemCaseSensitive(comp, "title");
        const cJSON *subtitle = cJSON_GetObjectItemCaseSensitive(comp, "subtitle");
        create_text_obj(obj,
                        cJSON_IsString(title) ? title->valuestring : "",
                        "title", parse_bold(comp), true);
        if (cJSON_IsString(subtitle) && subtitle->valuestring[0]) {
            create_text_obj(obj, subtitle->valuestring, "caption", false, true);
        }
    } else if (strcmp(type, "Badge") == 0) {
        obj = lv_obj_create(parent);
        lv_obj_set_height(obj, LV_SIZE_CONTENT);
        lv_obj_set_width(obj, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(obj, lv_color_black(), 0);
        lv_obj_set_style_border_width(obj, 2, 0);
        lv_obj_set_style_radius(obj, 6, 0);
        lv_obj_set_style_pad_hor(obj, 10, 0);
        lv_obj_set_style_pad_ver(obj, 4, 0);

        const cJSON *text = cJSON_GetObjectItemCaseSensitive(comp, "text");
        create_text_obj(obj,
                        cJSON_IsString(text) ? text->valuestring : "",
                        "caption", parse_bold(comp), false);
    } else if (strcmp(type, "ListItem") == 0) {
        obj = lv_obj_create(parent);
        lv_obj_set_width(obj, lv_pct(100));
        lv_obj_set_height(obj, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(obj, lv_color_black(), 0);
        lv_obj_set_style_border_width(obj, 2, 0);
        lv_obj_set_style_radius(obj, 6, 0);
        lv_obj_set_style_pad_all(obj, 10, 0);
        lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(obj, 10, 0);

        const cJSON *done = cJSON_GetObjectItemCaseSensitive(comp, "done");
        bool is_done = cJSON_IsTrue(done);
        lv_obj_t *mark = lv_label_create(obj);
        lv_label_set_text(mark, is_done ? "[x]" : "[ ]");
        style_text(mark, "body", false);

        lv_obj_t *col = lv_obj_create(obj);
        lv_obj_remove_style_all(col);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_width(col, lv_pct(100));
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col, 2, 0);

        const cJSON *text = cJSON_GetObjectItemCaseSensitive(comp, "text");
        const cJSON *hint = cJSON_GetObjectItemCaseSensitive(comp, "hint");
        create_text_obj(col,
                        cJSON_IsString(text) ? text->valuestring : "",
                        "body", !is_done && parse_bold(comp), true);
        if (cJSON_IsString(hint) && hint->valuestring[0]) {
            create_text_obj(col, hint->valuestring, "caption", false, true);
        }
    } else if (strcmp(type, "Progress") == 0) {
        obj = lv_obj_create(parent);
        lv_obj_remove_style_all(obj);
        lv_obj_set_width(obj, lv_pct(100));
        lv_obj_set_height(obj, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(obj, 6, 0);

        const cJSON *label_j = cJSON_GetObjectItemCaseSensitive(comp, "label");
        const cJSON *value_j = cJSON_GetObjectItemCaseSensitive(comp, "value");
        int pct = cJSON_IsNumber(value_j) ? value_j->valueint : 0;
        if (pct < 0) {
            pct = 0;
        }
        if (pct > 100) {
            pct = 100;
        }

        if (cJSON_IsString(label_j) && label_j->valuestring[0]) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%s  %d%%", label_j->valuestring, pct);
            create_text_obj(obj, buf, "caption", false, true);
        }

        lv_obj_t *track = lv_obj_create(obj);
        lv_obj_set_size(track, lv_pct(100), 18);
        lv_obj_set_style_bg_color(track, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(track, lv_color_black(), 0);
        lv_obj_set_style_border_width(track, 2, 0);
        lv_obj_set_style_radius(track, 4, 0);
        lv_obj_set_style_pad_all(track, 2, 0);

        lv_obj_t *fill = lv_obj_create(track);
        if (pct <= 0) {
            lv_obj_set_size(fill, 0, lv_pct(100));
        } else {
            lv_obj_set_size(fill, lv_pct(pct), lv_pct(100));
        }
        lv_obj_set_style_bg_color(fill, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(fill, 0, 0);
        lv_obj_set_style_radius(fill, 2, 0);
        lv_obj_align(fill, LV_ALIGN_LEFT_MID, 0, 0);
    } else if (strcmp(type, "Button") == 0) {
        obj = lv_button_create(parent);
        lv_obj_set_width(obj, lv_pct(100));
        lv_obj_set_height(obj, 56);
        lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(obj, lv_color_black(), 0);
        lv_obj_set_style_border_width(obj, 2, 0);
        lv_obj_set_style_radius(obj, 8, 0);

        const cJSON *text = cJSON_GetObjectItemCaseSensitive(comp, "text");
        lv_obj_t *label = lv_label_create(obj);
        lv_label_set_text(label, cJSON_IsString(text) ? text->valuestring : "OK");
        style_text(label, "body", parse_bold(comp));
        lv_obj_center(label);

        const cJSON *action = cJSON_GetObjectItemCaseSensitive(comp, "action");
        const cJSON *aname = action ? cJSON_GetObjectItemCaseSensitive(action, "name") : NULL;
        if (cJSON_IsString(aname) && aname->valuestring) {
            char *name_copy = strdup(aname->valuestring);
            if (name_copy != NULL) {
                HapticAttachClick(obj);
                lv_obj_add_event_cb(obj, action_cb, LV_EVENT_CLICKED, name_copy);
                lv_obj_add_event_cb(obj, free_action_name_cb, LV_EVENT_DELETE, name_copy);
            }
        }
    } else if (strcmp(type, "Divider") == 0) {
        obj = lv_obj_create(parent);
        lv_obj_remove_style_all(obj);
        lv_obj_set_size(obj, lv_pct(100), 2);
        lv_obj_set_style_bg_color(obj, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    } else if (strcmp(type, "Spacer") == 0) {
        obj = lv_obj_create(parent);
        lv_obj_remove_style_all(obj);
        const cJSON *h = cJSON_GetObjectItemCaseSensitive(comp, "height");
        lv_obj_set_size(obj, lv_pct(100), cJSON_IsNumber(h) ? h->valueint : 16);
    } else if (strcmp(type, "Status") == 0) {
        obj = lv_obj_create(parent);
        lv_obj_remove_style_all(obj);
        lv_obj_set_width(obj, lv_pct(100));
        lv_obj_set_height(obj, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        const cJSON *label = cJSON_GetObjectItemCaseSensitive(comp, "label");
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(comp, "value");
        create_text_obj(obj,
                        cJSON_IsString(label) ? label->valuestring : "",
                        "caption", false, false);
        create_text_obj(obj,
                        cJSON_IsString(value) ? value->valuestring : "",
                        "body", parse_bold(comp), false);
    } else {
        ESP_LOGW(TAG, "unknown component: %s", type);
        return NULL;
    }

    register_node(id, obj);
    return obj;
}

static void attach_children(const cJSON *components)
{
    const cJSON *comp;
    cJSON_ArrayForEach(comp, components)
    {
        const cJSON *id_j = cJSON_GetObjectItemCaseSensitive(comp, "id");
        if (!cJSON_IsString(id_j)) {
            continue;
        }
        lv_obj_t *parent_obj = find_node(id_j->valuestring);
        if (!parent_obj) {
            continue;
        }

        const cJSON *children = cJSON_GetObjectItemCaseSensitive(comp, "children");
        if (cJSON_IsArray(children)) {
            const cJSON *cid;
            cJSON_ArrayForEach(cid, children)
            {
                if (!cJSON_IsString(cid)) {
                    continue;
                }
                lv_obj_t *child = find_node(cid->valuestring);
                if (child && lv_obj_get_parent(child) != parent_obj) {
                    lv_obj_set_parent(child, parent_obj);
                }
            }
        }

        const cJSON *child_one = cJSON_GetObjectItemCaseSensitive(comp, "child");
        if (cJSON_IsString(child_one)) {
            lv_obj_t *child = find_node(child_one->valuestring);
            if (child && lv_obj_get_parent(child) != parent_obj) {
                lv_obj_set_parent(child, parent_obj);
            }
        }
    }
}

static esp_err_t handle_update_components(const cJSON *body)
{
    const cJSON *comps = cJSON_GetObjectItemCaseSensitive(body, "components");
    if (!cJSON_IsArray(comps)) {
        return ESP_ERR_INVALID_ARG;
    }

    s_pending_refresh = parse_refresh(body);

    /* Caller already holds LVGL lock (e.g. DisplayLockGuard). Do not re-lock or refresh_now. */
    lv_obj_t *parent = render_parent();
    if (!parent) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Lifecycle under LVGL lock (worker blocks on same lock in apply_image):
     *   abort → destroy objs → free pixels → create → load_async
     */
    a2ui_image_abort_loads();
    lv_obj_clean(parent);
    clear_nodes();
    a2ui_image_clear();

    lv_obj_set_style_bg_color(parent, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    /* First pass: create all nodes under host/screen */
    const cJSON *comp;
    cJSON_ArrayForEach(comp, comps)
    {
        create_component(parent, comp);
    }
    attach_children(comps);

    const cJSON *root = cJSON_GetObjectItemCaseSensitive(body, "root");
    if (cJSON_IsString(root)) {
        s_root = find_node(root->valuestring);
        if (s_root) {
            /* 高度随内容，避免长文被 100% 视口裁切 */
            lv_obj_set_width(s_root, lv_pct(100));
            lv_obj_set_height(s_root, LV_SIZE_CONTENT);
        }
    }

    (void)s_disp;
    return ESP_OK;
}

static esp_err_t handle_create_surface(const cJSON *body)
{
    (void)body;
    ESP_LOGI(TAG, "createSurface");
    return ESP_OK;
}

void a2ui_render_init(lv_display_t *disp, const lv_font_t *font_regular,
                      const lv_font_t *font_bold)
{
    s_disp = disp;
    s_font_regular = font_regular;
    s_font_bold = font_bold ? font_bold : font_regular;
    s_host = NULL;
    a2ui_image_set_disp(disp);
}

void a2ui_render_set_host(lv_obj_t *host)
{
    s_host = host;
}

a2ui_refresh_t a2ui_render_take_pending_refresh(void)
{
    a2ui_refresh_t r = s_pending_refresh;
    s_pending_refresh = A2UI_REFRESH_PARTIAL;
    return r;
}

esp_err_t a2ui_render_handle_json(const char *json)
{
    ESP_RETURN_ON_FALSE(json, ESP_ERR_INVALID_ARG, TAG, "null json");
    cJSON *root = cJSON_Parse(json);
    ESP_RETURN_ON_FALSE(root, ESP_ERR_INVALID_ARG, TAG, "parse fail");

    esp_err_t ret = ESP_OK;
    if (cJSON_GetObjectItemCaseSensitive(root, "welcome")) {
        const cJSON *welcome = cJSON_GetObjectItemCaseSensitive(root, "welcome");
        const cJSON *origin = cJSON_GetObjectItemCaseSensitive(welcome, "httpOrigin");
        if (cJSON_IsString(origin) && origin->valuestring && origin->valuestring[0]) {
            a2ui_image_set_http_origin(origin->valuestring);
            ESP_LOGI(TAG, "welcome httpOrigin=%s", origin->valuestring);
        }
    } else if (cJSON_GetObjectItemCaseSensitive(root, "createSurface")) {
        ret = handle_create_surface(cJSON_GetObjectItemCaseSensitive(root, "createSurface"));
    } else if (cJSON_GetObjectItemCaseSensitive(root, "updateComponents")) {
        ret = handle_update_components(cJSON_GetObjectItemCaseSensitive(root, "updateComponents"));
    } else if (cJSON_GetObjectItemCaseSensitive(root, "updateDataModel")) {
        ESP_LOGI(TAG, "updateDataModel ignored (render via components)");
    } else if (cJSON_GetObjectItemCaseSensitive(root, "ping")) {
        ESP_LOGD(TAG, "ping");
    } else if (cJSON_GetObjectItemCaseSensitive(root, "deleteSurface")) {
        a2ui_image_abort_loads();
        lv_obj_t *parent = render_parent();
        if (parent) {
            lv_obj_clean(parent);
            clear_nodes();
        }
        a2ui_image_clear();
    } else {
        ESP_LOGW(TAG, "unknown message keys");
    }

    cJSON_Delete(root);
    return ret;
}
