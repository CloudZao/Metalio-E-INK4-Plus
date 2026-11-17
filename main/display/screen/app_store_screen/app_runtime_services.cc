#include "app_runtime_services.h"
#include "screen_common.h"
#include "screen_keyboard.h"
#include "standby_screen/standby_screen.h"
#include "reader/http_download_file.h"
#include "sd_access.h"
#include "sd_paths.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <new>
#include <sys/stat.h>
#include <unistd.h>

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
    if (StandbyScreen::IsActive() || lv_obj_get_screen(timer->root) != lv_screen_active()) return;
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
            if (!data->input || !data->state->open || data->state->closing || StandbyScreen::IsActive() ||
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
int KeepAwake(void* host, int enabled) {
    auto* state = Active(host);
    if (!state || state->closing) return XZ_APP_ERR_NOT_READY;
    if (enabled && !state->awake) state->awake.emplace(PowerNeed::StandbyInhibit);
    else if (!enabled) state->awake.reset();
    return XZ_APP_OK;
}
void Notify(void* host, const char* text) {
    auto* state = Active(host);
    if (state && !state->closing && !StandbyScreen::IsActive() && lv_obj_get_screen(state->root) == lv_screen_active()) ScreenShowToast(text);
}
}
void Bind(State& state, xz_app_api_t& api, lv_obj_t* root, const std::string& app_id, const std::string& installed_name) {
    state.root = root;
    state.storage_key = ValidKey(app_id.c_str()) && installed_name.size() <= 72 &&
        SdIsValidFileName(installed_name.c_str()) && installed_name.front() != '.' ? installed_name+"--"+app_id : "";
    state.open = true;
    api.capabilities = XZ_APP_CAP_TIME | XZ_APP_CAP_TIMER | XZ_APP_CAP_WIDGETS | XZ_APP_CAP_EXIT | XZ_APP_CAP_NOTICE | XZ_APP_CAP_KEEP_AWAKE;
    if (!state.storage_key.empty()) api.capabilities |= XZ_APP_CAP_STORAGE;
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
