#include "settings_conversation_tab.h"

#include "api_endpoints.h"
#include "api_http.h"
#include "assets/lang_config.h"
#include "board.h"
#include "fontpack_lvgl.h"
#include "power_policy.h"
#include "settings_common.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

static constexpr const char* TAG = "SettingsConv";
static constexpr int kHttpTimeoutMs = 15000;
static constexpr int kWorkerStack = 8 * 1024;

struct SettingsConversationOption {
    bool enabled;
};

static constexpr SettingsConversationOption kOptions[] = {
    {true},
    {false},
};
static constexpr int kOptionCount = sizeof(kOptions) / sizeof(kOptions[0]);

struct SettingsConversationUi {
    lv_obj_t* btns[kOptionCount] = {};
    lv_obj_t* current_lbl = nullptr;
    lv_obj_t* status_lbl = nullptr;
    bool alive = false;
    bool known = false;
    bool enabled = true;
};

static SettingsConversationUi s_ui;
static std::atomic<bool> s_busy{false};
static TaskHandle_t s_worker = nullptr;

struct SettingsTtsResultMsg {
    bool ok = false;
    bool enabled = false;
    char err[64] = {};
};

static const char* OptionTitle(bool enabled) {
    return enabled ? Lang::Strings::SETTINGS_CONV_TTS_ON : Lang::Strings::SETTINGS_CONV_TTS_OFF;
}
static const char* CurrentText(bool enabled) {
    return enabled ? Lang::Strings::SETTINGS_CONV_CUR_ON : Lang::Strings::SETTINGS_CONV_CUR_OFF;
}

static void SetStatus(const char* text) {
    if (s_ui.status_lbl != nullptr) {
        lv_label_set_text(s_ui.status_lbl, text != nullptr ? text : "");
    }
}

static void RefreshOptions() {
    for (int i = 0; i < kOptionCount; ++i) {
        if (s_ui.btns[i] == nullptr) {
            continue;
        }
        const bool selected = s_ui.known && (kOptions[i].enabled == s_ui.enabled);
        SettingsStyleSelectable(s_ui.btns[i], lv_obj_get_child(s_ui.btns[i], 0), selected);
        if (s_busy.load()) {
            lv_obj_clear_flag(s_ui.btns[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_state(s_ui.btns[i], LV_STATE_DISABLED);
        } else {
            lv_obj_add_flag(s_ui.btns[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_state(s_ui.btns[i], LV_STATE_DISABLED);
        }
    }
    if (s_ui.current_lbl != nullptr) {
        if (!s_ui.known) {
            lv_label_set_text(s_ui.current_lbl, Lang::Strings::SETTINGS_CONV_CUR_DASH);
        } else {
            lv_label_set_text(s_ui.current_lbl, CurrentText(s_ui.enabled));
        }
    }
}

static bool ParseTtsEnabled(const std::string& body, bool* enabled_out, std::string& err_out) {
    if (enabled_out == nullptr) {
        return false;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        err_out = Lang::Strings::SETTINGS_CONV_PARSE_FAIL;
        return false;
    }
    const cJSON* code = cJSON_GetObjectItem(root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 0) {
        const cJSON* msg = cJSON_GetObjectItem(root, "msg");
        err_out = cJSON_IsString(msg) && msg->valuestring != nullptr ? msg->valuestring
                                                                     : Lang::Strings::SETTINGS_CONV_REQUEST_FAIL;
        cJSON_Delete(root);
        return false;
    }
    const cJSON* data = cJSON_GetObjectItem(root, "data");
    const cJSON* en = data != nullptr ? cJSON_GetObjectItem(data, "enabled") : nullptr;
    if (!cJSON_IsBool(en)) {
        err_out = Lang::Strings::SETTINGS_CONV_MISSING_ENABLED;
        cJSON_Delete(root);
        return false;
    }
    *enabled_out = cJSON_IsTrue(en);
    cJSON_Delete(root);
    return true;
}

static bool HttpJson(const char* method, const std::string& url, const char* body_in, std::string& body_out,
                     std::string& err_out) {
    body_out.clear();
    if (url.empty()) {
        err_out = "api not configured";
        ESP_LOGI(TAG, "skip tts settings: cloud endpoints blank");
        return false;
    }
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        err_out = Lang::Strings::SETTINGS_CONV_NO_NETWORK;
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        err_out = Lang::Strings::SETTINGS_CONV_CONN_FAIL;
        return false;
    }
    http->SetTimeout(kHttpTimeoutMs);
    if (body_in != nullptr) {
        api::ApplyJsonHeaders(http);
        http->SetContent(std::string(body_in));
    } else {
        api::ApplyCommonHeaders(http);
    }
    // 与 curl 示例一致：URL 也带 Device-Id
    std::string full = url;
    if (full.find('?') == std::string::npos) {
        full += "?Device-Id=";
        full += api::DeviceId();
    }
    api::LogHttpRequest(TAG, method, full, body_in != nullptr ? body_in : "");
    if (!http->Open(method, full)) {
        err_out = Lang::Strings::SETTINGS_CONV_REQUEST_FAIL;
        api::LogHttpResponse(TAG, -1, err_out);
        return false;
    }
    const int status = http->GetStatusCode();
    body_out = http->ReadAll();
    http->Close();
    api::LogHttpResponse(TAG, status, api::RedactClawUrlsForLog(body_out));
    if (status < 200 || status >= 300) {
        err_out = body_out.empty() ? ("HTTP " + std::to_string(status)) : Lang::Strings::SETTINGS_CONV_REQUEST_FAIL;
        return false;
    }
    return true;
}

static void AsyncApplyResult(void* p) {
    auto* msg = static_cast<SettingsTtsResultMsg*>(p);
    s_busy.store(false);
    s_worker = nullptr;
    if (!s_ui.alive) {
        delete msg;
        return;
    }
    if (!msg->ok) {
        SetStatus(msg->err[0] != '\0' ? msg->err : Lang::Strings::SETTINGS_CONV_REQUEST_FAIL);
        RefreshOptions();
        delete msg;
        return;
    }
    s_ui.known = true;
    s_ui.enabled = msg->enabled;
    SetStatus("");
    RefreshOptions();
    delete msg;
}

// 短时联网：等待 WiFi 完成后再拉取 HTTP 配置，释放前已处理资源收口。
static void FetchTask(void* /*arg*/) {
    {
        PowerNeedHold hold(PowerNeed::OtaDownload);
        auto* msg = new SettingsTtsResultMsg();
        std::string body;
        std::string err;
        if (!Board::GetInstance().EnsureNetworkReady()) {
            msg->ok = false;
            std::snprintf(msg->err, sizeof(msg->err), "%s", Lang::Strings::SETTINGS_CONV_NET_NOT_READY);
        } else if (!HttpJson("GET", api::DeviceSettingsTtsUrl(), nullptr, body, err) ||
                   !ParseTtsEnabled(body, &msg->enabled, err)) {
            msg->ok = false;
            std::snprintf(msg->err, sizeof(msg->err), "%s", err.c_str());
        } else {
            msg->ok = true;
        }
        if (lv_async_call(AsyncApplyResult, msg) != LV_RESULT_OK) {
            ESP_LOGW(TAG, "lv_async_call fetch failed");
            delete msg;
            s_busy.store(false);
            s_worker = nullptr;
        }
    } // PowerNeedHold Release：须在 vTaskDelete 前析构
    vTaskDeleteWithCaps(nullptr);
}

static void PutTask(void* arg) {
    {
        PowerNeedHold hold(PowerNeed::OtaDownload);
        const bool enable = (arg != nullptr);
        auto* msg = new SettingsTtsResultMsg();
        if (!Board::GetInstance().EnsureNetworkReady()) {
            msg->ok = false;
            std::snprintf(msg->err, sizeof(msg->err), "%s", Lang::Strings::SETTINGS_CONV_NET_NOT_READY);
            if (lv_async_call(AsyncApplyResult, msg) != LV_RESULT_OK) {
                delete msg;
                s_busy.store(false);
                s_worker = nullptr;
            }
        } else {
            char req[32];
            std::snprintf(req, sizeof(req), "{\"enabled\":%s}", enable ? "true" : "false");
            std::string body;
            std::string err;
            if (!HttpJson("PUT", api::DeviceSettingsTtsUrl(), req, body, err) ||
                !ParseTtsEnabled(body, &msg->enabled, err)) {
                // 部分接口 PUT 只回 code；若解析失败但 HTTP 成功，按请求值展示
                if (err == Lang::Strings::SETTINGS_CONV_MISSING_ENABLED ||
                    err == Lang::Strings::SETTINGS_CONV_PARSE_FAIL ||
                    err.find("parse") != std::string::npos || err.find("Parse") != std::string::npos) {
                    cJSON* root = cJSON_Parse(body.c_str());
                    const cJSON* code = root != nullptr ? cJSON_GetObjectItem(root, "code") : nullptr;
                    if (cJSON_IsNumber(code) && code->valueint == 0) {
                        msg->ok = true;
                        msg->enabled = enable;
                        cJSON_Delete(root);
                    } else {
                        if (root != nullptr) {
                            cJSON_Delete(root);
                        }
                        msg->ok = false;
                        std::snprintf(msg->err, sizeof(msg->err), "%s", err.c_str());
                    }
                } else {
                    msg->ok = false;
                    std::snprintf(msg->err, sizeof(msg->err), "%s", err.c_str());
                }
            } else {
                msg->ok = true;
            }
            if (lv_async_call(AsyncApplyResult, msg) != LV_RESULT_OK) {
                ESP_LOGW(TAG, "lv_async_call put failed");
                delete msg;
                s_busy.store(false);
                s_worker = nullptr;
            }
        }
    }
    vTaskDeleteWithCaps(nullptr);
}

static bool StartWorker(TaskFunction_t fn, void* arg, const char* name) {
    if (s_busy.exchange(true)) {
        return false;
    }
    RefreshOptions();
    if (xTaskCreateWithCaps(fn, name, kWorkerStack, arg, 5, &s_worker,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(%s) failed", name);
        s_busy.store(false);
        s_worker = nullptr;
        RefreshOptions();
        return false;
    }
    return true;
}

static void OnOptionClicked(lv_event_t* e) {
    if (s_busy.load()) {
        return;
    }
    const bool enable = (reinterpret_cast<intptr_t>(lv_event_get_user_data(e)) != 0);
    if (s_ui.known && s_ui.enabled == enable) {
        return;
    }
    SetStatus(enable ? Lang::Strings::SETTINGS_CONV_ENABLING : Lang::Strings::SETTINGS_CONV_DISABLING);
    if (!StartWorker(PutTask, enable ? reinterpret_cast<void*>(1) : nullptr, "tts_put")) {
        SetStatus(Lang::Strings::SETTINGS_CONV_BUSY);
    }
}

void SettingsConversationTab_Reset() {
    // 仅拆 UI；busy/worker 留给 AsyncApplyResult，避免离 Tab 时强清导致重入双任务
    s_ui = {};
}

void SettingsConversationTab_OnActivated() {
    if (!s_ui.alive) {
        return;
    }
    if (s_busy.load()) {
        return;
    }
    SetStatus(Lang::Strings::SETTINGS_CONV_CONNECTING);
    if (!StartWorker(FetchTask, nullptr, "tts_get")) {
        SetStatus(Lang::Strings::SETTINGS_CONV_BUSY);
    }
}

void SettingsConversationTab_Build(lv_obj_t* page) {
    s_ui = {};
    s_ui.alive = true;

    lv_obj_t* title = lv_label_create(page);
    lv_label_set_text(title, Lang::Strings::SETTINGS_CONV_TITLE);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* hint = lv_label_create(page);
    lv_label_set_text(hint, Lang::Strings::SETTINGS_CONV_HINT);
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(hint, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);

    s_ui.status_lbl = lv_label_create(page);
    lv_label_set_text(s_ui.status_lbl, "");
    lv_obj_set_width(s_ui.status_lbl, lv_pct(100));
    lv_label_set_long_mode(s_ui.status_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_ui.status_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_ui.status_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(s_ui.status_lbl, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < kOptionCount; ++i) {
        s_ui.btns[i] = SettingsCreateSelectableOption(
            page, OptionTitle(kOptions[i].enabled), OnOptionClicked,
            static_cast<intptr_t>(kOptions[i].enabled ? 1 : 0));
    }

    s_ui.current_lbl = lv_label_create(page);
    lv_label_set_text(s_ui.current_lbl, Lang::Strings::SETTINGS_CONV_CUR_DASH);
    lv_obj_set_style_text_font(s_ui.current_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_ui.current_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(s_ui.current_lbl, LV_OBJ_FLAG_CLICKABLE);

    RefreshOptions();
}
