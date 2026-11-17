#include "yunxin_binding.h"

#include "assets/lang_screen.h"
#include "board.h"
#include "chat_cloud.h"
#include "fontpack_lvgl.h"
#include "power_policy.h"
#include "screen_common.h"
#include "system_info.h"
#include "ui_qrcode.h"
#include "ui_scale.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <new>
#include <mutex>
#include <string>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>

static const char* TAG = "YunxinBinding";
static std::atomic<bool> linked{false};
static std::atomic<int64_t> next_poll_us{0};
static std::atomic<uint32_t> revision{0};
static std::mutex state_mutex;
static std::string qr_content;
static bool request_failed = false;

bool YunxinBinding::Enabled() {
    #if CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS_GLOBAL
    return chat_cloud::IsYunxin();
    #else
    return false;
    #endif
}

bool YunxinBinding::NeedsBinding() { return Enabled() && !linked.load(std::memory_order_acquire); }
bool YunxinBinding::IsDue() { return esp_timer_get_time() >= next_poll_us; }

struct BindingPanel {
    lv_obj_t* root = nullptr; // 页面内容区内的二维码容器
    lv_obj_t* qr = nullptr; // 当前页面的二维码控件
    lv_obj_t* hint = nullptr; // 中英文绑定提示
    lv_timer_t* timer = nullptr; // 跟随容器销毁的 UI 状态检查
    void (*on_bound)() = nullptr; // 完成绑定后恢复页面业务
    std::string rendered_qr; // 已成功编码的二维码内容
    const char* rendered_hint = nullptr; // 已显示的语言资源
    uint32_t revision = UINT32_MAX; // 已应用的状态版本
};

static void UpdatePanel(lv_timer_t* timer) {
    auto* panel = static_cast<BindingPanel*>(lv_timer_get_user_data(timer));
    if (!YunxinBinding::NeedsBinding()) {
        auto completed = panel->on_bound;
        lv_obj_delete(panel->root);
        if (completed) completed();
        return;
    }
    const uint32_t current_revision = revision.load(std::memory_order_acquire);
    if (current_revision == panel->revision) return;
    std::string content;
    bool failed;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        content = qr_content;
        failed = request_failed;
    }
    if (!content.empty() && content != panel->rendered_qr) {
        if (lv_qrcode_update(panel->qr, content.data(), content.size()) == LV_RESULT_OK) {
            panel->rendered_qr = content;
        } else {
            ESP_LOGE(TAG, "QR encoding failed bytes=%u", static_cast<unsigned>(content.size()));
            failed = true;
        }
    }
    const char* text = failed ? Lang::Strings::YUNXIN_BINDING_RETRY :
        content.empty() ? Lang::Strings::YUNXIN_BINDING_CHECKING : Lang::Strings::YUNXIN_BINDING_SCAN;
    if (text != panel->rendered_hint) {
        lv_label_set_text(panel->hint, text);
        panel->rendered_hint = text;
    }
    panel->revision = current_revision;
}

lv_obj_t* YunxinBinding::CreatePanel(lv_obj_t* parent, void (*on_bound)()) {
    if (!parent || !NeedsBinding()) return nullptr;
    auto* panel = new (std::nothrow) BindingPanel;
    if (!panel) {
        ESP_LOGE(TAG, "binding panel allocation failed");
        return nullptr;
    }
    panel->on_bound = on_bound;
    panel->root = lv_obj_create(parent);
    lv_obj_remove_style_all(panel->root);
    lv_obj_set_size(panel->root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(panel->root, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(panel->root, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(panel->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel->root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(panel->root, UiSy(20), 0);
    lv_obj_clear_flag(panel->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel->root, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_FLOATING));
    const int side = std::min<int>(UiSx(280), LV_HOR_RES - UiSx(96));
    auto* frame = UiCreateQrFrame(panel->root, side);
    panel->qr = lv_qrcode_create(frame);
    lv_qrcode_set_size(panel->qr, side);
    lv_qrcode_set_dark_color(panel->qr, lv_color_black());
    lv_qrcode_set_light_color(panel->qr, lv_color_white());
    lv_qrcode_set_quiet_zone(panel->qr, true);
    lv_obj_clear_flag(panel->qr, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    panel->hint = lv_label_create(panel->root);
    lv_obj_set_width(panel->hint, lv_pct(90));
    lv_obj_set_style_text_font(panel->hint, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(panel->hint, lv_color_black(), 0);
    lv_obj_set_style_text_align(panel->hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(panel->hint, LV_LABEL_LONG_WRAP);
    lv_obj_add_event_cb(panel->root, [](lv_event_t* event) {
        auto* panel = static_cast<BindingPanel*>(lv_event_get_user_data(event));
        if (panel->timer) lv_timer_delete(panel->timer);
        delete panel;
    }, LV_EVENT_DELETE, panel);
    panel->timer = lv_timer_create(UpdatePanel, 250, panel);
    if (!panel->timer) {
        ESP_LOGE(TAG, "binding panel timer allocation failed");
        lv_obj_delete(panel->root);
        return nullptr;
    }
    lv_timer_ready(panel->timer);
    return panel->root;
}

static void NotifyUnlinked() {
    static bool shown = false;
    if (!shown && YunxinBinding::NeedsBinding()) {
        ScreenShowToast(Lang::Strings::YUNXIN_BINDING_OPEN_AI);
        shown = true;
    }
}
static ScreenPaintCoalesce notification{.paint = NotifyUnlinked};

static esp_err_t Query(bool& active, std::string& content, int& status, int& code, int& transport_error) {
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    if (!http) return ESP_ERR_NO_MEM;
    struct CloseGuard {
        Http* http; // 请求结束时关闭连接
        ~CloseGuard() { http->Close(); }
    } close{http.get()};
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
    // 空正文必须显式设置，否则底层 POST 默认进入流式 chunked 上传模式。
    http->SetContent(std::string{});
    constexpr int kRequestBudgetMs = 15000;
    http->SetTimeout(kRequestBudgetMs);
    http->SetRequestDeadline(esp_timer_get_time() + int64_t(kRequestBudgetMs) * 1000);
    if (!http->Open("POST", yunxin_binding_url)) {
        transport_error = http->GetLastError();
        return ESP_FAIL;
    }
    status = http->GetStatusCode();
    if (status != 200) return ESP_ERR_INVALID_RESPONSE;
    std::string body;
    char chunk[256];
    for (;;) {
        const int count = http->Read(chunk, sizeof(chunk));
        if (count < 0) {
            transport_error = http->GetLastError();
            return ESP_FAIL;
        }
        if (count == 0) break;
        if (body.size() + count > 4096) return ESP_ERR_INVALID_SIZE;
        body.append(chunk, count);
    }
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(cJSON_Parse(body.c_str()), cJSON_Delete);
    if (!json) return ESP_ERR_INVALID_RESPONSE;
    const auto* business_code = cJSON_GetObjectItemCaseSensitive(json.get(), "code");
    if (!cJSON_IsNumber(business_code)) return ESP_ERR_INVALID_RESPONSE;
    code = business_code->valueint;
    if (business_code->valuedouble != 0) return ESP_ERR_INVALID_RESPONSE;
    const auto* data = cJSON_GetObjectItemCaseSensitive(json.get(), "data");
    const auto* flag = cJSON_GetObjectItemCaseSensitive(data, "active");
    if (!cJSON_IsBool(flag)) return ESP_ERR_INVALID_RESPONSE;
    active = cJSON_IsTrue(flag);
    if (!active) {
        const auto* value = cJSON_GetObjectItemCaseSensitive(data, "qrContent");
        if (!cJSON_IsString(value) || !value->valuestring[0]) return ESP_ERR_INVALID_RESPONSE;
        content = value->valuestring;
        if (content.size() > 1024) return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t YunxinBinding::Check(bool& active) {
    active = false;
    if (!Enabled() || !IsDue()) return ESP_ERR_INVALID_STATE;
    next_poll_us = esp_timer_get_time() + 3000000;
    PowerNeedHold hold(PowerNeed::OtaDownload);
    std::string content;
    int status = 0, code = -1, transport_error = 0;
    const esp_err_t result = Query(active, content, status, code, transport_error);
    static int64_t last_failure_us = 0;
    const int64_t now = esp_timer_get_time();
    if (result != ESP_OK) {
        active = false;
        if (last_failure_us == 0 || now - last_failure_us >= 30000000) {
            ESP_LOGW(TAG, "binding check retry err=%d http=%d code=%d transport=%d",
                     result, status, code, transport_error);
            last_failure_us = now;
        }
        if (status == 429 || code == 429) next_poll_us = now + 10000000;
    } else if (last_failure_us != 0) {
        ESP_LOGI(TAG, "binding check recovered");
        last_failure_us = 0;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        request_failed = result != ESP_OK;
        if (result == ESP_OK && !active) qr_content = std::move(content);
        if (active) std::string{}.swap(qr_content);
    }
    if (active) {
        linked.store(true, std::memory_order_release);
        ESP_LOGI(TAG, "device binding confirmed");
    } else if (result == ESP_OK) {
        ScreenPaintCoalesceRequest(&notification);
    }
    revision.fetch_add(1, std::memory_order_release);
    return result;
}
