// assistant_ingest.cc — split from assistant_screen.cc
#include "assistant_screen_priv.h"

#include "assistant_ingest.h"
#include "assistant_chat_store.h"
#include "assistant_screen.h"
#include "board.h"
#include "display.h"

#include <cstring>
#include <string>

#include <cJSON.h>
#include <esp_log.h>

bool ParseBold(const cJSON* comp) {
    const cJSON* weight = cJSON_GetObjectItemCaseSensitive(comp, "weight");
    if (cJSON_IsString(weight) && weight->valuestring) {
        if (std::strcmp(weight->valuestring, "bold") == 0 ||
            std::strcmp(weight->valuestring, "700") == 0) {
            return true;
        }
    }
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(comp, "bold"));
}

cJSON* FindComponentById(cJSON* comps, const char* id) {
    if (!cJSON_IsArray(comps) || id == nullptr) {
        return nullptr;
    }
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, comps) {
        if (!cJSON_IsObject(item)) {
            continue;
        }
        const cJSON* id_j = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (cJSON_IsString(id_j) && id_j->valuestring && std::strcmp(id_j->valuestring, id) == 0) {
            return const_cast<cJSON*>(item);
        }
    }
    return nullptr;
}

bool ParseAlignCenter(const cJSON* comp) {
    const cJSON* align = cJSON_GetObjectItemCaseSensitive(comp, "align");
    return cJSON_IsString(align) && align->valuestring &&
           std::strcmp(align->valuestring, "center") == 0;
}

void WalkToFlow(cJSON* comps, cJSON* comp, FlowList& out, bool parent_center = false) {
    if (!cJSON_IsObject(comp)) {
        return;
    }
    const cJSON* type_j = cJSON_GetObjectItemCaseSensitive(comp, "component");
    if (!cJSON_IsString(type_j) || type_j->valuestring == nullptr) {
        return;
    }
    const char* type = type_j->valuestring;

    if (std::strcmp(type, "Column") == 0 || std::strcmp(type, "Row") == 0) {
        const bool center = parent_center || ParseAlignCenter(comp);
        const cJSON* children = cJSON_GetObjectItemCaseSensitive(comp, "children");
        if (cJSON_IsArray(children)) {
            const cJSON* cid = nullptr;
            cJSON_ArrayForEach(cid, children) {
                if (cJSON_IsString(cid) && cid->valuestring) {
                    WalkToFlow(comps, FindComponentById(comps, cid->valuestring), out, center);
                }
            }
        }
        const cJSON* child_one = cJSON_GetObjectItemCaseSensitive(comp, "child");
        if (cJSON_IsString(child_one) && child_one->valuestring) {
            WalkToFlow(comps, FindComponentById(comps, child_one->valuestring), out, center);
        }
        return;
    }

    if (std::strcmp(type, "Card") == 0) {
        FlowItem begin;
        begin.kind = FlowKind::CardBegin;
        out.push_back(std::move(begin));
        const cJSON* children = cJSON_GetObjectItemCaseSensitive(comp, "children");
        if (cJSON_IsArray(children)) {
            const cJSON* cid = nullptr;
            cJSON_ArrayForEach(cid, children) {
                if (cJSON_IsString(cid) && cid->valuestring) {
                    WalkToFlow(comps, FindComponentById(comps, cid->valuestring), out,
                               parent_center);
                }
            }
        }
        const cJSON* child_one = cJSON_GetObjectItemCaseSensitive(comp, "child");
        if (cJSON_IsString(child_one) && child_one->valuestring) {
            WalkToFlow(comps, FindComponentById(comps, child_one->valuestring), out, parent_center);
        }
        FlowItem end;
        end.kind = FlowKind::CardEnd;
        out.push_back(std::move(end));
        return;
    }

    FlowItem item;
    if (std::strcmp(type, "Text") == 0) {
        item.kind = FlowKind::Text;
        const cJSON* text = cJSON_GetObjectItemCaseSensitive(comp, "text");
        const cJSON* variant = cJSON_GetObjectItemCaseSensitive(comp, "variant");
        const cJSON* wrap = cJSON_GetObjectItemCaseSensitive(comp, "wrap");
        item.text = cJSON_IsString(text) ? text->valuestring : "";
        item.variant = cJSON_IsString(variant) ? variant->valuestring : "body";
        item.bold = ParseBold(comp);
        item.wrap = !cJSON_IsFalse(wrap);
        out.push_back(std::move(item));
    } else if (std::strcmp(type, "RichText") == 0) {
        item.kind = FlowKind::RichText;
        const cJSON* variant = cJSON_GetObjectItemCaseSensitive(comp, "variant");
        item.variant = cJSON_IsString(variant) ? variant->valuestring : "body";
        const cJSON* arr = cJSON_GetObjectItemCaseSensitive(comp, "spans");
        if (cJSON_IsArray(arr)) {
            const cJSON* sp = nullptr;
            cJSON_ArrayForEach(sp, arr) {
                if (!cJSON_IsObject(sp)) {
                    continue;
                }
                const cJSON* text = cJSON_GetObjectItemCaseSensitive(sp, "text");
                if (!cJSON_IsString(text) || !text->valuestring) {
                    continue;
                }
                SpanRun run;
                run.text = text->valuestring;
                run.bold = ParseBold(sp);
                item.spans.push_back(std::move(run));
            }
        }
        out.push_back(std::move(item));
    } else if (std::strcmp(type, "Header") == 0) {
        item.kind = FlowKind::Header;
        const cJSON* title = cJSON_GetObjectItemCaseSensitive(comp, "title");
        const cJSON* subtitle = cJSON_GetObjectItemCaseSensitive(comp, "subtitle");
        item.text = cJSON_IsString(title) ? title->valuestring : "";
        item.text2 = cJSON_IsString(subtitle) ? subtitle->valuestring : "";
        item.bold = ParseBold(comp);
        out.push_back(std::move(item));
    } else if (std::strcmp(type, "ListItem") == 0) {
        item.kind = FlowKind::ListItem;
        const cJSON* text = cJSON_GetObjectItemCaseSensitive(comp, "text");
        const cJSON* hint = cJSON_GetObjectItemCaseSensitive(comp, "hint");
        item.text = cJSON_IsString(text) ? text->valuestring : "";
        item.text2 = cJSON_IsString(hint) ? hint->valuestring : "";
        item.done = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(comp, "done"));
        item.bold = ParseBold(comp);
        out.push_back(std::move(item));
    } else if (std::strcmp(type, "Badge") == 0) {
        item.kind = FlowKind::Badge;
        const cJSON* text = cJSON_GetObjectItemCaseSensitive(comp, "text");
        item.text = cJSON_IsString(text) ? text->valuestring : "";
        item.bold = ParseBold(comp);
        out.push_back(std::move(item));
    } else if (std::strcmp(type, "Button") == 0) {
        item.kind = FlowKind::Button;
        const cJSON* text = cJSON_GetObjectItemCaseSensitive(comp, "text");
        item.text = cJSON_IsString(text) ? text->valuestring : "OK";
        item.bold = ParseBold(comp);
        const cJSON* action = cJSON_GetObjectItemCaseSensitive(comp, "action");
        const cJSON* aname = action ? cJSON_GetObjectItemCaseSensitive(action, "name") : nullptr;
        if (cJSON_IsString(aname) && aname->valuestring) {
            item.action_name = aname->valuestring;
        }
        out.push_back(std::move(item));
    } else if (std::strcmp(type, "Status") == 0) {
        item.kind = FlowKind::Status;
        const cJSON* label = cJSON_GetObjectItemCaseSensitive(comp, "label");
        const cJSON* value = cJSON_GetObjectItemCaseSensitive(comp, "value");
        item.text = cJSON_IsString(label) ? label->valuestring : "";
        item.text2 = cJSON_IsString(value) ? value->valuestring : "";
        item.bold = ParseBold(comp);
        out.push_back(std::move(item));
    } else if (std::strcmp(type, "Progress") == 0) {
        item.kind = FlowKind::Progress;
        const cJSON* label = cJSON_GetObjectItemCaseSensitive(comp, "label");
        const cJSON* value = cJSON_GetObjectItemCaseSensitive(comp, "value");
        item.text = cJSON_IsString(label) ? label->valuestring : "";
        item.pct = cJSON_IsNumber(value) ? value->valueint : 0;
        out.push_back(std::move(item));
    } else if (std::strcmp(type, "Math") == 0 || std::strcmp(type, "Formula") == 0) {
        item.kind = FlowKind::Math;
        const cJSON* latex = cJSON_GetObjectItemCaseSensitive(comp, "latex");
        if (!cJSON_IsString(latex) || !latex->valuestring) {
            latex = cJSON_GetObjectItemCaseSensitive(comp, "text");
        }
        item.text = cJSON_IsString(latex) ? latex->valuestring : "";
        const cJSON* disp = cJSON_GetObjectItemCaseSensitive(comp, "display");
        item.math_display = cJSON_IsTrue(disp) ||
                            (cJSON_IsString(disp) && disp->valuestring &&
                             std::strcmp(disp->valuestring, "true") == 0);
        item.center = parent_center || ParseAlignCenter(comp);
        out.push_back(std::move(item));
    } else if (std::strcmp(type, "Image") == 0) {
        item.kind = FlowKind::Image;
        const cJSON* url = cJSON_GetObjectItemCaseSensitive(comp, "url");
        const cJSON* w = cJSON_GetObjectItemCaseSensitive(comp, "width");
        const cJSON* h = cJSON_GetObjectItemCaseSensitive(comp, "height");
        const cJSON* border = cJSON_GetObjectItemCaseSensitive(comp, "border");
        item.image_url = cJSON_IsString(url) ? url->valuestring : "";
        item.img_w = cJSON_IsNumber(w) ? w->valueint : kDefaultImgW;
        item.img_h = cJSON_IsNumber(h) ? h->valueint : kDefaultImgH;
        item.img_border = !cJSON_IsFalse(border);
        item.center = parent_center || ParseAlignCenter(comp);
        out.push_back(std::move(item));
    } else if (std::strcmp(type, "Divider") == 0) {
        item.kind = FlowKind::Divider;
        out.push_back(std::move(item));
    } else if (std::strcmp(type, "Spacer") == 0) {
        item.kind = FlowKind::Spacer;
        const cJSON* h = cJSON_GetObjectItemCaseSensitive(comp, "height");
        item.spacer_h = cJSON_IsNumber(h) ? h->valueint : kDefaultSpacerH;
        out.push_back(std::move(item));
    } else {
        ESP_LOGW(TAG, "skip unknown component: %s", type);
    }
}

IngestResult Assistant_IngestA2uiMessage(cJSON* msg, FlowList& out) {
    if (!cJSON_IsObject(msg)) {
        return IngestResult::kNone;
    }
    if (cJSON_GetObjectItemCaseSensitive(msg, "deleteSurface")) {
        return IngestResult::kClear;
    }
    const cJSON* body = cJSON_GetObjectItemCaseSensitive(msg, "updateComponents");
    if (!cJSON_IsObject(body)) {
        return IngestResult::kNone;
    }
    const cJSON* comps = cJSON_GetObjectItemCaseSensitive(body, "components");
    if (!cJSON_IsArray(comps)) {
        return IngestResult::kNone;
    }
    const size_t before = out.size();
    const cJSON* root = cJSON_GetObjectItemCaseSensitive(body, "root");
    if (cJSON_IsString(root) && root->valuestring) {
        WalkToFlow(const_cast<cJSON*>(comps),
                   FindComponentById(const_cast<cJSON*>(comps), root->valuestring), out);
    } else if (cJSON_GetArraySize(comps) > 0) {
        WalkToFlow(const_cast<cJSON*>(comps), cJSON_GetArrayItem(comps, 0), out);
    }
    return out.size() > before ? IngestResult::kAppended : IngestResult::kNone;
}

void Assistant_PersistTurnJson(cJSON* msg)
{
    if (Assistant_State().layout.replaying_history || msg == nullptr) {
        return;
    }
    if (!assistant_chat_store_is_ready()) {
        return;
    }
    char* raw = cJSON_PrintUnformatted(msg);
    if (raw == nullptr) {
        ESP_LOGW(TAG, "persist: PrintUnformatted failed");
        return;
    }
    const esp_err_t err = assistant_chat_store_append(raw, std::strlen(raw));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "persist failed: %s", esp_err_to_name(err));
    }
    cJSON_free(raw);
}

void AssistantScreen::AddMessage(const char* role, const char* content) {
    ESP_LOGI(TAG, "AddMessage role=%s content=%s",
             role != nullptr ? role : "(null)",
             content != nullptr ? content : "(null)");
    if (!IsActive()) {
        return;
    }
    if (content == nullptr || content[0] == '\0') {
        return;
    }

    const char* p = content;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        ++p;
    }
    if (*p != '{' && *p != '[') {
        ESP_LOGW(TAG, "AddMessage: content is not A2UI JSON, skip (role=%s len=%u)",
                 role != nullptr ? role : "?", static_cast<unsigned>(std::strlen(content)));
        return;
    }

    cJSON* root = cJSON_Parse(content);
    if (root == nullptr) {
        ESP_LOGW(TAG, "AddMessage: cJSON_Parse failed");
        return;
    }

    FlowList chunk;
    bool cleared = false;
    auto feed_one = [&](cJSON* msg) {
        FlowList part;
        const IngestResult r = Assistant_IngestA2uiMessage(msg, part);
        if (r == IngestResult::kClear) {
            assistant_chat_store_wipe();
            cleared = true;
            chunk.clear();
            return;
        }
        if (r == IngestResult::kAppended) {
            Assistant_PersistTurnJson(msg);
            for (auto& it : part) {
                chunk.push_back(std::move(it));
            }
        }
    };

    if (cJSON_IsArray(root)) {
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, root) {
            feed_one(const_cast<cJSON*>(item));
        }
    } else if (cJSON_IsObject(root)) {
        feed_one(root);
    } else {
        ESP_LOGW(TAG, "AddMessage: unexpected JSON type");
    }
    cJSON_Delete(root);

    if (cleared) {
        Display* display = Board::GetInstance().GetDisplay();
        if (display == nullptr) {
            return;
        }
        DisplayLockGuard lock(display);
        if (!IsActive()) {
            return;
        }
        Assistant_ClearSession();
        Assistant_ShowIdleHint();
        return;
    }
    if (chunk.empty()) {
        return;
    }
    const bool jump_user = role != nullptr && std::strcmp(role, "user") == 0;
    Assistant_QueueOrShowChunk(std::move(chunk), jump_user);
}
