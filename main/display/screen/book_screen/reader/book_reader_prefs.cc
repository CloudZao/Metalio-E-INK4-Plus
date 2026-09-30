#include "book_screen/reader/book_reader_prefs.h"

#include "assets/lang_config.h"
#include "epd_gray_aa.h"
#include "epd_i1_glyph_thin.h"
#include "sd_paths.h"
#include "settings.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern "C" {
volatile int epd_i1_glyph_aa_enabled = 0;

void epd_i1_glyph_aa_plot(int32_t abs_x, int32_t abs_y, uint8_t mask_val) {
    epd_gray_aa_plot_logical(abs_x, abs_y, mask_val);
}
}


static constexpr const char* TAG = "BookPrefs";
static constexpr const char* kNvsNs = "reader";
static constexpr const char* kNvsFontKey = "font";
static constexpr const char* kNvsSpacingKey = "spacing";
static constexpr const char* kNvsMarginKey = "margin";
static constexpr const char* kNvsHideProgKey = "hide_prog";
static constexpr const char* kNvsFooterMaskKey = "foot_msk";
static constexpr const char* kNvsAaKey = "aa";
static constexpr const char* kNvsUnderlineKey = "underline";
static constexpr const char* kNvsShelfViewKey = "shelf_view";
static constexpr const char* kNvsShelfPctKey = "shelf_pct";
static constexpr const char* kNvsOrientKey = "orient";
static constexpr const char* kDefaultFile = "misans_25_2.ef"; // SD 现网默认 25px/2bpp
static constexpr size_t kMaxFileLen = 63;
static constexpr int kDefaultSpacingPreset = 1;  // 标准
static constexpr int kDefaultMarginPreset = 1;   // 标准（约原 kListPad=12）
static constexpr int kDefaultFooterMask = kBookReaderFooterDefault;
static constexpr int kDefaultShelfView = kBookReaderShelfViewCover;
static constexpr int kDefaultShelfPct = 0;  // 进度角标默认关

struct BookSpacingPreset {
    int line_gap;
    int para_gap;
};

struct BookMarginPreset {
    int left;
    int right;
    int top;
    int bottom;
};

// 行距档为主（每档明显拉开行间）；段距≈行距+固定余量，避免「调行距却像调段距」
static constexpr BookSpacingPreset kSpacingTable[kBookReaderSpacingPresetCount] = {
    {0, 10},
    {4, 12},
    {14, 20},
    {22, 26},
};

// 成套页边距：与分页 viewport 共用，避免只改 UI pad 导致裁切/留白不一致
static constexpr BookMarginPreset kMarginTable[kBookReaderMarginPresetCount] = {
    {8, 8, 8, 6},
    {12, 12, 12, 8},
    {20, 20, 18, 12},
    {28, 28, 24, 16},
};

static char s_font_file[kMaxFileLen + 1] = {};
static int s_spacing_preset = kDefaultSpacingPreset;
static int s_margin_preset = kDefaultMarginPreset;
static int s_footer_mask = kDefaultFooterMask;
static int s_aa = 0;
static int s_underline = 0;
static int s_shelf_view = kDefaultShelfView;
static int s_shelf_pct = kDefaultShelfPct;
static int s_orient = kBookReaderOrientPortrait;
static bool s_ready = false;


static bool IsSafeBasename(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    const size_t n = std::strlen(name);
    if (n == 0 || n > kMaxFileLen) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        const char c = name[i];
        if (c == '/' || c == '\\' || c == ':' || c == '\0') {
            return false;
        }
    }
    return true;
}
static int ClampSpacing(int preset) {
    if (preset < 0 || preset >= kBookReaderSpacingPresetCount) {
        return kDefaultSpacingPreset;
    }
    return preset;
}

static int ClampMargin(int preset) {
    if (preset < 0 || preset >= kBookReaderMarginPresetCount) {
        return kDefaultMarginPreset;
    }
    return preset;
}

static int ClampUnderline(int mode) {
    if (mode < 0 || mode >= kBookReaderUnderlineModeCount) {
        return kBookReaderUnderlineOff;
    }
    return mode;
}

static int ClampShelfView(int view) {
    if (view != kBookReaderShelfViewList) {
        return kBookReaderShelfViewCover;
    }
    return kBookReaderShelfViewList;
}

static int ClampFooterMask(int mask) {
    return mask & kBookReaderFooterAll;
}

static int ClampOrient(int orient) {
    if (orient < 0 || orient >= kBookReaderOrientCount) {
        return kBookReaderOrientPortrait;
    }
    return orient;
}

static void LoadFromNvs() {
    Settings settings(kNvsNs, false);
    const std::string v = settings.GetString(kNvsFontKey, kDefaultFile);
    if (IsSafeBasename(v.c_str())) {
        std::snprintf(s_font_file, sizeof(s_font_file), "%s", v.c_str());
    } else {
        std::snprintf(s_font_file, sizeof(s_font_file), "%s", kDefaultFile);
    }
    s_spacing_preset = ClampSpacing(settings.GetInt(kNvsSpacingKey, kDefaultSpacingPreset));
    s_margin_preset = ClampMargin(settings.GetInt(kNvsMarginKey, kDefaultMarginPreset));
    // foot_msk 优先；无则按旧 hide_prog 迁移
    const int raw_mask = settings.GetInt(kNvsFooterMaskKey, -1);
    if (raw_mask >= 0) {
        s_footer_mask = ClampFooterMask(raw_mask);
    } else {
        const int hide = settings.GetInt(kNvsHideProgKey, 0) != 0 ? 1 : 0;
        s_footer_mask = hide ? 0 : kDefaultFooterMask;
    }
    s_aa = settings.GetInt(kNvsAaKey, 0) != 0 ? 1 : 0;
    s_underline = ClampUnderline(settings.GetInt(kNvsUnderlineKey, 0));
    s_shelf_view = ClampShelfView(settings.GetInt(kNvsShelfViewKey, kDefaultShelfView));
    s_shelf_pct = settings.GetInt(kNvsShelfPctKey, kDefaultShelfPct) != 0 ? 1 : 0;
    s_orient = ClampOrient(settings.GetInt(kNvsOrientKey, kBookReaderOrientPortrait));
    epd_i1_glyph_set_aa(0);
    s_ready = true;
    ESP_LOGI(TAG,
             "nvs load font=%s spacing=%d margin=%d foot_msk=0x%x aa=%d underline=%d shelf_view=%d "
             "shelf_pct=%d orient=%d",
             s_font_file, s_spacing_preset, s_margin_preset, s_footer_mask, s_aa, s_underline,
             s_shelf_view, s_shelf_pct, s_orient);
}

enum class BookPrefsPersistKind : uint8_t {
    kFont = 0,
    kSpacing = 1,
    kMargin = 2,
    kFooterMask = 3,
    kAntialias = 4,
    kUnderline = 5,
    kShelfView = 6,
    kShelfPct = 7,
    kOrient = 8,
};

struct BookPrefsPersistArg {
    BookPrefsPersistKind kind = BookPrefsPersistKind::kFont;
    char file[kMaxFileLen + 1] = {};
    int ival = 0;
};

// 异步落盘：LVGL 栈常在 PSRAM，禁止在此线程直接 nvs_*（对齐 397 PersistTask）
static void PersistTask(void* arg) {
    auto* p = static_cast<BookPrefsPersistArg*>(arg);
    if (p == nullptr) {
        vTaskDelete(nullptr);
        return;
    }
    {
        Settings settings(kNvsNs, true);
        switch (p->kind) {
            case BookPrefsPersistKind::kFont:
                settings.SetString(kNvsFontKey, p->file);
                ESP_LOGI(TAG, "nvs save font=%s", p->file);
                break;
            case BookPrefsPersistKind::kSpacing:
                settings.SetInt(kNvsSpacingKey, p->ival);
                ESP_LOGI(TAG, "nvs save spacing=%d", p->ival);
                break;
            case BookPrefsPersistKind::kMargin:
                settings.SetInt(kNvsMarginKey, p->ival);
                ESP_LOGI(TAG, "nvs save margin=%d", p->ival);
                break;
            case BookPrefsPersistKind::kFooterMask:
                settings.SetInt(kNvsFooterMaskKey, p->ival);
                // 兼容旧键：无位图即隐藏
                settings.SetInt(kNvsHideProgKey, p->ival == 0 ? 1 : 0);
                ESP_LOGI(TAG, "nvs save foot_msk=0x%x", p->ival);
                break;
            case BookPrefsPersistKind::kAntialias:
                settings.SetInt(kNvsAaKey, p->ival);
                ESP_LOGI(TAG, "nvs save aa=%d", p->ival);
                break;
            case BookPrefsPersistKind::kUnderline:
                settings.SetInt(kNvsUnderlineKey, p->ival);
                ESP_LOGI(TAG, "nvs save underline=%d", p->ival);
                break;
            case BookPrefsPersistKind::kShelfView:
                settings.SetInt(kNvsShelfViewKey, p->ival);
                ESP_LOGI(TAG, "nvs save shelf_view=%d", p->ival);
                break;
            case BookPrefsPersistKind::kShelfPct:
                settings.SetInt(kNvsShelfPctKey, p->ival);
                ESP_LOGI(TAG, "nvs save shelf_pct=%d", p->ival);
                break;
            case BookPrefsPersistKind::kOrient:
                settings.SetInt(kNvsOrientKey, p->ival);
                ESP_LOGI(TAG, "nvs save orient=%d", p->ival);
                break;
        }
    }
    delete p;
    vTaskDelete(nullptr);
}

static bool StartPersist(BookPrefsPersistArg* arg) {
    if (arg == nullptr) {
        return false;
    }
    // 对齐 397：普通 xTaskCreate，栈可走 PSRAM（ALLOW_STACK_EXTERNAL）。
    // 勿 WithCaps(INTERNAL)：S31 内部堆常 <4KB，建任务失败且偏好落不了盘。
    if (xTaskCreate(PersistTask, "book_prefs_nvs", 4096, arg, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(book_prefs_nvs) failed");
        delete arg;
        return false;
    }
    return true;
}


void BookReaderPrefsEnsureLoaded(void) {
    if (!s_ready) {
        LoadFromNvs();
    }
}

const char* BookReaderPrefsFontFile(void) {
    BookReaderPrefsEnsureLoaded();
    return s_font_file[0] != '\0' ? s_font_file : kDefaultFile;
}

void BookReaderPrefsSetFontFile(const char* filename) {
    if (!IsSafeBasename(filename)) {
        ESP_LOGW(TAG, "reject bad font name");
        return;
    }
    BookReaderPrefsEnsureLoaded();
    std::snprintf(s_font_file, sizeof(s_font_file), "%s", filename);
    s_ready = true;

    auto* arg = new BookPrefsPersistArg{};
    arg->kind = BookPrefsPersistKind::kFont;
    std::snprintf(arg->file, sizeof(arg->file), "%s", s_font_file);
    StartPersist(arg);
}

char* BookReaderPrefsFontFullPath(char* out, size_t out_len) {
    if (out == nullptr || out_len < 16) {
        return nullptr;
    }
    const char* file = BookReaderPrefsFontFile();
    const int n = std::snprintf(out, out_len, "%s/%s", SD_PATH_FONTS, file);
    if (n <= 0 || static_cast<size_t>(n) >= out_len) {
        return nullptr;
    }
    return out;
}

int BookReaderPrefsSpacingPreset(void) {
    BookReaderPrefsEnsureLoaded();
    return ClampSpacing(s_spacing_preset);
}

void BookReaderPrefsSetSpacingPreset(int preset) {
    if (preset < 0 || preset >= kBookReaderSpacingPresetCount) {
        ESP_LOGW(TAG, "reject bad spacing preset=%d", preset);
        return;
    }
    BookReaderPrefsEnsureLoaded();
    s_spacing_preset = preset;
    s_ready = true;
    auto* arg = new BookPrefsPersistArg{};
    arg->kind = BookPrefsPersistKind::kSpacing;
    arg->ival = s_spacing_preset;
    StartPersist(arg);
}

const char* BookReaderPrefsSpacingLabel(int preset) {
    switch (ClampSpacing(preset)) {
        case 0:
            return Lang::Strings::BOOK_SPACING_COMPACT;
        case 1:
            return Lang::Strings::BOOK_SPACING_STANDARD;
        case 2:
            return Lang::Strings::BOOK_SPACING_RELAXED;
        case 3:
            return Lang::Strings::BOOK_SPACING_VERY_RELAXED;
        default:
            return Lang::Strings::BOOK_SPACING_STANDARD;
    }
}

void BookReaderPrefsSpacingGaps(int preset, int* line_gap, int* para_gap) {
    const BookSpacingPreset& sp = kSpacingTable[ClampSpacing(preset)];
    if (line_gap != nullptr) {
        *line_gap = sp.line_gap;
    }
    if (para_gap != nullptr) {
        *para_gap = sp.para_gap;
    }
}

int BookReaderPrefsLineGap(void) {
    int line = kSpacingTable[kDefaultSpacingPreset].line_gap;
    BookReaderPrefsSpacingGaps(BookReaderPrefsSpacingPreset(), &line, nullptr);
    return line;
}

int BookReaderPrefsParaGap(void) {
    int para = kSpacingTable[kDefaultSpacingPreset].para_gap;
    BookReaderPrefsSpacingGaps(BookReaderPrefsSpacingPreset(), nullptr, &para);
    return para;
}

int BookReaderPrefsMarginPreset(void) {
    BookReaderPrefsEnsureLoaded();
    return ClampMargin(s_margin_preset);
}

void BookReaderPrefsSetMarginPreset(int preset) {
    if (preset < 0 || preset >= kBookReaderMarginPresetCount) {
        ESP_LOGW(TAG, "reject bad margin preset=%d", preset);
        return;
    }
    BookReaderPrefsEnsureLoaded();
    s_margin_preset = preset;
    s_ready = true;
    auto* arg = new BookPrefsPersistArg{};
    arg->kind = BookPrefsPersistKind::kMargin;
    arg->ival = s_margin_preset;
    StartPersist(arg);
}

const char* BookReaderPrefsMarginLabel(int preset) {
    switch (ClampMargin(preset)) {
        case 0:
            return Lang::Strings::BOOK_MARGIN_NARROW;
        case 1:
            return Lang::Strings::BOOK_MARGIN_STANDARD;
        case 2:
            return Lang::Strings::BOOK_MARGIN_WIDE;
        case 3:
            return Lang::Strings::BOOK_MARGIN_VERY_WIDE;
        default:
            return Lang::Strings::BOOK_MARGIN_STANDARD;
    }
}

void BookReaderPrefsMarginBox(int preset, int* left, int* right, int* top, int* bottom) {
    const BookMarginPreset& m = kMarginTable[ClampMargin(preset)];
    if (left != nullptr) {
        *left = m.left;
    }
    if (right != nullptr) {
        *right = m.right;
    }
    if (top != nullptr) {
        *top = m.top;
    }
    if (bottom != nullptr) {
        *bottom = m.bottom;
    }
}

int BookReaderPrefsMarginLeft(void) {
    int v = kMarginTable[kDefaultMarginPreset].left;
    BookReaderPrefsMarginBox(BookReaderPrefsMarginPreset(), &v, nullptr, nullptr, nullptr);
    return v;
}

int BookReaderPrefsMarginRight(void) {
    int v = kMarginTable[kDefaultMarginPreset].right;
    BookReaderPrefsMarginBox(BookReaderPrefsMarginPreset(), nullptr, &v, nullptr, nullptr);
    return v;
}

int BookReaderPrefsMarginTop(void) {
    int v = kMarginTable[kDefaultMarginPreset].top;
    BookReaderPrefsMarginBox(BookReaderPrefsMarginPreset(), nullptr, nullptr, &v, nullptr);
    return v;
}

int BookReaderPrefsMarginBottom(void) {
    int v = kMarginTable[kDefaultMarginPreset].bottom;
    BookReaderPrefsMarginBox(BookReaderPrefsMarginPreset(), nullptr, nullptr, nullptr, &v);
    return v;
}

int BookReaderPrefsHideProgress(void) {
    return BookReaderPrefsFooterMask() == 0 ? 1 : 0;
}

void BookReaderPrefsSetHideProgress(int hide) {
    if (hide != 0) {
        BookReaderPrefsSetFooterMask(0);
        return;
    }
    if (BookReaderPrefsFooterMask() == 0) {
        BookReaderPrefsSetFooterMask(kDefaultFooterMask);
    }
}

int BookReaderPrefsFooterMask(void) {
    BookReaderPrefsEnsureLoaded();
    return ClampFooterMask(s_footer_mask);
}

void BookReaderPrefsSetFooterMask(int mask) {
    BookReaderPrefsEnsureLoaded();
    s_footer_mask = ClampFooterMask(mask);
    s_ready = true;
    auto* arg = new BookPrefsPersistArg{};
    arg->kind = BookPrefsPersistKind::kFooterMask;
    arg->ival = s_footer_mask;
    StartPersist(arg);
}

int BookReaderPrefsToggleFooterBit(int bit) {
    const int b = bit & kBookReaderFooterAll;
    if (b == 0) {
        return BookReaderPrefsFooterMask();
    }
    const int next = BookReaderPrefsFooterMask() ^ b;
    BookReaderPrefsSetFooterMask(next);
    return next;
}

int BookReaderPrefsAntialias(void) {
    BookReaderPrefsEnsureLoaded();
    return s_aa != 0 ? 1 : 0;
}

void BookReaderPrefsSetAntialias(int enabled) {
    BookReaderPrefsEnsureLoaded();
    s_aa = enabled != 0 ? 1 : 0;
    s_ready = true;
    auto* arg = new BookPrefsPersistArg{};
    arg->kind = BookPrefsPersistKind::kAntialias;
    arg->ival = s_aa;
    StartPersist(arg);
}

int BookReaderPrefsSyncGlyphAa(int immersive_reading) {
    (void)immersive_reading;
    const int prev = epd_i1_glyph_aa_enabled != 0 ? 1 : 0;
    // 470：阅读抗锯齿展示关闭（非白即黑）；保留 NVS/API 以免旧键迁移再开
    const int on = 0;
    epd_i1_glyph_set_aa(on);
    if (prev != on) {
        ESP_LOGI(TAG, "glyph aa=%d (%s)", on, on ? "gray4" : "bw");
    }
    return prev != on ? 1 : 0;
}

int BookReaderPrefsUnderlineMode(void) {
    BookReaderPrefsEnsureLoaded();
    return ClampUnderline(s_underline);
}

void BookReaderPrefsSetUnderlineMode(int mode) {
    if (mode < 0 || mode >= kBookReaderUnderlineModeCount) {
        ESP_LOGW(TAG, "reject bad underline mode=%d", mode);
        return;
    }
    BookReaderPrefsEnsureLoaded();
    s_underline = mode;
    s_ready = true;
    auto* arg = new BookPrefsPersistArg{};
    arg->kind = BookPrefsPersistKind::kUnderline;
    arg->ival = s_underline;
    StartPersist(arg);
}

int BookReaderPrefsShelfView(void) {
    BookReaderPrefsEnsureLoaded();
    return ClampShelfView(s_shelf_view);
}

void BookReaderPrefsSetShelfView(int view) {
    BookReaderPrefsEnsureLoaded();
    s_shelf_view = ClampShelfView(view);
    s_ready = true;
    auto* arg = new BookPrefsPersistArg{};
    arg->kind = BookPrefsPersistKind::kShelfView;
    arg->ival = s_shelf_view;
    StartPersist(arg);
}

int BookReaderPrefsShowShelfProgress(void) {
    BookReaderPrefsEnsureLoaded();
    return s_shelf_pct != 0 ? 1 : 0;
}

int BookReaderPrefsOrient(void) {
    BookReaderPrefsEnsureLoaded();
    return ClampOrient(s_orient);
}

void BookReaderPrefsSetOrient(int orient) {
    if (orient < 0 || orient >= kBookReaderOrientCount) {
        ESP_LOGW(TAG, "reject bad orient=%d", orient);
        return;
    }
    BookReaderPrefsEnsureLoaded();
    s_orient = orient;
    s_ready = true;
    auto* arg = new BookPrefsPersistArg{};
    arg->kind = BookPrefsPersistKind::kOrient;
    arg->ival = s_orient;
    StartPersist(arg);
}

void BookReaderPrefsSetShowShelfProgress(int enabled) {
    BookReaderPrefsEnsureLoaded();
    s_shelf_pct = enabled != 0 ? 1 : 0;
    s_ready = true;
    auto* arg = new BookPrefsPersistArg{};
    arg->kind = BookPrefsPersistKind::kShelfPct;
    arg->ival = s_shelf_pct;
    StartPersist(arg);
}
