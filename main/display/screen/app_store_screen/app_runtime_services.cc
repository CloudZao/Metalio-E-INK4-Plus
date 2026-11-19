#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "app_runtime_services.h"
#include "app_store/app_audio_player.h"
#include "app_store/app_audio_recorder.h"
#include "board.h"
#include "epd_refresh.h"
#include "haptic_feedback.h"
#include "metalio_accel.h"
#include "screen_common.h"
#include "screen_keyboard.h"
#include "standby_screen/standby_screen.h"
#include "reader/http_download_file.h"
#include "sd_access.h"
#include "sd_paths.h"

namespace AppRuntimeServices {
namespace {
constexpr const char* TAG = "AppServices";
constexpr uint32_t kDataLimit = 16 * 1024;
constexpr unsigned kKeyLimit = 32;
const std::string kDataRoot = SD_PATH_APP_STATE;

bool ValidKey(const char* key) {
    if (!key || !key[0]) return false;
    unsigned length = 0;
    for (; key[length]; ++length) {
        if (length >= 48) return false;
        const char c = key[length];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}
State* Active(void* host) {
    auto* state = static_cast<State*>(host);
    return state && state->open ? state : nullptr;
}
int IoError(const char* operation, const char* stage) {
    ESP_LOGE(TAG, "operation=%s stage=%s result=failed errno=%d",operation,stage,errno);
    return XZ_APP_ERR_IO;
}
int LocalTime(xz_app_time_t* out) {
    if (!out) return XZ_APP_ERR_INVALID;
    *out = {};
    const time_t now = std::time(nullptr);
    struct tm date {};
    if (!localtime_r(&now,&date) || date.tm_year < 120) return XZ_APP_ERR_NOT_READY;
    *out = {static_cast<int64_t>(now),static_cast<int16_t>(date.tm_year+1900),
        static_cast<uint8_t>(date.tm_mon+1),static_cast<uint8_t>(date.tm_mday),
        static_cast<uint8_t>(date.tm_hour),static_cast<uint8_t>(date.tm_min),
        static_cast<uint8_t>(date.tm_sec),static_cast<uint8_t>(date.tm_wday)};
    return XZ_APP_OK;
}
uint64_t MonotonicMs() { return static_cast<uint64_t>(esp_timer_get_time()/1000); }
void TimerTick(lv_timer_t* handle) {
    auto* timer = static_cast<Timer*>(lv_timer_get_user_data(handle));
    if ((StandbyScreen::IsActive() || StandbyScreen::IsResuming()) || lv_obj_get_screen(timer->root) != lv_screen_active()) return;
    const auto callback = timer->callback;
    void* context = timer->context;
    if (!timer->repeat) {
        lv_timer_delete(timer->handle);
        *timer = {};
    }
    callback(context);
}
uint32_t StartTimer(void* host, uint32_t interval, int repeat, xz_app_click_t callback, void* context) {
    auto* state = Active(host);
    if (!state || state->closing || !callback || interval < 200 || interval > 86400000) return 0;
    auto slot = std::find_if(state->timers.begin(),state->timers.end(),[](const Timer& value){return !value.handle;});
    if (slot == state->timers.end() || state->next_id == UINT32_MAX) return 0;
    slot->handle = lv_timer_create(TimerTick,interval,&*slot);
    if (!slot->handle) { ESP_LOGE(TAG,"operation=timer_start stage=allocate result=failed"); return 0; }
    slot->id = ++state->next_id;
    slot->repeat = repeat != 0;
    slot->callback = callback;
    slot->context = context;
    slot->root = state->root;
    ESP_LOGD(TAG,"operation=timer_start result=success id=%lu interval_ms=%lu",(unsigned long)slot->id,(unsigned long)interval);
    return slot->id;
}
int CancelTimer(void* host, uint32_t id) {
    auto* state = Active(host);
    if (!state) return XZ_APP_ERR_NOT_READY;
    if (!id) return XZ_APP_ERR_INVALID;
    for (auto& timer : state->timers) if (timer.handle && timer.id == id) {
        lv_timer_delete(timer.handle);
        timer = {};
        return XZ_APP_OK;
    }
    return XZ_APP_ERR_NOT_FOUND;
}
bool Directory(const std::string& path) {
    if (mkdir(path.c_str(),0755) == 0) return true;
    struct stat st {};
    return errno == EEXIST && stat(path.c_str(),&st) == 0 && S_ISDIR(st.st_mode);
}
std::string DataDirectory(const State& state) { return kDataRoot+"/"+state.storage_key; }
std::string DataPath(const State& state, const char* key) { return DataDirectory(state)+"/"+key+".bin"; }
std::string WorkDirectory(const State& state) { return std::string(SD_PATH_APP_STORE_TEMP)+"/"+state.storage_key; }
std::string PendingPath(const State& state, const char* key) { return WorkDirectory(state)+"/data-"+key+".bin.part"; }
void CleanWorkDirectory(const State& state) {
    if (rmdir(WorkDirectory(state).c_str()) != 0 && errno != ENOENT && errno != ENOTEMPTY && errno != EEXIST)
        ESP_LOGW(TAG,"operation=data_cleanup stage=work_directory result=failed errno=%d",errno);
}
void CleanPending(const State& state, const std::string& pending) {
    if (unlink(pending.c_str()) != 0 && errno != ENOENT)
        ESP_LOGW(TAG,"operation=data_write stage=pending_cleanup result=failed errno=%d",errno);
    CleanWorkDirectory(state);
}
int ReadData(void* host, const char* key, void* buffer, uint32_t capacity) {
    auto* state = Active(host);
    if (!state) return XZ_APP_ERR_NOT_READY;
    if (state->storage_key.empty() || !ValidKey(key)) return XZ_APP_ERR_INVALID;
    sd_access::Transaction transaction;
    if (!transaction) { ESP_LOGW(TAG,"operation=data_read stage=sd_access result=unavailable"); return XZ_APP_ERR_NOT_READY; }
    const auto path = DataPath(*state,key);
    FILE* file = fopen(path.c_str(),"rb");
    if (!file && errno == ENOENT) file = fopen((path+".cloud-backup").c_str(),"rb");
    if (!file) return errno == ENOENT ? XZ_APP_ERR_NOT_FOUND : IoError("data_read","open");
    int result = XZ_APP_ERR_IO;
    struct stat st {};
    if (fstat(fileno(file),&st) != 0) result = IoError("data_read","stat");
    else if (!S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > kDataLimit) {
        ESP_LOGE(TAG,"operation=data_read stage=size result=invalid");
        result = XZ_APP_ERR_LIMIT;
    } else if (!buffer) result = static_cast<int>(st.st_size);
    else if (capacity < static_cast<uint32_t>(st.st_size)) result = XZ_APP_ERR_LIMIT;
    else if (fread(buffer,1,st.st_size,file) != static_cast<size_t>(st.st_size)) result = IoError("data_read","read");
    else result = static_cast<int>(st.st_size);
    if (fclose(file) != 0 && result >= 0) result = IoError("data_read","close");
    return result;
}
int CountKeys(const std::string& directory) {
    DIR* dir = opendir(directory.c_str());
    if (!dir) return IoError("data_write","open_directory");
    int count = 0;
    while (auto* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name.size() > 4 && name.compare(name.size()-4,4,".bin") == 0) ++count;
        else if (name.size() > 17 && name.compare(name.size()-17,17,".bin.cloud-backup") == 0) {
            struct stat st {};
            if (stat((directory+"/"+name.substr(0,name.size()-13)).c_str(),&st) != 0) {
                if (errno != ENOENT) { const int result = IoError("data_write","count_stat"); closedir(dir); return result; }
                ++count;
            }
        }
    }
    closedir(dir);
    return count;
}
int WriteData(void* host, const char* key, const void* data, uint32_t length) {
    auto* state = Active(host);
    if (!state) return XZ_APP_ERR_NOT_READY;
    if (state->storage_key.empty() || !ValidKey(key) || (!data && length)) return XZ_APP_ERR_INVALID;
    if (length > kDataLimit) return XZ_APP_ERR_LIMIT;
    sd_access::Transaction transaction;
    if (!transaction) { ESP_LOGW(TAG,"operation=data_write stage=sd_access result=unavailable"); return XZ_APP_ERR_NOT_READY; }
    const std::string directory = DataDirectory(*state);
    if (!Directory(SD_PATH_SETTINGS) || !Directory(kDataRoot) || !Directory(directory)) return IoError("data_write","directory");
    const auto path = DataPath(*state,key);
    const auto backup = path+".cloud-backup";
    struct stat st {};
    if (stat(backup.c_str(),&st) == 0) {
        if (stat(path.c_str(),&st) == 0) {
            if (unlink(backup.c_str()) != 0) return IoError("data_write","clean_backup");
        } else if (errno != ENOENT || rename(backup.c_str(),path.c_str()) != 0) return IoError("data_write","restore_backup");
    } else if (errno != ENOENT) return IoError("data_write","stat_backup");
    if (stat(path.c_str(),&st) != 0) {
        if (errno != ENOENT) return IoError("data_write","stat");
        const int count = CountKeys(directory);
        if (count < 0) return count;
        if (count >= static_cast<int>(kKeyLimit)) return XZ_APP_ERR_LIMIT;
    }
    if (!Directory(SD_PATH_TEMP) || !Directory(SD_PATH_APP_STORE_TEMP) || !Directory(WorkDirectory(*state)))
        return IoError("data_write","work_directory");
    const auto pending = PendingPath(*state,key);
    FILE* file = fopen(pending.c_str(),"wb");
    if (!file) { const int result = IoError("data_write","open"); CleanPending(*state,pending); return result; }
    bool ok = !length || fwrite(data,1,length,file) == length;
    int failure = ok ? 0 : errno;
    if (fflush(file) != 0 && ok) { ok = false; failure = errno; }
    if (ok && fsync(fileno(file)) != 0) { ok = false; failure = errno; }
    if (fclose(file) != 0 && ok) { ok = false; failure = errno; }
    if (!ok) {
        ESP_LOGE(TAG,"operation=data_write stage=write result=failed errno=%d",failure);
        CleanPending(*state,pending);
        return XZ_APP_ERR_IO;
    }
    std::string error;
    if (!reader::CommitDownloadedFile(pending,path,true,error)) {
        const int result = IoError("data_write","commit");
        CleanPending(*state,pending);
        return result;
    }
    CleanWorkDirectory(*state);
    ESP_LOGD(TAG,"operation=data_write result=success bytes=%lu",(unsigned long)length);
    return XZ_APP_OK;
}
int RemoveData(void* host, const char* key) {
    auto* state = Active(host);
    if (!state) return XZ_APP_ERR_NOT_READY;
    if (state->storage_key.empty() || !ValidKey(key)) return XZ_APP_ERR_INVALID;
    sd_access::Transaction transaction;
    if (!transaction) { ESP_LOGW(TAG,"operation=data_remove stage=sd_access result=unavailable"); return XZ_APP_ERR_NOT_READY; }
    const auto path = DataPath(*state,key);
    bool found = false;
    for (const auto& suffix : {".cloud-backup",""}) {
        if (unlink((path+suffix).c_str()) == 0) found = true;
        else if (errno != ENOENT) return IoError("data_remove","unlink");
    }
    const auto pending = PendingPath(*state,key);
    if (unlink(pending.c_str()) == 0) found = true;
    else if (errno != ENOENT) return IoError("data_remove","pending_unlink");
    CleanWorkDirectory(*state);
    return found ? XZ_APP_OK : XZ_APP_ERR_NOT_FOUND;
}
void* Container(void* parent, int x, int y, int width, int height, int scroll_y) {
    if (!parent) return nullptr;
    auto* object = lv_obj_create(static_cast<lv_obj_t*>(parent));
    lv_obj_set_pos(object,x,y);
    lv_obj_set_size(object,width,height);
    lv_obj_set_style_pad_all(object,0,0);
    lv_obj_set_style_border_width(object,0,0);
    lv_obj_set_style_radius(object,0,0);
    lv_obj_set_style_bg_opa(object,LV_OPA_TRANSP,0);
    if (scroll_y) lv_obj_set_scroll_dir(object,LV_DIR_VER);
    else lv_obj_remove_flag(object,LV_OBJ_FLAG_SCROLLABLE);
    return object;
}
struct InputCallback {
    xz_app_click_t callback; // 应用输入确认回调
    void* context; // 应用确认数据
};
void* Input(void* parent, int x, int y, int width, int height, const char* text, uint32_t max_chars,
            xz_app_click_t submit, void* context) {
    if (!parent || !max_chars || max_chars > 256) return nullptr;
    auto* binding = submit ? new(std::nothrow) InputCallback{submit,context} : nullptr;
    if (submit && !binding) { ESP_LOGE(TAG,"operation=input stage=allocate result=failed"); return nullptr; }
    auto* object = lv_textarea_create(static_cast<lv_obj_t*>(parent));
    lv_obj_set_pos(object,x,y);
    lv_obj_set_size(object,width,height);
    lv_textarea_set_one_line(object,true);
    lv_textarea_set_max_length(object,max_chars);
    lv_textarea_set_text(object,text ? text : "");
    if (binding) lv_obj_add_event_cb(object,[](lv_event_t* event) {
        auto* data = static_cast<InputCallback*>(lv_event_get_user_data(event));
        if (lv_event_get_code(event) == LV_EVENT_DELETE) delete data;
        else if (lv_event_get_code(event) == LV_EVENT_READY) data->callback(data->context);
    },LV_EVENT_ALL,binding);
    return object;
}
int GetText(void* widget, char* buffer, uint32_t capacity) {
    if (!widget) return XZ_APP_ERR_INVALID;
    auto* object = static_cast<lv_obj_t*>(widget);
    if (lv_obj_check_type(object,&lv_button_class)) object = lv_obj_get_child(object,0);
    const char* value = nullptr;
    if (object && lv_obj_check_type(object,&lv_label_class)) value = lv_label_get_text(object);
    else if (object && lv_obj_check_type(object,&lv_textarea_class)) value = lv_textarea_get_text(object);
    if (!value) return XZ_APP_ERR_INVALID;
    const size_t length = strlen(value);
    if (!buffer) return static_cast<int>(length);
    if (capacity <= length) { if (capacity) buffer[0] = 0; return XZ_APP_ERR_LIMIT; }
    memcpy(buffer,value,length+1);
    return static_cast<int>(length);
}
struct KeyboardBinding {
    State* state; // 键盘所属运行环境
    lv_obj_t* input; // 输入框被删除时置空
};
void InputDeleted(lv_event_t* event) {
    static_cast<KeyboardBinding*>(lv_event_get_user_data(event))->input = nullptr;
}
void* Keyboard(void* host, void* parent, void* input) {
    auto* state = Active(host);
    auto* target = static_cast<lv_obj_t*>(input);
    if (!state || state->closing || !parent || !target || state->keyboard || !lv_obj_check_type(target,&lv_textarea_class)) return nullptr;
    auto* binding = new(std::nothrow) KeyboardBinding{state,target};
    if (!binding) { ESP_LOGE(TAG,"operation=keyboard stage=allocate result=failed"); return nullptr; }
    auto* keyboard = ScreenKeyboardCreate(static_cast<lv_obj_t*>(parent),
        [](ScreenKeyboardEvent event, const char* text, void* context) {
            auto* data = static_cast<KeyboardBinding*>(context);
            if (!data->input || !data->state->open || data->state->closing || (StandbyScreen::IsActive() || StandbyScreen::IsResuming()) ||
                lv_obj_get_screen(data->state->root) != lv_screen_active()) return;
            if (event == ScreenKeyboardEvent::kInsert) lv_textarea_add_text(data->input,text);
            else if (event == ScreenKeyboardEvent::kBackspace) lv_textarea_delete_char(data->input);
            else if (event == ScreenKeyboardEvent::kClear) lv_textarea_set_text(data->input,"");
            else if (event == ScreenKeyboardEvent::kConfirm) lv_obj_send_event(data->input,LV_EVENT_READY,nullptr);
        },binding);
    if (!keyboard) { delete binding; ESP_LOGE(TAG,"operation=keyboard stage=create result=failed"); return nullptr; }
    state->keyboard = keyboard;
    lv_obj_align(keyboard,LV_ALIGN_BOTTOM_MID,0,0);
    lv_obj_add_event_cb(target,InputDeleted,LV_EVENT_DELETE,binding);
    lv_obj_add_event_cb(keyboard,[](lv_event_t* event) {
        auto* data = static_cast<KeyboardBinding*>(lv_event_get_user_data(event));
        if (data->input) lv_obj_remove_event_cb_with_user_data(data->input,InputDeleted,data);
        data->state->keyboard = nullptr;
        delete data;
    },LV_EVENT_DELETE,binding);
    ScreenKeyboardReset(keyboard);
    lv_obj_update_layout(keyboard);
    lv_obj_move_foreground(keyboard);
    ESP_LOGI(TAG,"operation=keyboard result=opened");
    return keyboard;
}
int CloseKeyboard(void* host) {
    auto* state = Active(host);
    if (!state) return XZ_APP_ERR_NOT_READY;
    if (!state->keyboard) return XZ_APP_ERR_NOT_FOUND;
    ScreenKeyboardPause(state->keyboard);
    lv_obj_delete(state->keyboard);
    return XZ_APP_OK;
}
void* Progress(void* parent, int x, int y, int width, int height) {
    if (!parent) return nullptr;
    auto* object = lv_bar_create(static_cast<lv_obj_t*>(parent));
    lv_obj_set_pos(object,x,y);
    lv_obj_set_size(object,width,height);
    lv_bar_set_range(object,0,100);
    lv_obj_set_style_bg_color(object,lv_color_white(),0);
    lv_obj_set_style_bg_opa(object,LV_OPA_COVER,0);
    lv_obj_set_style_border_width(object,1,0);
    lv_obj_set_style_border_color(object,lv_color_black(),0);
    lv_obj_set_style_bg_color(object,lv_color_black(),LV_PART_INDICATOR);
    return object;
}
void SetProgress(void* widget, uint32_t percent) {
    auto* object = static_cast<lv_obj_t*>(widget);
    if (object && lv_obj_check_type(object,&lv_bar_class)) lv_bar_set_value(object,std::min<uint32_t>(percent,100),LV_ANIM_OFF);
}
int OnExit(void* host, xz_app_click_t callback, void* context) {
    auto* state = Active(host);
    if (!state || state->closing) return XZ_APP_ERR_NOT_READY;
    state->on_exit = callback;
    state->exit_context = context;
    return XZ_APP_OK;
}
int OnBack(void* host, xz_app_back_t callback, void* context) {
    auto* state = Active(host);
    if (!state || state->closing) return XZ_APP_ERR_NOT_READY;
    state->on_back = callback;
    state->back_context = context;
    return XZ_APP_OK;
}
int KeepAwake(void* host, int enabled) {
    auto* state = Active(host);
    if (!state || state->closing) return XZ_APP_ERR_NOT_READY;
    if (enabled && !state->awake) state->awake.emplace(PowerNeed::StandbyInhibit);
    else if (!enabled) state->awake.reset();
    return XZ_APP_OK;
}
struct Drawing {
    std::vector<xz_app_draw_t> commands;
};
void DrawingEvent(lv_event_t* event) {
    auto* data = static_cast<Drawing*>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        delete data;
        return;
    }
    if (lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) return;
    auto* object = static_cast<lv_obj_t*>(lv_event_get_target(event));
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    auto* layer = lv_event_get_layer(event);
    for (const auto& command : data->commands) {
        if (command.kind == 0) {
            lv_draw_line_dsc_t line;
            lv_draw_line_dsc_init(&line);
            line.color = lv_color_black();
            line.width = command.width;
            line.p1.x = area.x1 + command.x1;
            line.p1.y = area.y1 + command.y1;
            line.p2.x = area.x1 + command.x2;
            line.p2.y = area.y1 + command.y2;
            lv_draw_line(layer, &line);
        } else {
            lv_draw_rect_dsc_t rect;
            lv_draw_rect_dsc_init(&rect);
            rect.radius = command.kind == 2 ? LV_RADIUS_CIRCLE : 0;
            rect.border_color = lv_color_black();
            rect.border_width = command.width;
            rect.bg_color = lv_color_black();
            rect.bg_opa = command.filled ? LV_OPA_COVER : LV_OPA_TRANSP;
            lv_area_t bounds = {area.x1 + command.x1, area.y1 + command.y1,
                                area.x1 + command.x2, area.y1 + command.y2};
            lv_draw_rect(layer, &rect, &bounds);
        }
    }
}
void* CreateDrawing(void* parent, int x, int y, int width, int height) {
    if (!parent || width < 1 || height < 1 || width > 1024 || height > 1600) return nullptr;
    auto* data = new(std::nothrow) Drawing;
    if (!data) {
        ESP_LOGE(TAG, "operation=drawing stage=allocate result=failed");
        return nullptr;
    }
    auto* object = lv_obj_create(static_cast<lv_obj_t*>(parent));
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(object, data);
    lv_obj_add_event_cb(object, DrawingEvent, LV_EVENT_ALL, data);
    return object;
}
int Draw(void* widget, const xz_app_draw_t* commands, uint32_t count) {
    if (!widget || count > 1024 || (count && !commands)) return XZ_APP_ERR_INVALID;
    auto* object = static_cast<lv_obj_t*>(widget);
    bool matched = false;
    for (uint32_t i = 0; i < lv_obj_get_event_count(object); ++i) {
        if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(object, i)) == DrawingEvent) matched = true;
    }
    if (!matched) return XZ_APP_ERR_INVALID;
    // LVGL 延迟计算坐标；首次提交绘图前必须落实创建时设置的尺寸。
    lv_obj_update_layout(object);
    auto* data = static_cast<Drawing*>(lv_obj_get_user_data(object));
    for (uint32_t i = 0; i < count; ++i) {
        const auto& c = commands[i];
        if (c.kind > 2 || c.width < 1 || c.width > 8 || c.reserved ||
            c.x1 < 0 || c.y1 < 0 || c.x2 < 0 || c.y2 < 0 ||
            c.x1 >= lv_obj_get_width(object) || c.x2 >= lv_obj_get_width(object) ||
            c.y1 >= lv_obj_get_height(object) || c.y2 >= lv_obj_get_height(object) ||
            (c.kind && (c.x2 < c.x1 || c.y2 < c.y1))) {
            ESP_LOGE(TAG, "operation=draw stage=command result=invalid index=%lu area=%ldx%ld",
                static_cast<unsigned long>(i), static_cast<long>(lv_obj_get_width(object)),
                static_cast<long>(lv_obj_get_height(object)));
            return XZ_APP_ERR_INVALID;
        }
    }
    uint32_t first = 0;
    while (first < count && first < data->commands.size() &&
           std::memcmp(&commands[first], &data->commands[first], sizeof(xz_app_draw_t)) == 0) ++first;
    if (first == count && first == data->commands.size()) return XZ_APP_OK;
    lv_area_t dirty = {LV_COORD_MAX, LV_COORD_MAX, 0, 0};
    const auto include = [&dirty](const xz_app_draw_t& c) {
        dirty.x1 = std::min<int32_t>(dirty.x1, std::min(c.x1, c.x2) - c.width);
        dirty.y1 = std::min<int32_t>(dirty.y1, std::min(c.y1, c.y2) - c.width);
        dirty.x2 = std::max<int32_t>(dirty.x2, std::max(c.x1, c.x2) + c.width);
        dirty.y2 = std::max<int32_t>(dirty.y2, std::max(c.y1, c.y2) + c.width);
    };
    for (uint32_t i = first; i < count; ++i) include(commands[i]);
    for (uint32_t i = first; i < data->commands.size(); ++i) include(data->commands[i]);
    if (count) data->commands.assign(commands, commands + count);
    else data->commands.clear();
    lv_area_t origin;
    lv_obj_get_coords(object, &origin);
    dirty.x1 += origin.x1;
    dirty.x2 += origin.x1;
    dirty.y1 += origin.y1;
    dirty.y2 += origin.y1;
    lv_obj_invalidate_area(object, &dirty);
    return XZ_APP_OK;
}
void ClearChildren(void* parent) {
    if (parent) lv_obj_clean(static_cast<lv_obj_t*>(parent));
}
int RequestScreenChange(void* host) {
    auto* state = Active(host);
    if (!state || state->closing || (StandbyScreen::IsActive() || StandbyScreen::IsResuming())) return XZ_APP_ERR_NOT_READY;
    state->transition_sequence = EpdGetStats().submitted;
    state->transition_pending = true;
    state->foreground = false;
    EpdRequestScreenChange();
    return XZ_APP_OK;
}
void ReleaseVibration(State& state) {
    if (state.vibration_until && MonotonicMs() < state.vibration_until)
        Board::GetInstance().SetVibration(false);
    state.vibration_until = 0;
    state.vibration_hold.reset();
}
int Vibrate(void* host, uint32_t duration_ms) {
    auto* state = Active(host);
    if (!duration_ms || duration_ms > 15000) return XZ_APP_ERR_INVALID;
    if (!state || state->closing || !state->foreground_timer || state->transition_pending ||
        (StandbyScreen::IsActive() || StandbyScreen::IsResuming()) || lv_obj_get_screen(state->root) != lv_screen_active()) return XZ_APP_ERR_NOT_READY;
    const uint64_t now = MonotonicMs();
    if (now < state->vibration_until) return XZ_APP_ERR_NOT_READY;
    if (!HapticIsEnabled()) return XZ_APP_OK;
    state->vibration_hold.emplace(PowerNeed::StandbyInhibit);
    state->vibration_until = now + duration_ms;
    Board::GetInstance().PulseVibration(static_cast<int>(duration_ms));
    return XZ_APP_OK;
}
void UpdateForeground(State& state) {
    const uint64_t now = MonotonicMs() - StandbyScreen::TotalDurationMs();
    if (state.transition_pending && static_cast<int32_t>(EpdGetStats().completed - state.transition_sequence) > 0)
        state.transition_pending = false;
    const bool active = !state.transition_pending && !(StandbyScreen::IsActive() || StandbyScreen::IsResuming()) &&
        lv_obj_get_screen(state.root) == lv_screen_active();
    if (state.vibration_until && (!active || MonotonicMs() >= state.vibration_until)) ReleaseVibration(state);
    const uint64_t elapsed = now - state.foreground_last;
    if (active && state.foreground) state.foreground_elapsed += elapsed;
    state.foreground_last = now;
    state.foreground = active;
    AppAudioSetForeground(state.audio, active && !state.closing);
    AppRecordForeground(state.recorder, active && !state.closing);
    const bool sample = active && state.acceleration_requested;
    // 休眠入口会清除驱动订阅，因此唤醒后重复应用启用请求。
    if (sample || state.acceleration_running) MetalioAccel_SetAppTracking(sample);
    state.acceleration_running = sample;
}
void ForegroundTick(lv_timer_t* timer) {
    auto* state = static_cast<State*>(lv_timer_get_user_data(timer));
    UpdateForeground(*state);
}
int SetAcceleration(void* host, int enabled) {
    auto* state = Active(host);
    if (!state || state->closing || !state->foreground_timer) return XZ_APP_ERR_NOT_READY;
    state->acceleration_requested = enabled != 0;
    UpdateForeground(*state);
    return XZ_APP_OK;
}
int GetAcceleration(void* host, xz_app_acceleration_t* out) {
    auto* state = Active(host);
    if (!out) return XZ_APP_ERR_INVALID;
    *out = {};
    if (!state || !state->foreground || !state->acceleration_requested) return XZ_APP_ERR_NOT_READY;
    int x, y, z;
    if (!MetalioAccel_GetAppGravity(&x, &y, &z)) return XZ_APP_ERR_NOT_READY;
    *out = {x, y, z};
    return XZ_APP_OK;
}
uint64_t ForegroundMs(void* host) {
    auto* state = Active(host);
    if (!state) return 0;
    UpdateForeground(*state);
    return state->foreground_elapsed;
}
void Notify(void* host, const char* text) {
    auto* state = Active(host);
    if (state && !state->closing && !(StandbyScreen::IsActive() || StandbyScreen::IsResuming()) && lv_obj_get_screen(state->root) == lv_screen_active()) ScreenShowToast(text);
}
int AudioCaps(void* host, xz_app_audio_caps_t* result) {
    auto* state = Active(host);
    return state && !state->closing ? AppAudioGetCaps(result) : XZ_APP_ERR_NOT_READY;
}
int AudioPlay(void* host, const xz_app_audio_config_t* config) {
    auto* state = Active(host);
    if (!state || state->closing) return XZ_APP_ERR_NOT_READY;
    if (AppRecordBusy(state->recorder)) return XZ_APP_ERR_BUSY;
    return AppAudioPlay(state->audio, config);
}
int AudioPause(void* host, int paused) {
    auto* state = Active(host);
    return state && !state->closing ? AppAudioPause(state->audio, paused != 0) : XZ_APP_ERR_NOT_READY;
}
int AudioStop(void* host) {
    auto* state = Active(host);
    return state && !state->closing ? AppAudioStop(state->audio) : XZ_APP_ERR_NOT_READY;
}
int AudioVolume(void* host, uint32_t volume) {
    auto* state = Active(host);
    return state && !state->closing ? AppAudioSetVolume(volume) : XZ_APP_ERR_NOT_READY;
}
int AudioStatus(void* host, xz_app_audio_status_t* result) {
    auto* state = Active(host);
    return state && !state->closing ? AppAudioGetStatus(state->audio, result) : XZ_APP_ERR_NOT_READY;
}
bool PrepareRecorder(State& state) {
    if (!state.recorder) state.recorder = AppRecordCreate(state.storage_key);
    if (!state.recorder) return false;
    UpdateForeground(state);
    return true;
}
int RecordCaps(void* host, xz_app_audio_caps_t* result) {
    auto* state = Active(host);
    return state && !state->closing ? AppRecordCaps(result) : XZ_APP_ERR_NOT_READY;
}
int RecordStart(void* host) {
    auto* state = Active(host);
    if (!state || state->closing) return XZ_APP_ERR_NOT_READY;
    xz_app_audio_caps_t caps {};
    caps.struct_size = sizeof(caps);
    const int supported = AppRecordCaps(&caps);
    if (supported != XZ_APP_OK) return supported;
    if (AppAudioBusy(state->audio)) return XZ_APP_ERR_BUSY;
    if (!PrepareRecorder(*state)) return XZ_APP_ERR_MEMORY;
    return AppRecordStart(state->recorder);
}
int RecordPause(void* host, int paused) {
    auto* state = Active(host);
    return state && !state->closing ? AppRecordPause(state->recorder, paused != 0) : XZ_APP_ERR_NOT_READY;
}
int RecordStop(void* host) {
    auto* state = Active(host);
    return state && !state->closing ? AppRecordStop(state->recorder) : XZ_APP_ERR_NOT_READY;
}
int RecordStatus(void* host, xz_app_record_status_t* result) {
    auto* state = Active(host);
    if (!state || state->closing) return XZ_APP_ERR_NOT_READY;
    if (!PrepareRecorder(*state)) return XZ_APP_ERR_MEMORY;
    return AppRecordStatus(state->recorder, result);
}

}
void Bind(State& state, xz_app_api_t& api, lv_obj_t* root, const AppStore::Item& item) {
    const auto& app_id = item.app_id;
    const auto& installed_name = item.installed_name;
    state.root = root;
    state.storage_key = ValidKey(app_id.c_str()) && installed_name.size() <= 72 &&
        SdIsValidFileName(installed_name.c_str()) && installed_name.front() != '.' ? installed_name+"--"+app_id : "";
    state.open = true;
    state.audio = AppAudioCreate(item);
    if (!state.audio) ESP_LOGE(TAG, "operation=audio_bind stage=allocate result=failed");
    api.capabilities = XZ_APP_CAP_TIME | XZ_APP_CAP_TIMER | XZ_APP_CAP_WIDGETS | XZ_APP_CAP_EXIT | XZ_APP_CAP_NOTICE | XZ_APP_CAP_KEEP_AWAKE;
    if (!state.storage_key.empty()) api.capabilities |= XZ_APP_CAP_STORAGE;
    state.foreground_last = MonotonicMs() - StandbyScreen::TotalDurationMs();
    state.foreground_timer = lv_timer_create(ForegroundTick, 200, &state);
    if (!state.foreground_timer) ESP_LOGE(TAG, "operation=foreground_timer stage=allocate result=failed");
    api.capabilities |= XZ_APP_CAP_DRAWING | XZ_APP_CAP_BACK | XZ_APP_CAP_SCREEN_CHANGE;
    if (state.foreground_timer) api.capabilities |= XZ_APP_CAP_ACCELERATION;
    api.clear_children = ClearChildren;
    api.drawing = CreateDrawing;
    api.draw = Draw;
    api.set_acceleration = SetAcceleration;
    api.get_acceleration = GetAcceleration;
    api.foreground_ms = ForegroundMs;
    api.set_on_back = OnBack;
    api.request_screen_change = RequestScreenChange;
    api.vibrate = Vibrate;
    if (state.foreground_timer) api.capabilities |= XZ_APP_CAP_VIBRATION;
    if (state.foreground_timer && state.audio) api.capabilities |= XZ_APP_CAP_AUDIO;
    if (state.foreground_timer && !state.storage_key.empty()) api.capabilities |= XZ_APP_CAP_RECORDING;
    api.record_get_caps = RecordCaps;
    api.record_start = RecordStart;
    api.record_pause = RecordPause;
    api.record_stop = RecordStop;
    api.record_get_status = RecordStatus;
    api.audio_get_caps = AudioCaps;
    api.audio_play = AudioPlay;
    api.audio_pause = AudioPause;
    api.audio_stop = AudioStop;
    api.audio_set_volume = AudioVolume;
    api.audio_get_status = AudioStatus;
    api.host_context = &state;
    api.get_local_time = LocalTime;
    api.monotonic_ms = MonotonicMs;
    api.timer_start = StartTimer;
    api.timer_cancel = CancelTimer;
    api.storage_read = ReadData;
    api.storage_write = WriteData;
    api.storage_remove = RemoveData;
    api.container = Container;
    api.input = Input;
    api.get_text = GetText;
    api.keyboard = Keyboard;
    api.keyboard_close = CloseKeyboard;
    api.progress = Progress;
    api.set_progress = SetProgress;
    api.set_on_exit = OnExit;
    api.notify = Notify;
    api.set_keep_awake = KeepAwake;
}
void Close(State& state) {
    if (!state.open || state.closing) return;
    state.closing = true;
    AppRecordClose(state.recorder);
    state.recorder.reset();
    AppAudioStop(state.audio);
    AppAudioSetForeground(state.audio, false);
    state.audio.reset();
    ReleaseVibration(state);
    state.on_back = nullptr;
    state.back_context = nullptr;
    if (state.foreground_timer) lv_timer_delete(state.foreground_timer);
    state.foreground_timer = nullptr;
    if (state.acceleration_running) MetalioAccel_SetAppTracking(false);
    state.acceleration_requested = false;
    state.acceleration_running = false;
    state.awake.reset();
    for (auto& timer : state.timers) if (timer.handle) {
        lv_timer_delete(timer.handle);
        timer = {};
    }
    if (state.keyboard) lv_obj_delete(state.keyboard);
    if (state.on_exit) state.on_exit(state.exit_context);
    state.on_exit = nullptr;
    state.open = false;
    ESP_LOGD(TAG,"operation=close result=success");
}
}
