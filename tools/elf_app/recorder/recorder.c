#include "app_api.h"

/** @brief 提供独立 ELF 所需的本地内存复制 */
void* memcpy(void* destination, const void* source, size_t length) {
    unsigned char* out = destination;
    const unsigned char* in = source;
    for (size_t i = 0; i < length; ++i) out[i] = in[i];
    return destination;
}

/** @brief 提供独立 ELF 所需的本地内存初始化 */
void* memset(void* destination, int value, size_t length) {
    unsigned char* out = destination;
    for (size_t i = 0; i < length; ++i) out[i] = (unsigned char)value;
    return destination;
}

static const xz_app_api_t* api;
static int zh, pending_start;
static xz_app_record_status_t recording;
static xz_app_audio_status_t audio;
static void* state_label;
static void* timer_drawing;
static void* start_button;
static void* stop_button;
static void* info_label;
static void* duration_label;
static void* play_button;
static void* play_stop_button;
static void* progress;
static void* progress_label;
static char output[320];
static unsigned used;
static uint32_t shown_state = UINT32_MAX, shown_seconds = UINT32_MAX;
static uint32_t shown_saved = UINT32_MAX, shown_play = UINT32_MAX, shown_position = UINT32_MAX;
static int shown_error, shown_has_file = -1;
static uint32_t shown_total = UINT32_MAX, shown_percent = UINT32_MAX;

static void Empty(void) { used = 0; output[0] = 0; }
static void Add(const char* value) {
    while (*value && used + 1 < sizeof(output)) output[used++] = *value++;
    output[used] = 0;
}
static void Number(uint32_t value) {
    char digits[11];
    unsigned n = 0;
    do { digits[n++] = '0' + value % 10; value /= 10; } while (value);
    while (n) { char part[2] = {digits[--n], 0}; Add(part); }
}
static void Clock(uint32_t ms) {
    const uint32_t seconds = ms / 1000;
    const unsigned values[3] = {seconds / 3600, seconds / 60 % 60, seconds % 60};
    for (unsigned i = 0; i < 3; ++i) {
        if (i) Add(":");
        if (values[i] < 10) Add("0");
        Number(values[i]);
    }
}
static void Style(void* widget, unsigned size, unsigned align, unsigned radius, unsigned border, unsigned primary) {
    const xz_app_style_t style = {(uint16_t)size, (uint8_t)align, (uint8_t)radius, (uint8_t)border, (uint8_t)primary};
    if (widget) api->set_style(widget, &style);
}
static void* Label(void* parent, int x, int y, int width, int height, const char* value, unsigned size, unsigned align) {
    void* widget = api->label(parent, x, y, width, height, value);
    Style(widget, size, align, 0, 0, 0);
    return widget;
}
static void Click(void* context);
static void* Button(void* parent, int x, int y, int width, int height, const char* value, unsigned action, int primary) {
    void* widget = api->button(parent, x, y, width, height, value, Click, (void*)(uintptr_t)action);
    Style(widget, 25, XZ_APP_ALIGN_CENTER, 14, primary ? 1 : 2, primary);
    return widget;
}
static void TimerDigits(uint32_t seconds) {
    static const uint8_t masks[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
    static const int16_t segments[7][4] = {
        {8, 0, 43, 5}, {46, 8, 51, 46}, {46, 54, 51, 92}, {8, 95, 43, 100},
        {0, 54, 5, 92}, {0, 8, 5, 46}, {8, 48, 43, 53}
    };
    const unsigned values[6] = {seconds / 36000 % 10, seconds / 3600 % 10,
        seconds / 600 % 6, seconds / 60 % 10, seconds / 10 % 6, seconds % 10};
    xz_app_draw_t commands[46];
    unsigned count = 0;
    for (unsigned digit = 0; digit < 6; ++digit) {
        const int offset = digit * 64 + (digit >= 2 ? 24 : 0) + (digit >= 4 ? 24 : 0);
        for (unsigned n = 0; n < 7; ++n) if (masks[values[digit]] & (1u << n)) {
            commands[count++] = (xz_app_draw_t){offset + segments[n][0], segments[n][1],
                offset + segments[n][2], segments[n][3], 1, 1, 1, 0};
        }
    }
    for (unsigned i = 0; i < 2; ++i) {
        const int x = i ? 285 : 133;
        commands[count++] = (xz_app_draw_t){x, 33, x + 5, 38, 1, 1, 1, 0};
        commands[count++] = (xz_app_draw_t){x, 63, x + 5, 68, 1, 1, 1, 0};
    }
    if (api->draw(timer_drawing, commands, count) != XZ_APP_OK)
        api->notify(api->host_context, zh ? "计时显示暂不可用" : "Timer display unavailable");
}
static int RecordingActive(void) {
    return recording.state >= XZ_APP_RECORD_PREPARING && recording.state <= XZ_APP_RECORD_SAVING;
}
static void Error(int result) {
    const char* value = zh ? "操作失败，请检查 SD 卡或稍后重试" : "Operation failed. Check SD card or try again.";
    if (result == XZ_APP_ERR_BUSY) value = zh ? "音频正在使用，请稍后重试" : "Audio is busy. Try again shortly.";
    if (result == XZ_APP_ERR_NOT_READY) value = zh ? "暂未就绪，请稍后重试" : "Not ready. Try again shortly.";
    if (result == XZ_APP_ERR_LIMIT) value = zh ? "录音已达到文件大小上限" : "Recording reached the file size limit.";
    api->notify(api->host_context, value);
}
static void RefreshSavedInfo(void) {
    const int has_file = recording.path[0] != 0;
    if (has_file == shown_has_file && recording.saved_duration_ms == shown_saved) return;
    if (!has_file) {
        api->set_text(info_label, zh ? "还没有录音，点上方开始" : "Nothing saved yet. Tap Start above.");
        api->set_text(duration_label, "--:--:--");
    } else {
        api->set_text(info_label, zh ? "录音已就绪，可播放或重新录制" : "Ready to play or record again.");
        Empty();
        Clock(recording.saved_duration_ms);
        api->set_text(duration_label, output);
    }
    shown_has_file = has_file;
    shown_saved = recording.saved_duration_ms;
}
static void Tick(void* context) {
    (void)context;
    recording.struct_size = sizeof(recording);
    audio.struct_size = sizeof(audio);
    if (api->record_get_status(api->host_context, &recording) != XZ_APP_OK ||
        api->audio_get_status(api->host_context, &audio) != XZ_APP_OK) return;
    if (pending_start) {
        const int result = api->record_start(api->host_context);
        if (result == XZ_APP_OK) {
            pending_start = 0;
            recording.struct_size = sizeof(recording);
            api->record_get_status(api->host_context, &recording);
        }
        else if (result != XZ_APP_ERR_BUSY && result != XZ_APP_ERR_NOT_READY) { pending_start = 0; Error(result); }
    }
    if (recording.result != shown_error) {
        if (recording.result < 0) Error(recording.result);
        shown_error = recording.result;
    }
    const uint32_t state = pending_start ? XZ_APP_RECORD_PREPARING : recording.state;
    if (state != shown_state) {
        const char* cn[] = {"准备就绪", "正在准备…", "正在录音", "已暂停", "正在保存…", "已保存", "未完成"};
        const char* en[] = {"Ready", "Preparing…", "Recording", "Paused", "Saving…", "Saved", "Failed"};
        api->set_text(state_label, state <= XZ_APP_RECORD_ERROR ? (zh ? cn[state] : en[state]) : "");
        api->set_text(start_button, state == XZ_APP_RECORD_RECORDING ? (zh ? "暂停" : "Pause") :
            state == XZ_APP_RECORD_PAUSED ? (zh ? "继续" : "Resume") :
            (state == XZ_APP_RECORD_PREPARING || state == XZ_APP_RECORD_SAVING) ? (zh ? "请稍候" : "Wait") :
            recording.path[0] ? (zh ? "重新录制" : "Record again") : (zh ? "开始录音" : "Start"));
        shown_state = state;
    }
    const uint32_t seconds = recording.duration_ms / 1000;
    if (seconds != shown_seconds) {
        TimerDigits(seconds);
        shown_seconds = seconds;
    }
    RefreshSavedInfo();
    if (audio.state != shown_play) {
        api->set_text(play_button, audio.state == XZ_APP_AUDIO_PAUSED ? (zh ? "继续" : "Resume") :
            (audio.state == XZ_APP_AUDIO_PLAYING || audio.state == XZ_APP_AUDIO_LOADING) ? (zh ? "暂停" : "Pause") :
            (zh ? "播放" : "Play"));
        if (audio.state == XZ_APP_AUDIO_ERROR) Error(audio.result);
        shown_play = audio.state;
    }
    const uint32_t position_ms = audio.state == XZ_APP_AUDIO_STOPPED ? 0 : (uint32_t)audio.position_ms;
    const uint32_t position = position_ms / 1000;
    const uint32_t total = audio.state != XZ_APP_AUDIO_STOPPED && audio.duration_ms ?
        (uint32_t)audio.duration_ms : recording.saved_duration_ms;
    if (position != shown_position || total != shown_total) {
        Empty(); Clock(position_ms); Add(" / "); Clock(total);
        api->set_text(progress_label, output);
        shown_position = position;
        shown_total = total;
    }
    const uint32_t percent = audio.state != XZ_APP_AUDIO_STOPPED && audio.duration_ms ?
        (uint32_t)(audio.position_ms * 100 / audio.duration_ms) : 0;
    if (percent != shown_percent) { api->set_progress(progress, percent); shown_percent = percent; }
    const int active = RecordingActive() || pending_start;
    api->set_enabled(start_button, !pending_start && (recording.state == XZ_APP_RECORD_RECORDING ||
        recording.state == XZ_APP_RECORD_PAUSED || !active));
    api->set_enabled(stop_button, recording.state == XZ_APP_RECORD_RECORDING || recording.state == XZ_APP_RECORD_PAUSED);
    api->set_enabled(play_button, !active && recording.path[0]);
    api->set_enabled(play_stop_button, audio.state == XZ_APP_AUDIO_PLAYING || audio.state == XZ_APP_AUDIO_PAUSED || audio.state == XZ_APP_AUDIO_LOADING);
}
static void Click(void* context) {
    int result = XZ_APP_OK;
    switch ((uintptr_t)context) {
    case 1:
        if (recording.state == XZ_APP_RECORD_RECORDING || recording.state == XZ_APP_RECORD_PAUSED) {
            result = api->record_pause(api->host_context, recording.state != XZ_APP_RECORD_PAUSED);
            break;
        }
        if (pending_start || RecordingActive()) return;
        api->audio_stop(api->host_context);
        pending_start = 1;
        break;
    case 3: result = api->record_stop(api->host_context); break;
    case 4:
        if (audio.state == XZ_APP_AUDIO_PAUSED) result = api->audio_pause(api->host_context, 0);
        else if (audio.state == XZ_APP_AUDIO_PLAYING || audio.state == XZ_APP_AUDIO_LOADING) result = api->audio_pause(api->host_context, 1);
        else {
            const xz_app_audio_config_t config = {sizeof(config), XZ_APP_AUDIO_SD, XZ_APP_AUDIO_WAV_PCM, 0, recording.path, 0, 0};
            result = api->audio_play(api->host_context, &config);
        }
        break;
    case 5: result = api->audio_stop(api->host_context); break;
    }
    if (result != XZ_APP_OK) Error(result);
    Tick(0);
}

int app_main(int argc, char** argv) {
    if (argc != 1 || !argv || !argv[0]) return XZ_APP_ERR_INVALID;
    api = (const xz_app_api_t*)argv[0];
    if (api->abi_version != XZ_APP_ABI_VERSION || !XZ_APP_HAS_FIELD(api, record_get_status)) return XZ_APP_ERR_UNSUPPORTED;
    const uint32_t required = XZ_APP_CAP_RECORDING | XZ_APP_CAP_AUDIO | XZ_APP_CAP_TIMER | XZ_APP_CAP_WIDGETS | XZ_APP_CAP_DRAWING;
    if ((api->capabilities & required) != required || !api->record_get_caps || !api->record_start ||
        !api->record_pause || !api->record_stop || !api->record_get_status || !api->audio_play ||
        !api->audio_pause || !api->audio_stop || !api->audio_get_status || !api->audio_get_caps || !api->label || !api->button ||
        !api->set_text || !api->set_style || !api->set_enabled || !api->progress || !api->set_progress ||
        !api->timer_start || !api->notify || !api->container || !api->drawing || !api->draw || api->width < 600 || api->height < 1000) return XZ_APP_ERR_UNSUPPORTED;
    xz_app_audio_caps_t caps;
    caps.struct_size = sizeof(caps);
    if (api->record_get_caps(api->host_context, &caps) != XZ_APP_OK || !(caps.formats & XZ_APP_AUDIO_WAV_PCM)) return XZ_APP_ERR_UNSUPPORTED;
    caps.struct_size = sizeof(caps);
    if (api->audio_get_caps(api->host_context, &caps) != XZ_APP_OK || !(caps.formats & XZ_APP_AUDIO_WAV_PCM)) return XZ_APP_ERR_UNSUPPORTED;
    zh = !api->language || api->language[0] != 'e';

    const int pad = 24, gap = 16, width = api->width - pad * 2, inner = width - 48;
    const int btn_h = 76, main_w = (inner - gap) * 2 / 3, side_w = inner - gap - main_w;
    const int record_y = 118, record_h = 390;
    const int saved_y = record_y + record_h + 20;
    const int saved_h = api->height - saved_y - 64;

    if (!Label(api->root, pad, 22, width, 44, zh ? "录音机" : "Recorder", 30, XZ_APP_ALIGN_CENTER) ||
        !Label(api->root, pad, 68, width, 36, zh ? "录下这一刻" : "Capture this moment", 25, XZ_APP_ALIGN_CENTER))
        return XZ_APP_ERR_MEMORY;

    void* record_card = api->container(api->root, pad, record_y, width, record_h, 0);
    void* saved_card = api->container(api->root, pad, saved_y, width, saved_h, 0);
    if (!record_card || !saved_card) return XZ_APP_ERR_MEMORY;
    Style(record_card, 0, XZ_APP_ALIGN_LEFT, 18, 1, 0);
    Style(saved_card, 0, XZ_APP_ALIGN_LEFT, 18, 1, 0);

    state_label = Label(record_card, 24, 28, inner, 40, "", 25, XZ_APP_ALIGN_CENTER);
    timer_drawing = api->drawing(record_card, (width - 420) / 2, 96, 420, 102);
    start_button = Button(record_card, 24, record_h - 24 - btn_h, main_w, btn_h, zh ? "开始录音" : "Start", 1, 1);
    stop_button = Button(record_card, 24 + main_w + gap, record_h - 24 - btn_h, side_w, btn_h, zh ? "保存" : "Save", 3, 0);
    if (!state_label || !timer_drawing || !start_button || !stop_button) return XZ_APP_ERR_MEMORY;

    if (!Label(saved_card, 24, 24, inner / 2, 40, zh ? "最近录音" : "Latest", 25, XZ_APP_ALIGN_LEFT)) return XZ_APP_ERR_MEMORY;
    duration_label = Label(saved_card, 24 + inner / 2, 24, inner / 2, 40, "--:--:--", 25, XZ_APP_ALIGN_RIGHT);
    info_label = Label(saved_card, 24, 78, inner, 56, "", 25, XZ_APP_ALIGN_LEFT);
    progress = api->progress(saved_card, 24, saved_h - 24 - btn_h - 72, inner, 10);
    progress_label = Label(saved_card, 24, saved_h - 24 - btn_h - 52, inner, 36, "00:00:00 / 00:00:00", 25, XZ_APP_ALIGN_CENTER);
    play_button = Button(saved_card, 24, saved_h - 24 - btn_h, main_w, btn_h, zh ? "播放" : "Play", 4, 0);
    play_stop_button = Button(saved_card, 24 + main_w + gap, saved_h - 24 - btn_h, side_w, btn_h, zh ? "停止" : "Stop", 5, 0);
    if (!duration_label || !info_label || !progress || !progress_label || !play_button || !play_stop_button) return XZ_APP_ERR_MEMORY;

    if (!Label(api->root, pad, api->height - 48, width, 32,
        zh ? "单条录音 · 重新录制将覆盖上一条" : "One take · New recording replaces the last",
        25, XZ_APP_ALIGN_CENTER)) return XZ_APP_ERR_MEMORY;

    api->set_enabled(start_button, 0);
    api->set_enabled(stop_button, 0);
    api->set_enabled(play_button, 0);
    api->set_enabled(play_stop_button, 0);
    if (!api->timer_start(api->host_context, 400, 1, Tick, 0)) return XZ_APP_ERR_MEMORY;
    Tick(0);
    return XZ_APP_OK;
}
