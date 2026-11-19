#include <stddef.h>
#include <stdint.h>
#include "app_api.h"
#include "maze_engine.h"
#include "music_assets.h"

static const xz_app_api_t* api;
static Maze maze;
static xz_app_draw_t commands[700];
static unsigned command_count;
static void* page;
static void* board;
static void* time_label;
static void* score_label;
static void* modal_layer;
static int zh, selected, phase, save_failed, sensor_warned;
static int music_requested, music_warned;
static int music_enabled = 1;
typedef struct {
    uint8_t highest, recent; // 得分；recent 为 255 表示尚无完成记录
    uint32_t best_ms, recent_ms; // 成功通关时长；零表示尚未通关
} MazeRecord;
static MazeRecord records[4];
static uint32_t random_seed = 2026;
static uint64_t last_tick, remaining, last_move, missing_since;
static int baseline_x, baseline_y, filtered_x, filtered_y, calibrated;
static int width, height;
static char text[128];
static unsigned text_length;
static const int limits[4] = {60, 75, 90, 90};
static const int columns[4] = {7, 9, 9, 9};
static const int rows[4] = {11, 15, 15, 15};
static const uint32_t seeds[3] = {13, 37, 149};
static const char* names[2][4] = {
    {"First steps", "The passages", "Into the maze", "Random maze"},
    {"初探", "回廊", "纵深", "随机迷宫"}
};
static const char* level_names[2][4] = {
    {"Level 1", "Level 2", "Level 3", "Random"},
    {"第一关", "第二关", "第三关", "随机模式"}
};
enum { HOME, PLAYING, PAUSED, RETRY, WON, LOST, LANGUAGE, SETTINGS };
enum { BACK = 10, PAUSE, ASK_RETRY, RESUME, RESTART, NEXT, LANG, CHINESE, ENGLISH, MENU,
       OPEN_SETTINGS, MUSIC_TOGGLE, SETTINGS_BACK };

static void Click(void* context);
static void Home(void);
static void Start(int level);

static void Empty(void) {
    text_length = 0;
    text[0] = 0;
}
static void Add(const char* value) {
    while (*value && text_length + 1 < sizeof(text)) text[text_length++] = *value++;
    text[text_length] = 0;
}
static void Number(unsigned value) {
    char digits[11];
    unsigned count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (count) {
        char digit[2] = {digits[--count], 0};
        Add(digit);
    }
}
static int X(int value) { return value * width / 684; }
static int Y(int value) { return value * height / 1162; }
static const char* T(const char* cn, const char* en) { return zh ? cn : en; }
static void Style(void* widget, int size, int border, int black) {
    const xz_app_style_t style = {(uint16_t)size, XZ_APP_ALIGN_LEFT, 12, (uint8_t)border, (uint8_t)black};
    if (widget) api->set_style(widget, &style);
}
static void* Label(void* parent, int x, int y, int w, int h, const char* value, int size) {
    void* object = api->label(parent, X(x), Y(y), X(w), Y(h), value);
    Style(object, size, 0, 0);
    return object;
}
static void* Button(void* parent, int x, int y, int w, int h, const char* value, int key, int black) {
    void* object = api->button(parent, X(x), Y(y), X(w), Y(h), value, Click, (void*)(uintptr_t)key);
    const xz_app_style_t style = {25, XZ_APP_ALIGN_CENTER, 12, 2, (uint8_t)black};
    if (object) api->set_style(object, &style);
    return object;
}
static void Notice(const char* cn, const char* en) {
    api->notify(api->host_context, T(cn, en));
}
static void MusicStop(void) {
    if (music_requested) api->audio_stop(api->host_context);
    music_requested = 0;
}
static void MusicStart(int track) {
    if (!music_enabled) return;
    const xz_app_audio_config_t config = {
        sizeof(config), XZ_APP_AUDIO_ASSET, XZ_APP_AUDIO_OGG_OPUS, 1, 0,
        music_offsets[track], music_lengths[track]
    };
    music_warned = 0;
    music_requested = api->audio_play(api->host_context, &config) == XZ_APP_OK;
    if (!music_requested) Notice("背景音乐未就绪", "Background music unavailable.");
}
static void MusicPause(int paused) {
    if (!music_requested) return;
    const int result = api->audio_pause(api->host_context, paused);
    if (result != XZ_APP_OK && result != XZ_APP_ERR_NOT_READY)
        Notice("背景音乐控制失败", "Unable to control background music.");
}
static uint32_t Read32(const uint8_t* data) {
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}
static void Write32(uint8_t* data, uint32_t value) {
    for (int i = 0; i < 4; ++i) data[i] = (uint8_t)(value >> (i * 8));
}
static void Save(void) {
    uint8_t data[43];
    data[0] = 3;
    data[1] = (uint8_t)zh;
    data[42] = (uint8_t)music_enabled;
    for (int i = 0; i < 4; ++i) {
        uint8_t* item = data + 2 + i * 10;
        item[0] = records[i].highest;
        item[1] = records[i].recent;
        Write32(item + 2, records[i].best_ms);
        Write32(item + 6, records[i].recent_ms);
    }
    const int result = api->storage_write(api->host_context, "records", data, sizeof(data));
    if (result != XZ_APP_OK && !save_failed) Notice("保存失败，请检查 SD 卡", "Save failed. Check SD card.");
    save_failed = result != XZ_APP_OK;
}
static void RecordScoreText(const char* label, unsigned score) {
    Empty();
    Add(label);
    Add(" ");
    if (score == 255) Add("—");
    else Number(score);
}
static void RecordTimeText(const char* label, uint32_t milliseconds) {
    Empty();
    Add(label);
    Add(" ");
    if (!milliseconds) {
        Add("—");
        return;
    }
    const uint32_t tenths = (milliseconds + 50) / 100;
    Number(tenths / 10);
    Add(".");
    Number(tenths % 10);
    Add("s");
}
static void RecordResult(int won) {
    MazeRecord* record = &records[selected];
    record->recent = (uint8_t)MazeScore(&maze);
    if (record->recent > record->highest) record->highest = record->recent;
    if (won) {
        record->recent_ms = (uint32_t)((uint64_t)limits[selected] * 1000 - remaining);
        if (!record->recent_ms) record->recent_ms = 1;
        if (!record->best_ms || record->recent_ms < record->best_ms) record->best_ms = record->recent_ms;
    }
    Save();
}
static void ScoreText(int score) {
    Empty();
    Add(T("得分 ", "Score "));
    Number((unsigned)score);
    Add(" / 100");
}
static void TimeText(void) {
    const unsigned seconds = (unsigned)((remaining + 999) / 1000);
    Empty();
    Number(seconds / 60);
    Add(":");
    if (seconds % 60 < 10) Add("0");
    Number(seconds % 60);
}
static void Command(int x1, int y1, int x2, int y2, int kind, int thickness, int filled) {
    if (command_count >= sizeof(commands) / sizeof(commands[0])) return;
    xz_app_draw_t* c = &commands[command_count++];
    c->x1 = (int16_t)x1;
    c->y1 = (int16_t)y1;
    c->x2 = (int16_t)x2;
    c->y2 = (int16_t)y2;
    c->kind = (uint8_t)kind;
    c->width = (uint8_t)thickness;
    c->filled = (uint8_t)filled;
    c->reserved = 0;
}
static void DrawMaze(void) {
    const int w = X(636), h = Y(1048);
    const int inner_w = w - 12, inner_h = h - 12;
    command_count = 0;
    Command(3, 3, w - 4, h - 4, 1, 5, 0);
    for (int cell = 0; cell < maze.columns * maze.rows; ++cell) {
        const int x = cell % maze.columns, y = cell / maze.columns;
        const int left = 6 + x * inner_w / maze.columns;
        const int right = 6 + (x + 1) * inner_w / maze.columns;
        const int top = 6 + y * inner_h / maze.rows;
        const int bottom = 6 + (y + 1) * inner_h / maze.rows;
        if (y && (maze.walls[cell] & 1)) Command(left, top, right, top, 0, 5, 0);
        if (x && (maze.walls[cell] & 8)) Command(left, top, left, bottom, 0, 5, 0);
    }
    int cell_size = inner_w / maze.columns;
    if (inner_h / maze.rows < cell_size) cell_size = inner_h / maze.rows;
    const int radius = cell_size * 23 / 100;
    // 五角星使用整数顶点，避免引入浮点三角函数。
    static const int8_t star_x[10] = {0, 24, 95, 38, 59, 0, -59, -38, -95, -24};
    static const int8_t star_y[10] = {-100, -32, -31, 12, 81, 40, 81, 12, -31, -32};
    for (int i = 0; i < 5; ++i) {
        if (maze.collected & (1 << i)) continue;
        const int cell = maze.stars[i];
        const int cx = 6 + (2 * (cell % maze.columns) + 1) * inner_w / (2 * maze.columns);
        const int cy = 6 + (2 * (cell / maze.columns) + 1) * inner_h / (2 * maze.rows);
        for (int j = 0; j < 10; ++j) {
            const int k = (j + 1) % 10;
            Command(cx + star_x[j] * radius * 115 / 10000, cy + star_y[j] * radius * 115 / 10000,
                    cx + star_x[k] * radius * 115 / 10000, cy + star_y[k] * radius * 115 / 10000, 0, 2, 0);
        }
    }
    const int goal_x = 6 + (2 * maze.columns - 1) * inner_w / (2 * maze.columns);
    const int goal_y = 6 + (2 * maze.rows - 1) * inner_h / (2 * maze.rows);
    Command(goal_x - radius, goal_y - radius, goal_x + radius, goal_y + radius, 2, 4, 0);
    const int ball_x = 6 + (2 * (maze.position % maze.columns) + 1) * inner_w / (2 * maze.columns);
    const int ball_y = 6 + (2 * (maze.position / maze.columns) + 1) * inner_h / (2 * maze.rows);
    Command(ball_x - radius, ball_y - radius, ball_x + radius, ball_y + radius, 2, 1, 1);
    if (api->draw(board, commands, command_count) != XZ_APP_OK) Notice("迷宫绘制失败", "Unable to draw maze.");
}
static void NewPage(void) {
    api->clear_children(api->root);
    modal_layer = 0;
    board = time_label = score_label = 0;
    page = api->container(api->root, 0, 0, width, height, 0);
}
static void Home(void) {
    const int from_settings = phase == SETTINGS || phase == LANGUAGE;
    if (!from_settings) MusicStop();
    if (phase != HOME && phase != LANGUAGE && phase != SETTINGS && api->request_screen_change(api->host_context) != XZ_APP_OK)
        Notice("画面切换未就绪，请重试", "Screen transition unavailable. Please retry.");
    phase = HOME;
    api->set_acceleration(api->host_context, 0);
    api->set_keep_awake(api->host_context, 0);
    NewPage();
    Label(page, 24, 30, zh ? 130 : 240, 56, T("重力迷宫", "Gravity Maze"), zh ? 30 : 36);
    Label(page, zh ? 154 : 251, 35, zh ? 350 : 253, 40, T("倾斜设备，控制小球", "Tilt to move"), 25);
    Button(page, 524, 28, 136, 64, T("设置", "Settings"), OPEN_SETTINGS, 0);
    for (int i = 0; i < 4; ++i) {
        const int top = 126 + i * 252;
        Button(page, 24, top, 636, 232, "", i, 0);
        Label(page, 48, top + 86, 142, 48, level_names[zh][i], 30);
        Label(page, 212, top + 42, 360, 52, names[zh][i], 30);
        Empty();
        if (i < 3) {
            static const char* difficulties[2][3] = {{"Easy", "Medium", "Hard"}, {"简单", "中等", "困难"}};
            Add(difficulties[zh][i]);
            Add(" · ");
        }
        Number((unsigned)limits[i]);
        Add(T(" 秒", " s"));
        Label(page, 212, top + 100, 360, 40, text, 25);
        RecordScoreText(T("最高分", "High score"), records[i].recent == 255 && !records[i].highest ? 255 : records[i].highest);
        Label(page, 212, top + 150, 176, 32, text, 25);
        RecordScoreText(T("最近得分", "Last score"), records[i].recent);
        Label(page, 400, top + 150, 176, 32, text, 25);
        RecordTimeText(T("最佳通关", "Best"), records[i].best_ms);
        Label(page, 212, top + 184, 176, 32, text, 25);
        RecordTimeText(T("最近通关", "Last"), records[i].recent_ms);
        Label(page, 400, top + 184, 176, 32, text, 25);
        void* arrow = api->drawing(page, X(605), Y(top + 85), X(38), Y(48));
        const xz_app_draw_t chevron[] = {
            {(int16_t)X(10), (int16_t)Y(10), (int16_t)X(22), (int16_t)Y(24), 0, 3, 0, 0},
            {(int16_t)X(22), (int16_t)Y(24), (int16_t)X(10), (int16_t)Y(38), 0, 3, 0, 0}
        };
        if (!arrow || api->draw(arrow, chevron, 2) != XZ_APP_OK) Notice("箭头绘制失败", "Unable to draw arrow.");
    }
    if (!from_settings) MusicStart(0);
}
static void GamePage(void) {
    NewPage();
    Button(page, 24, 12, 56, 66, "<", BACK, 0);
    Label(page, 92, 10, 190, 42, level_names[zh][selected], 30);
    ScoreText(MazeScore(&maze));
    score_label = Label(page, 92, 54, 200, 40, text, 25);
    TimeText();
    time_label = Label(page, 300, 24, 100, 50, text, 30);
    Button(page, 416, 12, 112, 66, T("重试", "Retry"), ASK_RETRY, 0);
    Button(page, 544, 12, 116, 66, T("暂停", "Pause"), PAUSE, 1);
    board = api->drawing(page, X(24), Y(102), X(636), Y(1048));
    if (!board) {
        Home();
        Notice("内存不足，无法创建迷宫", "Not enough memory to open maze.");
        return;
    }
    DrawMaze();
}
static void ShowModal(int mode) {
    if (phase == PLAYING) {
        const uint64_t now = api->foreground_ms(api->host_context);
        const uint64_t elapsed = now - last_tick;
        remaining = elapsed < remaining ? remaining - elapsed : 0;
        last_tick = now;
        if (!remaining) mode = LOST;
    }
    const int just_finished = phase == PLAYING && (mode == WON || mode == LOST);
    if (just_finished) RecordResult(mode == WON);
    phase = mode;
    api->set_acceleration(api->host_context, 0);
    if (just_finished) MusicStop();
    else if (mode == PAUSED || mode == RETRY) MusicPause(1);
    if (just_finished && api->vibrate(api->host_context, 500) != XZ_APP_OK)
        Notice("震动未就绪", "Vibration unavailable.");
    api->set_keep_awake(api->host_context, 0);
    api->set_enabled(page, 0);
    // 全尺寸透明容器截获触摸，背景迷宫保持可见。
    if (!modal_layer) modal_layer = api->container(api->root, 0, 0, width, height, 0);
    else api->clear_children(modal_layer);
    const int result = mode == WON || mode == LOST;
    const int button_count = mode == WON && selected < 2 ? 3 : 2;
    const int modal_width = result ? 480 : mode == RETRY || mode == SETTINGS || mode == LANGUAGE ? 440 : 360;
    const int padding = 32;
    const int content_width = modal_width - padding * 2;
    const int title_height = 44;
    const int button_height = 80;
    const int button_gap = 16;
    const int first_button_y = padding + title_height + 24 + (result ? 44 + 20 : 0);
    const int modal_height = first_button_y + button_count * button_height +
        (button_count - 1) * button_gap + padding;
    void* modal = api->container(modal_layer, X((684 - modal_width) / 2),
        Y((1162 - modal_height) / 2), X(modal_width), Y(modal_height), 0);
    Style(modal, 25, 3, 0);
    const char* title = mode == SETTINGS ? T("设置", "Settings") : mode == LANGUAGE ? T("选择语言", "Language") :
        mode == PAUSED ? T("已暂停", "Paused") : mode == RETRY ? T("重新开始本关？", "Restart this level?") :
        mode == WON ? T("通关", "Complete") : T("时间到了", "Time is up");
    const xz_app_style_t centered = {0, XZ_APP_ALIGN_CENTER, 12, 0, 0};
    api->set_style(Label(modal, padding, padding, content_width, title_height, title, 30), &centered);
    const int second_button_y = first_button_y + button_height + button_gap;
    if (mode == SETTINGS || mode == LANGUAGE) {
        Button(modal, padding, padding, 48, title_height, "<", SETTINGS_BACK, 0);
    }
    if (mode == SETTINGS) {
        Button(modal, padding, first_button_y, content_width, button_height, T("语言", "Language"), LANG, 0);
        Button(modal, padding, second_button_y, content_width, button_height,
            music_enabled ? T("音乐：开启", "Music: On") : T("音乐：关闭", "Music: Off"), MUSIC_TOGGLE, music_enabled);
    } else if (mode == LANGUAGE) {
        Button(modal, padding, first_button_y, content_width, button_height, "简体中文", CHINESE, zh);
        Button(modal, padding, second_button_y, content_width, button_height, "English", ENGLISH, !zh);
    } else if (mode == PAUSED) {
        Button(modal, padding, first_button_y, content_width, button_height, T("继续游戏", "Continue"), RESUME, 1);
        Button(modal, padding, second_button_y, content_width, button_height, T("返回菜单", "Main menu"), MENU, 0);
    } else if (mode == RETRY) {
        Button(modal, padding, first_button_y, content_width, button_height, T("重新开始", "Restart"), RESTART, 1);
        Button(modal, padding, second_button_y, content_width, button_height, T("继续游戏", "Continue"), RESUME, 0);
    } else {
        ScoreText(MazeScore(&maze));
        if (mode == WON) {
            Add(T(" · 用时 ", " · Time "));
            Number((unsigned)((limits[selected] * 1000u - remaining + 999) / 1000));
            Add("s");
        }
        api->set_style(Label(modal, padding, padding + title_height + 24,
            content_width, 44, text, 25), &centered);
        const int next = mode == WON && selected < 2;
        Button(modal, padding, first_button_y, content_width, button_height,
            next ? T("下一关", "Next level") : T("重试", "Retry"), next ? NEXT : RESTART, 1);
        Button(modal, padding, second_button_y, content_width, button_height,
            next ? T("重试", "Retry") : T("返回菜单", "Main menu"), next ? RESTART : MENU, 0);
        if (next) Button(modal, padding, second_button_y + button_height + button_gap,
            content_width, button_height, T("返回菜单", "Main menu"), MENU, 0);
    }
}
static void Start(int level) {
    if (api->request_screen_change(api->host_context) != XZ_APP_OK) {
        Notice("画面切换未就绪，请重试", "Screen transition unavailable. Please retry.");
        return;
    }
    selected = level;
    if (level == 3) {
        random_seed ^= (uint32_t)api->monotonic_ms();
        MazeRandom(&random_seed, 0x7fffffffu);
    }
    MazeGenerate(&maze, columns[level], rows[level], level < 3 ? seeds[level] : random_seed);
    remaining = (uint64_t)limits[level] * 1000;
    phase = PLAYING;
    calibrated = sensor_warned = 0;
    missing_since = 0;
    last_tick = last_move = api->foreground_ms(api->host_context);
    api->set_keep_awake(api->host_context, 1);
    api->set_acceleration(api->host_context, 1);
    GamePage();
    if (phase == PLAYING) MusicStart(level + 1);
}
static void Resume(void) {
    // 清除弹层并复用原有迷宫状态；恢复时重新建立握持基准。
    phase = PLAYING;
    calibrated = 0;
    sensor_warned = 0;
    missing_since = 0;
    last_tick = last_move = api->foreground_ms(api->host_context);
    api->set_keep_awake(api->host_context, 1);
    api->set_acceleration(api->host_context, 1);
    GamePage();
    MusicPause(0);
}
static void Click(void* context) {
    const int key = (int)(uintptr_t)context;
    if (phase == HOME) {
        if (key < 4) Start(key);
        else if (key == OPEN_SETTINGS) ShowModal(SETTINGS);
        return;
    }
    if (phase == SETTINGS) {
        if (key == LANG) ShowModal(LANGUAGE);
        else if (key == MUSIC_TOGGLE) {
            music_enabled = !music_enabled;
            if (music_enabled) MusicStart(0);
            else MusicStop();
            Save();
            ShowModal(SETTINGS);
        } else if (key == SETTINGS_BACK) Home();
        return;
    }
    if (phase == LANGUAGE) {
        if (key == SETTINGS_BACK) {
            ShowModal(SETTINGS);
            return;
        }
        if (key != CHINESE && key != ENGLISH) return;
        zh = key == CHINESE;
        Save();
        Home();
        ShowModal(SETTINGS);
        return;
    }
    if (key == BACK || key == MENU) Home();
    else if (phase == PLAYING && key == PAUSE) ShowModal(PAUSED);
    else if (phase == PLAYING && key == ASK_RETRY) ShowModal(RETRY);
    else if ((phase == PAUSED || phase == RETRY) && key == RESUME) Resume();
    else if ((phase == RETRY || phase == WON || phase == LOST) && key == RESTART) Start(selected);
    else if (phase == WON && selected < 2 && key == NEXT) Start(selected + 1);
}
static int Absolute(int value) { return value < 0 ? -value : value; }
static int Back(void* context) {
    (void)context;
    if (phase == HOME) return 0;
    if (phase == LANGUAGE) {
        ShowModal(SETTINGS);
        return 1;
    }
    if (phase == PAUSED || phase == RETRY) Resume();
    else Home();
    return 1;
}
static void Tick(void* context) {
    (void)context;
    if (music_requested && !music_warned) {
        xz_app_audio_status_t status;
        status.struct_size = sizeof(status);
        if (api->audio_get_status(api->host_context, &status) == XZ_APP_OK && status.state == XZ_APP_AUDIO_ERROR) {
            music_warned = 1;
            Notice("背景音乐已停止", "Background music stopped.");
        }
    }
    if (phase != PLAYING) return;
    const uint64_t now = api->foreground_ms(api->host_context);
    const uint64_t elapsed = now - last_tick;
    last_tick = now;
    if (elapsed >= remaining) {
        remaining = 0;
        TimeText();
        api->set_text(time_label, text);
        ShowModal(LOST);
        return;
    }
    const uint64_t previous_seconds = (remaining + 999) / 1000;
    remaining -= elapsed;
    if ((remaining + 999) / 1000 != previous_seconds) {
        TimeText();
        api->set_text(time_label, text);
    }
    xz_app_acceleration_t sample;
    if (api->get_acceleration(api->host_context, &sample) != XZ_APP_OK) {
        calibrated = 0;
        if (!missing_since) missing_since = now;
        if (!sensor_warned && now - missing_since >= 2000) {
            sensor_warned = 1;
            Notice("加速度计未就绪，请重试", "Accelerometer unavailable. Please retry.");
            ShowModal(PAUSED);
        }
        return;
    }
    missing_since = 0;
    if (!calibrated) {
        baseline_x = sample.x;
        baseline_y = sample.y;
        filtered_x = filtered_y = 0;
        calibrated = 1;
        last_move = now;
        return;
    }
    filtered_x = (filtered_x + sample.x - baseline_x) / 2;
    filtered_y = (filtered_y + sample.y - baseline_y) / 2;
    if (now - last_move < 220) return;
    const int ax = Absolute(filtered_x), ay = Absolute(filtered_y);
    int direction = -1;
    if (ax >= 150 && ax >= ay + 40) direction = filtered_x > 0 ? 2 : 0;
    else if (ay >= 150 && ay >= ax + 40) direction = filtered_y > 0 ? 3 : 1;
    if (direction < 0) return;
    last_move = now;
    const int previous_score = MazeScore(&maze);
    if (!MazeMove(&maze, direction)) return;
    const int score = MazeScore(&maze);
    if (score != previous_score) {
        ScoreText(score);
        api->set_text(score_label, text);
    }
    DrawMaze();
    if (maze.position + 1 == maze.columns * maze.rows) ShowModal(WON);
}

int app_main(int argc, char** argv) {
    if (argc < 1 || !argv || !argv[0]) return -1;
    api = (const xz_app_api_t*)argv[0];
    if (api->abi_version != XZ_APP_ABI_VERSION || !XZ_APP_HAS_FIELD(api, audio_get_status)) return XZ_APP_ERR_UNSUPPORTED;
    const uint32_t required = XZ_APP_CAP_TIMER | XZ_APP_CAP_STORAGE | XZ_APP_CAP_WIDGETS |
        XZ_APP_CAP_TIME | XZ_APP_CAP_KEEP_AWAKE | XZ_APP_CAP_NOTICE | XZ_APP_CAP_DRAWING | XZ_APP_CAP_ACCELERATION | XZ_APP_CAP_BACK | XZ_APP_CAP_SCREEN_CHANGE | XZ_APP_CAP_VIBRATION | XZ_APP_CAP_AUDIO;
    if ((api->capabilities & required) != required || !api->foreground_ms || !api->clear_children ||
        !api->drawing || !api->draw || !api->set_acceleration || !api->get_acceleration ||
        !api->timer_start || !api->storage_read || !api->storage_write || !api->set_keep_awake ||
        !api->notify || !api->container || !api->set_style || !api->set_enabled || !api->monotonic_ms ||
        !api->set_on_back || !api->request_screen_change || !api->vibrate || !api->audio_get_caps ||
        !api->audio_play || !api->audio_pause || !api->audio_stop || !api->audio_get_status ||
        api->asset_size != MUSIC_ASSETS_SIZE) return XZ_APP_ERR_UNSUPPORTED;
    xz_app_audio_caps_t audio_caps;
    audio_caps.struct_size = sizeof(audio_caps);
    if (api->audio_get_caps(api->host_context, &audio_caps) != XZ_APP_OK ||
        !(audio_caps.formats & XZ_APP_AUDIO_OGG_OPUS) || !(audio_caps.sources & XZ_APP_AUDIO_ASSET) ||
        audio_caps.sample_rate != 16000 || audio_caps.channels != 1) return XZ_APP_ERR_UNSUPPORTED;
    width = api->width;
    height = api->height;
    if (width < 500 || height < width || width > 1024 || height > 1600) return XZ_APP_ERR_UNSUPPORTED;
    zh = !api->language || api->language[0] != 'e';
    uint8_t data[43];
    const int stored = api->storage_read(api->host_context, "records", data, sizeof(data));
    for (int i = 0; i < 4; ++i) records[i].recent = 255;
    if (stored >= 2 && data[1] <= 1) {
        if (stored == 6 && data[0] == 1) {
            zh = data[1];
            for (int i = 0; i < 4; ++i)
                if (data[i + 2] <= 100 && data[i + 2] % 20 == 0) records[i].highest = data[i + 2];
        } else if ((stored == 42 && data[0] == 2) || (stored == sizeof(data) && data[0] == 3)) {
            zh = data[1];
            if (stored == sizeof(data) && data[42] <= 1) music_enabled = data[42];
            for (int i = 0; i < 4; ++i) {
                const uint8_t* item = data + 2 + i * 10;
                if (item[0] > 100 || item[0] % 20 ||
                    (item[1] != 255 && (item[1] > 100 || item[1] % 20))) continue;
                const uint32_t best_time = Read32(item + 2), recent_time = Read32(item + 6);
                if (best_time > (uint32_t)limits[i] * 1000 || recent_time > (uint32_t)limits[i] * 1000) continue;
                records[i].highest = item[0];
                records[i].recent = item[1];
                records[i].best_ms = best_time;
                records[i].recent_ms = recent_time;
            }
        }
    }
    // 每 220ms 最多移动一格，传感器由宿主以更高频率采样。
    if (!api->timer_start(api->host_context, 220, 1, Tick, 0)) return -1;
    if (api->set_on_back(api->host_context, Back, 0) != XZ_APP_OK) return -1;
    Home();
    return 0;
}
