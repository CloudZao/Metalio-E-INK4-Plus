// assistant_render.cc — split from assistant_screen.cc
#include "assistant_screen_priv.h"

#include "assistant_render.h"
#include "a2ui.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <cJSON.h>
#include <esp_log.h>

cJSON* MakeComp(const char* id, const char* type) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", id);
    cJSON_AddStringToObject(o, "component", type);
    return o;
}

// 把一页 FlowItem 编成 updateComponents，交给 a2ui 按原样式渲染。
bool BuildPageJson(const FlowList& page, std::string& out_json) {
    cJSON* root = cJSON_CreateObject();
    cJSON* body = cJSON_AddObjectToObject(root, "updateComponents");
    cJSON_AddStringToObject(body, "root", "root");
    cJSON* comps = cJSON_AddArrayToObject(body, "components");

    cJSON* col = MakeComp("root", "Column");
    cJSON_AddNumberToObject(col, "gap", kBlockGap);
    cJSON_AddNumberToObject(col, "padding", 0);
    cJSON* children = cJSON_AddArrayToObject(col, "children");
    cJSON_AddItemToArray(comps, col);

    std::vector<std::string> open_cards;  // card ids awaiting children
    int seq = 0;
    auto next_id = [&]() {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "n%d", seq++);
        return std::string(buf);
    };
    auto add_child_to_parent = [&](const std::string& id) {
        if (!open_cards.empty()) {
            // 挂到当前 Card：在 comps 里找到该 card 的 children 数组
            const char* card_id = open_cards.back().c_str();
            const cJSON* it = nullptr;
            cJSON_ArrayForEach(it, comps) {
                const cJSON* id_j = cJSON_GetObjectItemCaseSensitive(it, "id");
                if (cJSON_IsString(id_j) && id_j->valuestring &&
                    std::strcmp(id_j->valuestring, card_id) == 0) {
                    cJSON* ch = cJSON_GetObjectItemCaseSensitive(it, "children");
                    if (!cJSON_IsArray(ch)) {
                        ch = cJSON_AddArrayToObject(const_cast<cJSON*>(it), "children");
                    }
                    cJSON_AddItemToArray(ch, cJSON_CreateString(id.c_str()));
                    return;
                }
            }
        }
        cJSON_AddItemToArray(children, cJSON_CreateString(id.c_str()));
    };

    for (const auto& item : page) {
        if (item.kind == FlowKind::MsgGap) {
            continue;
        }
        if (item.kind == FlowKind::CardBegin) {
            const std::string id = next_id();
            cJSON* card = MakeComp(id.c_str(), "Card");
            cJSON_AddArrayToObject(card, "children");
            cJSON_AddItemToArray(comps, card);
            add_child_to_parent(id);
            open_cards.push_back(id);
            continue;
        }
        if (item.kind == FlowKind::CardEnd) {
            if (!open_cards.empty()) {
                open_cards.pop_back();
            }
            continue;
        }

        const std::string id = next_id();
        cJSON* comp = nullptr;
        switch (item.kind) {
            case FlowKind::Text:
                comp = MakeComp(id.c_str(), "Text");
                cJSON_AddStringToObject(comp, "text", item.text.c_str());
                cJSON_AddStringToObject(comp, "variant", item.variant.c_str());
                if (item.bold) {
                    cJSON_AddBoolToObject(comp, "bold", true);
                }
                if (!item.wrap) {
                    cJSON_AddBoolToObject(comp, "wrap", false);
                }
                break;
            case FlowKind::RichText: {
                comp = MakeComp(id.c_str(), "RichText");
                cJSON_AddStringToObject(comp, "variant", item.variant.c_str());
                cJSON* spans = cJSON_AddArrayToObject(comp, "spans");
                for (const auto& sp : item.spans) {
                    cJSON* s = cJSON_CreateObject();
                    cJSON_AddStringToObject(s, "text", sp.text.c_str());
                    if (sp.bold) {
                        cJSON_AddBoolToObject(s, "bold", true);
                    }
                    cJSON_AddItemToArray(spans, s);
                }
                break;
            }
            case FlowKind::Math: {
                auto fill_math = [&](cJSON* math) {
                    cJSON_AddStringToObject(math, "latex", item.text.c_str());
                    if (item.math_display) {
                        cJSON_AddBoolToObject(math, "display", true);
                    }
                    cJSON_AddNumberToObject(math, "width",
                                            Assistant_State().layout.viewport_w > 0 ? Assistant_State().layout.viewport_w
                                                                                  : kDefaultImgW);
                };
                if (item.center) {
                    // 与 Image 相同：分页扁平化会丢掉嵌套 Column；居中需重建 wrap
                    const std::string wrap_id = id;
                    const std::string math_id = next_id();
                    cJSON* wrap = MakeComp(wrap_id.c_str(), "Column");
                    cJSON_AddStringToObject(wrap, "align", "center");
                    cJSON_AddNumberToObject(wrap, "gap", 0);
                    cJSON_AddNumberToObject(wrap, "padding", 0);
                    cJSON* wch = cJSON_AddArrayToObject(wrap, "children");
                    cJSON_AddItemToArray(wch, cJSON_CreateString(math_id.c_str()));
                    cJSON_AddItemToArray(comps, wrap);
                    add_child_to_parent(wrap_id);

                    cJSON* math = MakeComp(math_id.c_str(), "Math");
                    fill_math(math);
                    cJSON_AddItemToArray(comps, math);
                    continue;
                }
                comp = MakeComp(id.c_str(), "Math");
                fill_math(comp);
                break;
            }
            case FlowKind::Header:
                comp = MakeComp(id.c_str(), "Header");
                cJSON_AddStringToObject(comp, "title", item.text.c_str());
                if (!item.text2.empty()) {
                    cJSON_AddStringToObject(comp, "subtitle", item.text2.c_str());
                }
                if (item.bold) {
                    cJSON_AddBoolToObject(comp, "bold", true);
                }
                break;
            case FlowKind::ListItem:
                comp = MakeComp(id.c_str(), "ListItem");
                cJSON_AddStringToObject(comp, "text", item.text.c_str());
                if (!item.text2.empty()) {
                    cJSON_AddStringToObject(comp, "hint", item.text2.c_str());
                }
                if (item.done) {
                    cJSON_AddBoolToObject(comp, "done", true);
                }
                if (item.bold) {
                    cJSON_AddBoolToObject(comp, "bold", true);
                }
                break;
            case FlowKind::Badge:
                comp = MakeComp(id.c_str(), "Badge");
                cJSON_AddStringToObject(comp, "text", item.text.c_str());
                if (item.bold) {
                    cJSON_AddBoolToObject(comp, "bold", true);
                }
                break;
            case FlowKind::Button:
                comp = MakeComp(id.c_str(), "Button");
                cJSON_AddStringToObject(comp, "text", item.text.c_str());
                if (item.bold) {
                    cJSON_AddBoolToObject(comp, "bold", true);
                }
                if (!item.action_name.empty()) {
                    cJSON* action = cJSON_AddObjectToObject(comp, "action");
                    cJSON_AddStringToObject(action, "name", item.action_name.c_str());
                }
                break;
            case FlowKind::Status:
                comp = MakeComp(id.c_str(), "Status");
                cJSON_AddStringToObject(comp, "label", item.text.c_str());
                cJSON_AddStringToObject(comp, "value", item.text2.c_str());
                if (item.bold) {
                    cJSON_AddBoolToObject(comp, "bold", true);
                }
                break;
            case FlowKind::Progress:
                comp = MakeComp(id.c_str(), "Progress");
                if (!item.text.empty()) {
                    cJSON_AddStringToObject(comp, "label", item.text.c_str());
                }
                cJSON_AddNumberToObject(comp, "value", item.pct);
                break;
            case FlowKind::Image: {
                auto fill_image = [&](cJSON* img) {
                    cJSON_AddStringToObject(img, "url", item.image_url.c_str());
                    cJSON_AddNumberToObject(img, "width", item.img_w);
                    cJSON_AddNumberToObject(img, "height", item.img_h);
                    if (!item.img_border) {
                        cJSON_AddBoolToObject(img, "border", false);
                    }
                };
                if (item.center) {
                    // 分页扁平化会丢掉嵌套 Column；居中需重建 wrap（gap/pad=0 避免撑高）
                    const std::string wrap_id = id;
                    const std::string img_id = next_id();
                    cJSON* wrap = MakeComp(wrap_id.c_str(), "Column");
                    cJSON_AddStringToObject(wrap, "align", "center");
                    cJSON_AddNumberToObject(wrap, "gap", 0);
                    cJSON_AddNumberToObject(wrap, "padding", 0);
                    cJSON* wch = cJSON_AddArrayToObject(wrap, "children");
                    cJSON_AddItemToArray(wch, cJSON_CreateString(img_id.c_str()));
                    cJSON_AddItemToArray(comps, wrap);
                    add_child_to_parent(wrap_id);

                    cJSON* img = MakeComp(img_id.c_str(), "Image");
                    fill_image(img);
                    cJSON_AddItemToArray(comps, img);
                    continue;
                }
                comp = MakeComp(id.c_str(), "Image");
                fill_image(comp);
                break;
            }
            case FlowKind::Divider:
                comp = MakeComp(id.c_str(), "Divider");
                break;
            case FlowKind::Spacer:
                comp = MakeComp(id.c_str(), "Spacer");
                cJSON_AddNumberToObject(comp, "height", item.spacer_h);
                break;
            default:
                break;
        }
        if (comp != nullptr) {
            cJSON_AddItemToArray(comps, comp);
            add_child_to_parent(id);
        }
    }

    // 未闭合 Card：保持打开即可（跨页卡片下半页）
    char* raw = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (raw == nullptr) {
        return false;
    }
    out_json.assign(raw);
    cJSON_free(raw);
    return true;
}

void Assistant_UpdatePageIndicator() {
    if (Assistant_State().chrome.page_label == nullptr || !lv_obj_is_valid(Assistant_State().chrome.page_label)) {
        return;
    }
    const int pages = static_cast<int>(Assistant_State().layout.pages.size());
    const int page = pages > 0 ? (Assistant_State().layout.page_index + 1) : 0;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d / %d", page, pages > 0 ? pages : 1);
    lv_label_set_text(Assistant_State().chrome.page_label, buf);
    if (!Assistant_State().layout.has_a2ui_content) {
        lv_obj_add_flag(Assistant_State().chrome.page_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(Assistant_State().chrome.page_label, LV_OBJ_FLAG_HIDDEN);
    }
}

void Assistant_RenderCurrentPage() {
    if (Assistant_State().chrome.content == nullptr || !lv_obj_is_valid(Assistant_State().chrome.content)) {
        return;
    }
    if (!Assistant_State().layout.a2ui_ready) {
        Assistant_EnsureA2ui(Assistant_State().chrome.scr);
    }
    if (!Assistant_State().layout.a2ui_ready) {
        return;
    }
    a2ui_set_host(Assistant_State().chrome.content);

    std::string json;
    {
        std::lock_guard<std::mutex> g(Assistant_State().stream.session_mu);
        if (Assistant_State().layout.pages.empty() || !Assistant_State().layout.has_a2ui_content) {
            lv_obj_clean(Assistant_State().chrome.content);
            Assistant_UpdatePageIndicator();
            return;
        }
        if (Assistant_State().layout.page_index < 0 || Assistant_State().layout.page_index >= static_cast<int>(Assistant_State().layout.pages.size())) {
            Assistant_State().layout.page_index = 0;
        }
        if (!BuildPageJson(Assistant_State().layout.pages[static_cast<size_t>(Assistant_State().layout.page_index)], json)) {
            ESP_LOGW(TAG, "BuildPageJson failed");
            return;
        }
    }
    const esp_err_t err = a2ui_handle_json(json.c_str());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "a2ui_handle_json page failed: %s", esp_err_to_name(err));
    }
    a2ui_take_pending_refresh();

    // a2ui root 默认 100% 高会拉满；改为随内容，避免裁切
    if (lv_obj_get_child_cnt(Assistant_State().chrome.content) > 0) {
        lv_obj_t* root = lv_obj_get_child(Assistant_State().chrome.content, 0);
        if (root != nullptr) {
            lv_obj_set_height(root, LV_SIZE_CONTENT);
            Assistant_DisableScroll(root);
        }
    }
    Assistant_DisableScroll(Assistant_State().chrome.content);
    Assistant_UpdatePageIndicator();
}

void PaintCurrentPage() {
    Assistant_RenderCurrentPage();
}

void Assistant_RequestRenderCurrentPage() {
    if (Assistant_State().stream.page_paint.paint == nullptr) {
        Assistant_State().stream.page_paint.paint = PaintCurrentPage;
    }
    ScreenPaintCoalesceRequest(&Assistant_State().stream.page_paint);
}

bool Assistant_PageRepeatStep(int page_delta) {
    bool ok = false;
    {
        std::lock_guard<std::mutex> g(Assistant_State().stream.session_mu);
        if (!Assistant_State().layout.has_a2ui_content || Assistant_State().layout.pages.empty() || page_delta == 0) {
            return false;
        }
        const int last = static_cast<int>(Assistant_State().layout.pages.size()) - 1;
        int next = Assistant_State().layout.page_index + page_delta;
        if (next < 0) {
            next = 0;
        } else if (next > last) {
            next = last;
        }
        if (next == Assistant_State().layout.page_index) {
            return false;
        }
        Assistant_State().layout.page_index = next;
        ESP_LOGI(TAG, "page-repeat %+d -> %d/%d", page_delta, Assistant_State().layout.page_index + 1, last + 1);
        ok = page_delta < 0 ? Assistant_State().layout.page_index > 0 : Assistant_State().layout.page_index < last;
    }
    Assistant_RequestRenderCurrentPage();
    return ok;
}

