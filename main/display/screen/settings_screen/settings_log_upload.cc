#include "settings_log_upload.h"
#include "log_upload_session.h"
#include "api_endpoints.h"
#include "api_http.h"
#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "power_policy.h"
#include "screen_common.h"
#include "sd_log_writer.h"
#include "ui_scale.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

namespace {
constexpr const char* TAG = "SettingsLogUp";
constexpr const char* kBoundary = "----ESP32_DEVICE_LOG_BOUNDARY";
constexpr int64_t kUploadBudgetUs = 120LL * 1000000;
LogUploadSessions s_sessions;
std::atomic<bool> s_active{false};

struct UploadUi {
    lv_obj_t* page = nullptr;
    lv_obj_t* title = nullptr;
    lv_obj_t* percent = nullptr;
    lv_obj_t* filename = nullptr;
    lv_obj_t* count = nullptr;
    unsigned shown_jobs = ~0U;
    lv_obj_t* bar = nullptr;
    lv_timer_t* timer = nullptr;
    LogUploadSessionPtr session;
    int shown_percent = -1;
    const char* shown_title = nullptr;
    int64_t done_us = 0;
} s_ui;

bool Abandoned(const LogUploadSession& s, int64_t deadline) {
    return s.cancel.load() || esp_timer_get_time() >= deadline;
}

// ESP-IDF HTTP client is private to this worker: no concurrent Close/Write, gate
// reset, borrowed Http*, or abort task. Network calls use a 60-second I/O timeout;
// cancellation is cooperative and does not claim to interrupt DNS immediately.
struct HttpOwner {
    esp_http_client_handle_t handle = nullptr;
    ~HttpOwner() { if (handle) esp_http_client_cleanup(handle); }
};

bool WriteAll(esp_http_client_handle_t http, const char* data, size_t size,
              LogUploadSession& session, int64_t deadline) {
    size_t offset = 0;
    while (offset < size) {
        if (Abandoned(session, deadline)) return false;
        int n = esp_http_client_write(http, data + offset, static_cast<int>(size - offset));
        if (n <= 0) return false;
        offset += n;
    }
    return true;
}

bool ReadResponse(esp_http_client_handle_t http, char* response, size_t capacity,
                  LogUploadSession& session, int64_t deadline) {
    size_t used = 0;
    while (used + 1 < capacity) {
        if (Abandoned(session, deadline)) return false;
        // fetch_headers can parse the complete response but still retain unread
        // body bytes. Always consume Read before examining parser completeness.
        int n = esp_http_client_read(http, response + used, capacity - 1 - used);
        if (n < 0) return false;
        if (n == 0) {
            response[used] = '\0';
            return used > 0 && esp_http_client_is_complete_data_received(http);
        }
        used += n;
    }
    if (Abandoned(session, deadline)) return false;
    char extra;
    if (esp_http_client_read(http, &extra, 1) != 0) return false; // reject oversized body
    response[used] = '\0';
    return esp_http_client_is_complete_data_received(http);
}

LogUploadResult SendSnapshot(LogUploadSession& session, const SdLogSnapshot& snapshot, bool crash, size_t sent, size_t total) {
    const unsigned previous = session.jobs.load();
    session.jobs.store((previous & 0xffffU) | ((crash ? 2U : 1U) << 16));
    std::string mac = api::DeviceId();
    for (char& ch : mac) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    std::string url = api::DeviceLogsUploadUrl(mac);
    if (url.empty()) return LogUploadResult::Failed;
    const int64_t deadline = esp_timer_get_time() + kUploadBudgetUs;
    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 60000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 1024;
    HttpOwner http;
    http.handle = esp_http_client_init(&config);
    if (!http.handle) return LogUploadResult::NoMemory;
    std::string ctype = std::string("multipart/form-data; boundary=") + kBoundary;
    if (esp_http_client_set_header(http.handle, "Device-Id", api::DeviceId().c_str()) != ESP_OK ||
        esp_http_client_set_header(http.handle, "Accept", "*/*") != ESP_OK ||
        esp_http_client_set_header(http.handle, "Connection", "close") != ESP_OK ||
        esp_http_client_set_header(http.handle, "Content-Type", ctype.c_str()) != ESP_OK)
        return LogUploadResult::Failed;
    char header[220], footer[64];
    int hn = snprintf(header, sizeof(header),
                      "--%s\r\nContent-Disposition: form-data; name=\"log\"; filename=\"%s\"\r\n"
                      "Content-Type: %s\r\n\r\n", kBoundary,
                      crash ? "metalio_coredump.elf" : "metalio_logs.txt",
                      crash ? "application/octet-stream" : "text/plain");
    int fn = snprintf(footer, sizeof(footer), "\r\n--%s--\r\n", kBoundary);
    if (hn <= 0 || size_t(hn) >= sizeof(header) || fn <= 0 || size_t(fn) >= sizeof(footer))
        return LogUploadResult::Failed;
    if (Abandoned(session, deadline)) return LogUploadResult::Cancelled;
    if (esp_http_client_open(http.handle, int(snapshot.size + hn + fn)) != ESP_OK)
        return LogUploadResult::Failed;
    if (!WriteAll(http.handle, header, hn, session, deadline)) return LogUploadResult::Failed;
    session.phase.store(LogUploadPhase::Sending);
    for (size_t offset = 0; offset < snapshot.size;) {
        size_t size = std::min(snapshot.size - offset, size_t(1024));
        if (!WriteAll(http.handle, reinterpret_cast<const char*>(snapshot.data + offset), size, session, deadline))
            return LogUploadResult::Failed;
        offset += size;
        session.percent.store(std::min(99, int((sent + offset) * 100ULL / total)));
    }
    if (!WriteAll(http.handle, footer, fn, session, deadline)) return LogUploadResult::Failed;
    if (Abandoned(session, deadline)) return LogUploadResult::Failed;
    if (esp_http_client_fetch_headers(http.handle) < 0) return LogUploadResult::Failed;
    int status = esp_http_client_get_status_code(http.handle);
    if (status < 200 || status >= 300) return LogUploadResult::Failed;
    char response[512];
    if (!ReadResponse(http.handle, response, sizeof(response), session, deadline)) return LogUploadResult::Failed;
    cJSON* root = cJSON_ParseWithOpts(response, nullptr, true);
    if (!root) return LogUploadResult::Failed;
    const cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    bool ok = (cJSON_IsNumber(code) && code->valuedouble == 0) ||
              (cJSON_IsString(code) && code->valuestring && strcmp(code->valuestring, "0") == 0);
    cJSON_Delete(root);
    if (ok) session.jobs.fetch_add(1U << 8);
    return ok ? LogUploadResult::Ok : LogUploadResult::Failed;
}

struct ActivationHold {
    ActivationHold() { Application::GetInstance().PauseActivationPolling(); }
    ~ActivationHold() { Application::GetInstance().ResumeActivationPolling(); }
};

LogUploadResult RunUpload(LogUploadSession& session) {
    if (session.cancel.load()) return LogUploadResult::Cancelled;
    PowerNeedHold power(PowerNeed::OtaDownload);
    ActivationHold activation;
    int64_t wait_deadline = esp_timer_get_time() + 30LL * 1000000;
    while (Application::GetInstance().IsActivationPollingBusy()) {
        if (Abandoned(session, wait_deadline)) return LogUploadResult::NetFailed;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (session.cancel.load()) return LogUploadResult::Cancelled;
    SdLogSnapshot snapshot;
    switch (SdLogWriter_Snapshot(snapshot)) {
        case SdLogSnapshotResult::NoFile: break;
        case SdLogSnapshotResult::Unavailable: return LogUploadResult::StorageUnavailable;
        case SdLogSnapshotResult::NoMemory: return LogUploadResult::NoMemory;
        case SdLogSnapshotResult::IoError: return LogUploadResult::Failed;
        case SdLogSnapshotResult::Ok: break;
    }
    SdLogSnapshot crash;
    auto crash_result = SdLogWriter_CrashSnapshot(crash);
    if (crash_result == SdLogSnapshotResult::Unavailable) return LogUploadResult::StorageUnavailable;
    if (crash_result == SdLogSnapshotResult::NoMemory) return LogUploadResult::NoMemory;
    if (crash_result == SdLogSnapshotResult::IoError) return LogUploadResult::Failed;
    if (!snapshot.size && !crash.size) return LogUploadResult::NoFile;
    session.jobs.store(unsigned(snapshot.size > 0) + unsigned(crash.size > 0));
    session.phase.store(LogUploadPhase::WaitingNetwork);
    bool ready = false;
    wait_deadline = esp_timer_get_time() + 30LL * 1000000;
    while (!Abandoned(session, wait_deadline)) {
        if (Board::GetInstance().IsWifiConfigMode()) break;
        if (Board::GetInstance().EnsureNetworkReady(200)) { ready = true; break; }
        if (Board::GetInstance().GetNetworkErrorText(nullptr)) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (session.cancel.load()) return LogUploadResult::Cancelled;
    if (!ready) return LogUploadResult::NetFailed;
    if (snapshot.size) {
        auto result = SendSnapshot(session, snapshot, false, 0, snapshot.size + crash.size);
        if (result != LogUploadResult::Ok) return result;
    }
    if (crash.size) {
        return SendSnapshot(session, crash, true, snapshot.size, snapshot.size + crash.size);
    }
    return LogUploadResult::Ok;
}

void Worker(void* arg) {
    {
        std::unique_ptr<LogUploadSessionPtr> owner(
            static_cast<LogUploadSessionPtr*>(arg));
        LogUploadResult result = RunUpload(**owner);
        // RunUpload has already destroyed HTTP, snapshot and all guards.
        ESP_LOGI(TAG, "worker complete result=%u cancelled=%d stack_hwm=%u",
                 unsigned(result), (*owner)->cancel.load(), unsigned(uxTaskGetStackHighWaterMark(nullptr)));
        (*owner)->Finish(result);
    }
    vTaskDeleteWithCaps(nullptr);
}

void StopUiTimer() {
    if (s_ui.timer) { lv_timer_delete(s_ui.timer); s_ui.timer = nullptr; }
}
void PageDeleted(lv_event_t*) {
    if (s_ui.session && s_ui.session->running.load()) s_ui.session->cancel.store(true);
    StopUiTimer();
    s_ui = {};
    s_active.store(false);
    HapticSetZoneEarlyPulseEnabled(true);
}
void ClosePage() {
    if (s_ui.page) lv_obj_delete(s_ui.page); // DELETE callback invalidates timer/pointers
}
void CancelClicked(lv_event_t*) {
    SettingsLogUpload_RequestAbort("cancel");
    ClosePage();
}

void Poll(lv_timer_t*) {
    const auto& session = s_ui.session;
    if (!session) return;
    const char* title;
    bool running = session->running.load(std::memory_order_acquire);
    if (running) {
        if (session->cancel.load()) title = Lang::Strings::SETTINGS_LOG_CANCELLING;
        else if (session->phase.load() == LogUploadPhase::Snapshot) title = Lang::Strings::SETTINGS_LOG_PREPARING;
        else if (session->phase.load() == LogUploadPhase::WaitingNetwork) title = Lang::Strings::SETTINGS_LOG_CONNECTING_NET;
        else title = Lang::Strings::SETTINGS_LOG_UPLOADING;
    } else {
        switch (session->result.load()) {
            case LogUploadResult::Ok: title = Lang::Strings::SETTINGS_LOG_UPLOAD_OK; break;
            case LogUploadResult::Cancelled: title = Lang::Strings::CLOUD_CANCELLED; break;
            case LogUploadResult::NoFile: title = Lang::Strings::SETTINGS_LOG_UPLOAD_NO_FILE; break;
            case LogUploadResult::NetFailed: title = Lang::Strings::SETTINGS_CONV_NET_NOT_READY; break;
            case LogUploadResult::StorageUnavailable: title = Lang::Strings::SETTINGS_LOG_SD_UNAVAILABLE; break;
            case LogUploadResult::NoMemory: title = Lang::Strings::SETTINGS_LOG_NO_MEMORY; break;
            default: title = Lang::Strings::SETTINGS_LOG_UPLOAD_FAIL; break;
        }
        if (!s_ui.done_us) s_ui.done_us = esp_timer_get_time();
        if (esp_timer_get_time() - s_ui.done_us >= 2000000) { ClosePage(); return; }
    }
    if (title != s_ui.shown_title) {
        lv_label_set_text(s_ui.title, title);
        s_ui.shown_title = title;
    }
    const unsigned jobs = session->jobs.load();
    if (jobs != s_ui.shown_jobs) {
        const unsigned total = jobs & 0xffU;
        const unsigned done = (jobs >> 8) & 0xffU;
        const unsigned file = (jobs >> 16) & 0xffU;
        lv_label_set_text(s_ui.filename, file == 1 ? "metalio_logs.txt" :
                          file == 2 ? "metalio_coredump.elf" : "");
        char text[96];
        snprintf(text, sizeof(text), Lang::Strings::SETTINGS_LOG_UPLOAD_COUNT_FMT,
                 done, total >= done ? total - done : 0);
        lv_label_set_text(s_ui.count, text);
        s_ui.shown_jobs = jobs;
    }
    int percent = !running && session->result.load() == LogUploadResult::Ok ? 100 : session->percent.load();
    if (percent != s_ui.shown_percent) {
        char text[16]; snprintf(text, sizeof(text), "%d%%", percent);
        lv_label_set_text(s_ui.percent, text);
        lv_bar_set_value(s_ui.bar, percent, LV_ANIM_OFF);
        s_ui.shown_percent = percent;
    }
}

bool ShowPage(const LogUploadSessionPtr& session) {
    if (s_ui.page) return true;
    lv_obj_t* parent = lv_screen_active();
    if (!parent) return false;
    s_ui.session = session;
    s_ui.page = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ui.page);
    lv_obj_set_size(s_ui.page, LV_HOR_RES, LV_VER_RES);
    ScreenApplyDotBackdrop(s_ui.page);
    lv_obj_add_flag(s_ui.page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_ui.page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_ui.page, PageDeleted, LV_EVENT_DELETE, nullptr);
    lv_obj_t* card = lv_obj_create(s_ui.page);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(card, LV_HOR_RES - UiSx(48), LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, UiSx(2), 0);
    lv_obj_set_style_pad_all(card, UiSx(18), 0);
    lv_obj_set_style_pad_row(card, UiSy(14), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_ui.title = lv_label_create(card);
    lv_obj_set_width(s_ui.title, lv_pct(100));
    lv_obj_set_style_text_font(s_ui.title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_align(s_ui.title, LV_TEXT_ALIGN_CENTER, 0);
    s_ui.filename = lv_label_create(card);
    lv_obj_set_width(s_ui.filename, lv_pct(100));
    lv_label_set_long_mode(s_ui.filename, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_ui.filename, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_align(s_ui.filename, LV_TEXT_ALIGN_CENTER, 0);
    s_ui.count = lv_label_create(card);
    lv_obj_set_width(s_ui.count, lv_pct(100));
    lv_obj_set_style_text_font(s_ui.count, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_align(s_ui.count, LV_TEXT_ALIGN_CENTER, 0);
    s_ui.bar = lv_bar_create(card);
    lv_obj_set_size(s_ui.bar, UiSx(300), UiSy(28));
    lv_bar_set_range(s_ui.bar, 0, 100);
    lv_obj_set_style_bg_color(s_ui.bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.bar, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_border_color(s_ui.bar, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.bar, UiSx(2), LV_PART_MAIN);
    s_ui.percent = lv_label_create(card);
    lv_obj_set_style_text_font(s_ui.percent, fontpack_lv_font_ui(), 0);
    lv_obj_t* cancel = lv_obj_create(card);
    lv_obj_clear_flag(cancel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(cancel, UiSx(160), UiSy(44));
    lv_obj_add_flag(cancel, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(cancel);
    lv_obj_add_event_cb(cancel, CancelClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* text = lv_label_create(cancel);
    lv_label_set_text(text, Lang::Strings::COMMON_CANCEL);
    lv_obj_set_style_text_font(text, fontpack_lv_font_ui(), 0);
    lv_obj_center(text);
    lv_obj_clear_flag(text, LV_OBJ_FLAG_CLICKABLE);
    s_ui.timer = lv_timer_create(Poll, 250, nullptr);
    if (!s_ui.timer) { ClosePage(); return false; }
    s_active.store(true);
    HapticSetZoneEarlyPulseEnabled(false);
    Poll(nullptr);
    return true;
}
}

bool SettingsLogUpload_IsActive() { return s_active.load(); }
bool SettingsLogUpload_IsRunning() {
    auto current = s_sessions.Current();
    return current && current->running.load(std::memory_order_acquire);
}
void SettingsLogUpload_RequestAbort(const char* why) {
    s_sessions.Cancel(); // no LVGL, I/O, join, heap allocation or HTTP destruction
    (void)why;
}
void SettingsLogUpload_Teardown() {
    SettingsLogUpload_RequestAbort("teardown");
    ClosePage();
}
bool SettingsLogUpload_OnVkKey(const char* key) {
    if (!SettingsLogUpload_IsActive() || !key) return false;
    if (!strcmp(key, "vk_home") || !strcmp(key, "vk_prev")) {
        SettingsLogUpload_RequestAbort(key);
        ClosePage();
    }
    return true;
}
void SettingsLogUpload_Start() {
    if (s_ui.page) return;
    auto current = s_sessions.Current();
    if (current && current->running.load(std::memory_order_acquire)) {
        ShowPage(current); // existing cancelling worker; never overlap uploads
        return;
    }
    auto session = LogUploadSessionPtr(new (std::nothrow) LogUploadSession);
    if (!session) { ESP_LOGE(TAG, "session allocation failed"); return; }
    if (!s_sessions.Begin(session)) return;
    if (!ShowPage(session)) { session->Finish(LogUploadResult::Failed); return; }
    auto* arg = new (std::nothrow) LogUploadSessionPtr(session);
    if (!arg || xTaskCreatePinnedToCoreWithCaps(Worker, "log_upload", 8192, arg, 5, nullptr, 0,
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        delete arg;
        session->Finish(LogUploadResult::NoMemory);
    }
}
