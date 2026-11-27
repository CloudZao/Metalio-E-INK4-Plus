#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <strings.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "infra/storage/sd_paths.h"
#include "infra/storage/settings.h"
#include "business/reader/book_reader_prefs.h"
#include "sysmgmt/tasks/task_executor.h"

static constexpr const char* TAG = "BookPrefs";
static constexpr const char* kNvsNs = "reader";
static constexpr const char* kNvsFontKey = "font";
static constexpr const char* kNvsFontSizeKey = "font_size";
static constexpr const char* kNvsSpacingKey = "spacing";
static constexpr const char* kNvsParaGapKey = "para_gap";
static constexpr const char* kNvsMarginKey = "margin";
static constexpr const char* kNvsHideProgKey = "hide_prog";
static constexpr const char* kNvsFooterMaskKey = "foot_msk";
static constexpr const char* kNvsFooterLayoutKey = "foot_layout";
static constexpr const char* kNvsAaKey = "aa";
static constexpr const char* kNvsUnderlineKey = "underline";
static constexpr const char* kNvsShelfViewKey = "shelf_view";
static constexpr const char* kNvsShelfPctKey = "shelf_pct";
static constexpr const char* kNvsOrientKey = "orient";
static constexpr const char* kNvsTextDirectionKey = "text_dir";
static constexpr const char* kNvsTextAlignKey = "text_align";
static constexpr size_t kMaxFileLen = SD_FILE_NAME_MAX_BYTES;
static constexpr int kDefaultSpacingPreset = 1;  // 标准
static constexpr int kDefaultParaGapPreset = 1;  // 标准
static constexpr int kDefaultMarginPreset = 1;   // 标准
static constexpr int kDefaultFooterMask = kBookReaderFooterDefault;
static constexpr int kDefaultShelfView = kBookReaderShelfViewCover;
static constexpr int kDefaultShelfPct = 0;  // 进度角标默认关

struct BookMarginPreset {
    int left;    // 左边距，单位像素
    int right;   // 右边距，单位像素
    int top;     // 上边距，单位像素
    int bottom;  // 下边距，单位像素
};

// 成套页边距：与分页 viewport 共用，避免只改 UI pad 导致裁切/留白不一致
static constexpr BookMarginPreset kMarginTable[kBookReaderMarginPresetCount] = {
    {16, 16, 16, 12},
    BOARD_READER_MARGIN_DEFAULT,
    {40, 40, 36, 24},
    {56, 56, 48, 32},
};

static_assert(kMarginTable[1].left >= BOARD_READER_MARGIN_MIN && kMarginTable[1].left <= BOARD_READER_MARGIN_MAX &&
              kMarginTable[1].right >= BOARD_READER_MARGIN_MIN && kMarginTable[1].right <= BOARD_READER_MARGIN_MAX &&
              kMarginTable[1].top >= BOARD_READER_MARGIN_MIN && kMarginTable[1].top <= BOARD_READER_MARGIN_MAX &&
              kMarginTable[1].bottom >= BOARD_READER_MARGIN_MIN && kMarginTable[1].bottom <= BOARD_READER_MARGIN_MAX);

static char s_font_file[kMaxFileLen + 1] = {};
static int s_font_size = 0;  // .ef 为 0；TTF/OTF 为实际像素字号
static int s_spacing_preset = kDefaultSpacingPreset;
static int s_para_gap_preset = kDefaultParaGapPreset;
static int s_margin_preset = kDefaultMarginPreset;
static int s_footer_mask = kDefaultFooterMask;
static int s_footer_layout = kBookReaderFooterBottom;
static int s_aa = BOARD_READER_AA_DEFAULT;
static int s_underline = 0;
static std::atomic<int> s_background{0}; // 壁纸能力可在后台提交阅读背景选择
static int s_shake_turn = 0;
static int s_shake_level = 3;
static int s_auto_turn_seconds = BOARD_READER_AUTO_TURN_SECONDS;
static int s_writing_mode = kBookReaderWritingHorizontal;
static int s_text_align = kBookReaderTextAlignJustify;
static int s_inline_images = 1;
static int s_text_direction = kBookReaderTextLeftToRight;
static int s_page_turn = BOARD_READER_PAGE_TURN_DEFAULT;
static int s_shelf_view = kDefaultShelfView;
static int s_shelf_pct = kDefaultShelfPct;
static int s_orient = kBookReaderOrientPortrait;
static std::atomic<bool> s_ready{false};
static constexpr int kTextOptionCount = static_cast<int>(BookTextOption::Count);
static constexpr const char* kTextKeys[] = {"para_px",       "letter_px",   "line_px",     "margin_px",
                                            "indent_ch",     "ul_offset",   "aa_strength", "margin_top",
                                            "margin_bottom", "margin_left", "margin_right"};
static constexpr int kTextDefaults[] = {-1, 0, -1, -1, 2, 0, BOARD_READER_AA_STRENGTH_DEFAULT, -1, -1, -1, -1};
static constexpr int kTextMin[] = {BOARD_READER_PARA_GAP_MIN,
                                   -2,
                                   BOARD_READER_LINE_GAP_MIN,
                                   BOARD_READER_MARGIN_MIN,
                                   0,
                                   -12,
                                   0,
                                   BOARD_READER_MARGIN_MIN,
                                   BOARD_READER_MARGIN_MIN,
                                   BOARD_READER_MARGIN_MIN,
                                   BOARD_READER_MARGIN_MIN};
static constexpr int kTextMax[] = {BOARD_READER_PARA_GAP_MAX,
                                   12,
                                   BOARD_READER_LINE_GAP_MAX,
                                   BOARD_READER_MARGIN_MAX,
                                   3,
                                   12,
                                   2,
                                   BOARD_READER_MARGIN_MAX,
                                   BOARD_READER_MARGIN_MAX,
                                   BOARD_READER_MARGIN_MAX,
                                   BOARD_READER_MARGIN_MAX};
static int s_text_values[kTextOptionCount] = {-1, 0, -1, -1, 2, 0, BOARD_READER_AA_STRENGTH_DEFAULT, -1, -1, -1, -1};

static bool IsSafeBasename(const char* name) {
    return SdIsValidFileName(name);
}

static bool IsTtfName(const char* name) {
    const char* extension = std::strrchr(name, '.');
    return extension && (strcasecmp(extension, ".ttf") == 0 || strcasecmp(extension, ".otf") == 0);
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

static int ClampParaGap(int preset) {
    if (preset < 0 || preset >= kBookReaderParaGapPresetCount) {
        return kDefaultParaGapPreset;
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
    if (orient < kBookReaderOrientAuto || orient >= kBookReaderOrientCount) {
        return kBookReaderOrientPortrait;
    }
    return orient;
}

static void LoadFromNvs() {
    Settings settings(kNvsNs, false);
    const std::string v = settings.GetString(kNvsFontKey, kBookReaderDefaultFontFile);
    if (IsSafeBasename(v.c_str())) {
        std::snprintf(s_font_file, sizeof(s_font_file), "%s", v.c_str());
    } else {
        std::snprintf(s_font_file, sizeof(s_font_file), "%s", kBookReaderDefaultFontFile);
    }
    s_font_size = IsTtfName(s_font_file)
                      ? std::clamp<int>(settings.GetInt(kNvsFontSizeKey, BOARD_READER_FONT_SIZE),
                                        BOARD_READER_FONT_MIN, BOARD_READER_FONT_MAX)
                      : 0;
    s_spacing_preset = ClampSpacing(settings.GetInt(kNvsSpacingKey, kDefaultSpacingPreset));
    s_para_gap_preset = ClampParaGap(settings.GetInt(kNvsParaGapKey, kDefaultParaGapPreset));
    s_margin_preset = ClampMargin(settings.GetInt(kNvsMarginKey, kDefaultMarginPreset));
    // foot_msk 优先；无则按旧 hide_prog 迁移
    const int raw_mask = settings.GetInt(kNvsFooterMaskKey, -1);
    if (raw_mask >= 0) {
        s_footer_mask = ClampFooterMask(raw_mask);
    } else {
        const int hide = settings.GetInt(kNvsHideProgKey, 0) != 0 ? 1 : 0;
        s_footer_mask = hide ? 0 : kDefaultFooterMask;
    }
    s_footer_layout = settings.GetInt(kNvsFooterLayoutKey, kBookReaderFooterBottom) == kBookReaderFooterSplit
        ? kBookReaderFooterSplit : kBookReaderFooterBottom;
    s_aa = settings.GetInt(kNvsAaKey, BOARD_READER_AA_DEFAULT) != 0 ? 1 : 0;
    s_underline = ClampUnderline(settings.GetInt(kNvsUnderlineKey, 0));
    s_shake_turn = settings.GetInt("shake_turn", 0) != 0;
    const int shake_level = settings.GetInt("shake_level5", -1);
    if (shake_level >= 0) {
        s_shake_level = std::clamp(shake_level, 1, 5);
    } else {
        constexpr int kLegacyDefaultLevel = 7; // 旧默认档位对应新默认第 3 档
        constexpr int kLegacyMaxLevel = 10; // 旧最高灵敏度对应新第 5 档
        const int old_level = std::clamp<int>(settings.GetInt("shake_level", kLegacyDefaultLevel), 1, kLegacyMaxLevel);
        // 保留主分支的分段迁移：旧 1–4→1、5–6→2、7→3、8–9→4、10→5。
        const int mapped = old_level <= kLegacyDefaultLevel ? (old_level - 1) / 2 : (old_level * 2 - 4) / 3;
        s_shake_level = std::clamp(mapped, 1, 5);
    }
    s_auto_turn_seconds = std::clamp<int>(settings.GetInt("auto_turn_s", BOARD_READER_AUTO_TURN_SECONDS),
                                          BOARD_READER_AUTO_TURN_MIN_SECONDS, BOARD_READER_AUTO_TURN_MAX_SECONDS);
    s_writing_mode = settings.GetInt("writing_mode", 0) == 1 ? 1 : 0;
    s_text_align = settings.GetInt(kNvsTextAlignKey, kBookReaderTextAlignJustify) == kBookReaderTextAlignLeft
                       ? kBookReaderTextAlignLeft : kBookReaderTextAlignJustify;
    s_inline_images = settings.GetInt("inline_images", 1) != 0;
    s_text_direction = settings.GetInt(kNvsTextDirectionKey, kBookReaderTextLeftToRight) == kBookReaderTextRightToLeft
                           ? kBookReaderTextRightToLeft
                           : kBookReaderTextLeftToRight;
    s_background = settings.GetInt("background", 0);
    if (s_background < 0 || s_background > 3) {
        s_background = 0;
    }
    s_page_turn = settings.GetInt("page_turn", BOARD_READER_PAGE_TURN_DEFAULT);
    if (!BOARD_SUPPORTS_PAGE_WIPE) {
        s_page_turn = BOARD_READER_PAGE_TURN_DEFAULT;
    }
    if (s_page_turn != kBookReaderPageTurnSlide && s_page_turn != kBookReaderPageTurnWipe) {
        s_page_turn = BOARD_READER_PAGE_TURN_DEFAULT;
    }
    s_shelf_view = ClampShelfView(settings.GetInt(kNvsShelfViewKey, kDefaultShelfView));
    s_shelf_pct = settings.GetInt(kNvsShelfPctKey, kDefaultShelfPct) != 0 ? 1 : 0;
    s_orient = ClampOrient(settings.GetInt(kNvsOrientKey, kBookReaderOrientPortrait));
    for (int i = 0; i < kTextOptionCount; ++i) {
        const int value = settings.GetInt(kTextKeys[i], kTextDefaults[i]);
        s_text_values[i] = value == kTextDefaults[i] ? value : std::clamp(value, kTextMin[i], kTextMax[i]);
    }
    s_ready = true;
    ESP_LOGI(TAG,
             "nvs load spacing=%d margin=%d foot_msk=0x%x aa=%d underline=%d shelf_view=%d "
             "shelf_pct=%d orient=%d",
             s_spacing_preset, s_margin_preset, s_footer_mask, s_aa, s_underline, s_shelf_view, s_shelf_pct, s_orient);
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
    kBackground = 9,
    kPageTurn = 10,
    kWritingMode = 11,
    kAutoTurnSeconds = 12,
    kTextDirection = 13,
    kParaGap = 14,
    kTextValues = 15,
    kShakeTurn = 16,
    kShakeLevel = 17,
    kInlineImages = 18,
    kFooterLayout = 19,
    kTextAlign = 20,
    kCount,
};

struct BookPrefsPersistArg {
    BookPrefsPersistKind kind = BookPrefsPersistKind::kFont;
    char file[kMaxFileLen + 1] = {};
    int ival = 0;
    int values[kTextOptionCount] = {};
};

static BookPrefsPersistArg* NewPersistArg(BookPrefsPersistKind kind) {
    auto* arg = new (std::nothrow) BookPrefsPersistArg;
    if (arg) {
        arg->kind = kind;
    } else {
        ESP_LOGE(TAG, "preference allocation failed kind=%u", static_cast<unsigned>(kind));
    }
    return arg;
}

static void PersistOne(BookPrefsPersistArg* p) {
    {
        Settings settings(kNvsNs, true);
        switch (p->kind) {
            case BookPrefsPersistKind::kTextValues:
                for (int i = 0; i < kTextOptionCount; ++i) {
                    settings.SetInt(kTextKeys[i], p->values[i]);
                }
                break;
            case BookPrefsPersistKind::kFont:
                settings.SetString(kNvsFontKey, p->file);
                settings.SetInt(kNvsFontSizeKey, p->ival);
                break;
            case BookPrefsPersistKind::kSpacing:
                settings.SetInt(kNvsSpacingKey, p->ival);
                break;
            case BookPrefsPersistKind::kParaGap:
                settings.SetInt(kNvsParaGapKey, p->ival);
                break;
            case BookPrefsPersistKind::kMargin:
                settings.SetInt(kNvsMarginKey, p->ival);
                break;
            case BookPrefsPersistKind::kFooterMask:
                settings.SetInt(kNvsFooterMaskKey, p->ival);
                // 兼容旧键：无位图即隐藏
                settings.SetInt(kNvsHideProgKey, p->ival == 0 ? 1 : 0);
                break;
            case BookPrefsPersistKind::kFooterLayout:
                settings.SetInt(kNvsFooterLayoutKey, p->ival);
                break;
            case BookPrefsPersistKind::kAntialias:
                settings.SetInt(kNvsAaKey, p->ival);
                break;
            case BookPrefsPersistKind::kAutoTurnSeconds:
                settings.SetInt("auto_turn_s", p->ival);
                break;
            case BookPrefsPersistKind::kShakeTurn:
                settings.SetInt("shake_turn", p->ival);
                break;
            case BookPrefsPersistKind::kShakeLevel:
                settings.SetInt("shake_level5", p->ival);
                break;
            case BookPrefsPersistKind::kWritingMode:
                settings.SetInt("writing_mode", p->ival);
                break;
            case BookPrefsPersistKind::kInlineImages:
                settings.SetInt("inline_images", p->ival);
                break;
            case BookPrefsPersistKind::kTextDirection:
                settings.SetInt(kNvsTextDirectionKey, p->ival);
                break;
            case BookPrefsPersistKind::kTextAlign:
                settings.SetInt(kNvsTextAlignKey, p->ival);
                break;
            case BookPrefsPersistKind::kPageTurn:
                settings.SetInt("page_turn", p->ival);
                break;
            case BookPrefsPersistKind::kBackground:
                settings.SetInt("background", p->ival);
                break;
            case BookPrefsPersistKind::kUnderline:
                settings.SetInt(kNvsUnderlineKey, p->ival);
                break;
            case BookPrefsPersistKind::kShelfView:
                settings.SetInt(kNvsShelfViewKey, p->ival);
                break;
            case BookPrefsPersistKind::kShelfPct:
                settings.SetInt(kNvsShelfPctKey, p->ival);
                break;
            case BookPrefsPersistKind::kOrient:
                settings.SetInt(kNvsOrientKey, p->ival);
                break;
            case BookPrefsPersistKind::kCount:
                break;
        }
        if (settings.Commit()) {
            ESP_LOGI(TAG, "operation=save_reader_preference kind=%u result=ok", static_cast<unsigned>(p->kind));
        } else {
            ESP_LOGE(TAG, "operation=save_reader_preference kind=%u result=storage_error; RAM retained",
                     static_cast<unsigned>(p->kind));
        }
    }
}

static std::mutex s_persist_mutex;  // 串行交接，避免旧快照晚于新快照写入
static BookPrefsPersistArg* s_pending[static_cast<unsigned>(BookPrefsPersistKind::kCount)] =
    {};                                 // 每类偏好仅保留最新快照
static bool s_persist_running = false;  // 无待写参数时退出任务

static void PersistTask(void*) {
    for (;;) {
        BookPrefsPersistArg* next = nullptr;
        {
            std::lock_guard<std::mutex> lock(s_persist_mutex);
            for (auto& pending : s_pending) {
                if (pending) {
                    next = pending;
                    pending = nullptr;
                    break;
                }
            }
            if (!next) {
                s_persist_running = false;
                break;
            }
        }
        PersistOne(next);
        delete next;
    }
}

static bool StartPersist(BookPrefsPersistArg* arg) {
    if (!arg) {
        ESP_LOGE(TAG, "preference allocation failed");
        return false;
    }
    std::lock_guard<std::mutex> lock(s_persist_mutex);
    const auto slot = static_cast<unsigned>(arg->kind);
    delete s_pending[slot];
    s_pending[slot] = arg;
    if (s_persist_running) {
        return true;
    }
    s_persist_running = true;
    if (TaskExecutor::Schedule([] { PersistTask(nullptr); }, TaskDomain::Storage)) {
        return true;
    }
    s_persist_running = false;
    ESP_LOGW(TAG, "operation=save_preferences stage=submit result=rejected; pending kept");
    return false;
}

static int MarginValue(BookTextOption option, int preset_value) {
    const int value = BookReaderPrefsTextValue(option);
    if (value >= 0) {
        return value;
    }
    const int legacy = BookReaderPrefsTextValue(BookTextOption::Margin);
    return std::clamp(legacy >= 0 ? legacy : preset_value, BOARD_READER_MARGIN_MIN, BOARD_READER_MARGIN_MAX);
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

static void SaveTextValues() {
    auto* arg = NewPersistArg(BookPrefsPersistKind::kTextValues);
    if (!arg) {
        return;
    }
    std::copy_n(s_text_values, kTextOptionCount, arg->values);
    StartPersist(arg);
}

void BookReaderPrefsEnsureLoaded(void) {
    if (!s_ready) {
        LoadFromNvs();
    }
}

const char* BookReaderPrefsFontFile(void) {
    BookReaderPrefsEnsureLoaded();
    return s_font_file[0] != '\0' ? s_font_file : kBookReaderDefaultFontFile;
}

void BookReaderPrefsSetFontFile(const char* filename, int size_px) {
    if (!IsSafeBasename(filename) || (IsTtfName(filename) && (size_px < BOARD_READER_FONT_MIN ||
                                                              size_px > BOARD_READER_FONT_MAX))) {
        ESP_LOGW(TAG, "reject bad font name");
        return;
    }
    BookReaderPrefsEnsureLoaded();
    std::snprintf(s_font_file, sizeof(s_font_file), "%s", filename);
    s_font_size = IsTtfName(filename) ? size_px : 0;
    s_ready = true;

    auto* arg = NewPersistArg(BookPrefsPersistKind::kFont);
    if (!arg) {
        return;
    }
    std::snprintf(arg->file, sizeof(arg->file), "%s", s_font_file);
    arg->ival = s_font_size;
    StartPersist(arg);
}

int BookReaderPrefsFontSize(void) {
    BookReaderPrefsEnsureLoaded();
    return s_font_size;
}

char* BookReaderPrefsFontFullPath(char* out, size_t out_len) {
    if (out == nullptr || out_len < 16) {
        return nullptr;
    }
    const char* file = BookReaderPrefsFontFile();
    const int n = std::snprintf(out, out_len, "%s/%s", s_font_size > 0 ? SD_PATH_FONT_SRC : SD_PATH_FONTS, file);
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
    auto* arg = NewPersistArg(BookPrefsPersistKind::kSpacing);
    if (!arg) {
        return;
    }
    arg->ival = s_spacing_preset;
    StartPersist(arg);
}

int BookReaderPrefsLineSpacingPercent(void) {
    return kBookReaderLineSpacingPercent[BookReaderPrefsSpacingPreset()];
}

int BookReaderPrefsParaGapPreset(void) {
    BookReaderPrefsEnsureLoaded();
    return ClampParaGap(s_para_gap_preset);
}

void BookReaderPrefsSetParaGapPreset(int preset) {
    if (preset < 0 || preset >= kBookReaderParaGapPresetCount) {
        ESP_LOGW(TAG, "reject bad paragraph gap preset=%d", preset);
        return;
    }
    BookReaderPrefsEnsureLoaded();
    if (s_para_gap_preset == preset) {
        return;
    }
    s_para_gap_preset = preset;
    auto* arg = NewPersistArg(BookPrefsPersistKind::kParaGap);
    if (!arg) {
        return;
    }
    arg->ival = s_para_gap_preset;
    StartPersist(arg);
}

int BookReaderPrefsParaGap(void) {
    const int custom = BookReaderPrefsTextValue(BookTextOption::ParaGap);
    return std::clamp(custom >= 0 ? custom : kBookReaderParagraphGapPx[BookReaderPrefsParaGapPreset()],
                      BOARD_READER_PARA_GAP_MIN, BOARD_READER_PARA_GAP_MAX);
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
    auto* arg = NewPersistArg(BookPrefsPersistKind::kMargin);
    if (!arg) {
        return;
    }
    arg->ival = s_margin_preset;
    StartPersist(arg);
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

void BookReaderPrefsMarginBox(int preset, int* left, int* right, int* top, int* bottom) {
    const auto& margins = kMarginTable[ClampMargin(preset)];
    if (left != nullptr) {
        *left = MarginValue(BookTextOption::MarginLeft, margins.left);
    }
    if (right != nullptr) {
        *right = MarginValue(BookTextOption::MarginRight, margins.right);
    }
    if (top != nullptr) {
        *top = MarginValue(BookTextOption::MarginTop, margins.top);
    }
    if (bottom != nullptr) {
        *bottom = MarginValue(BookTextOption::MarginBottom, margins.bottom);
    }
}

int BookReaderPrefsHideProgress(void) {
    return BookReaderPrefsFooterMask() == 0 ? 1 : 0;
}

int BookReaderPrefsFooterMask(void) {
    BookReaderPrefsEnsureLoaded();
    return ClampFooterMask(s_footer_mask);
}

void BookReaderPrefsSetFooterMask(int mask) {
    BookReaderPrefsEnsureLoaded();
    s_footer_mask = ClampFooterMask(mask);
    s_ready = true;
    auto* arg = NewPersistArg(BookPrefsPersistKind::kFooterMask);
    if (!arg) {
        return;
    }
    arg->ival = s_footer_mask;
    StartPersist(arg);
}

int BookReaderPrefsFooterLayout(void) {
    BookReaderPrefsEnsureLoaded();
    return s_footer_layout;
}

void BookReaderPrefsSetFooterLayout(int layout) {
    if (layout != kBookReaderFooterBottom && layout != kBookReaderFooterSplit) {
        return;
    }
    BookReaderPrefsEnsureLoaded();
    s_footer_layout = layout;
    auto* arg = NewPersistArg(BookPrefsPersistKind::kFooterLayout);
    if (arg) {
        arg->ival = layout;
        StartPersist(arg);
    }
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
    auto* arg = NewPersistArg(BookPrefsPersistKind::kAntialias);
    if (!arg) {
        return;
    }
    arg->ival = s_aa;
    StartPersist(arg);
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
    auto* arg = NewPersistArg(BookPrefsPersistKind::kUnderline);
    if (!arg) {
        return;
    }
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
    auto* arg = NewPersistArg(BookPrefsPersistKind::kShelfView);
    if (!arg) {
        return;
    }
    arg->ival = s_shelf_view;
    StartPersist(arg);
}

int BookReaderPrefsOrient(void) {
    BookReaderPrefsEnsureLoaded();
    return ClampOrient(s_orient);
}

void BookReaderPrefsSetOrient(int orient) {
    if (orient < kBookReaderOrientAuto || orient >= kBookReaderOrientCount) {
        ESP_LOGW(TAG, "reject bad orient=%d", orient);
        return;
    }
    BookReaderPrefsEnsureLoaded();
    s_orient = orient;
    s_ready = true;
    auto* arg = NewPersistArg(BookPrefsPersistKind::kOrient);
    if (!arg) {
        return;
    }
    arg->ival = s_orient;
    StartPersist(arg);
}

int BookReaderPrefsShowShelfProgress(void) {
    BookReaderPrefsEnsureLoaded();
    return s_shelf_pct != 0 ? 1 : 0;
}

void BookReaderPrefsSetShowShelfProgress(int enabled) {
    BookReaderPrefsEnsureLoaded();
    s_shelf_pct = enabled != 0 ? 1 : 0;
    s_ready = true;
    auto* arg = NewPersistArg(BookPrefsPersistKind::kShelfPct);
    if (!arg) {
        return;
    }
    arg->ival = s_shelf_pct;
    StartPersist(arg);
}

int BookReaderPrefsBackground() {
    BookReaderPrefsEnsureLoaded();
    return s_background;
}

void BookReaderPrefsSetBackground(int mode) {
    if (mode < 0 || mode > 3) {
        return;
    }
    BookReaderPrefsEnsureLoaded();
    s_background = mode;
    auto* arg = NewPersistArg(BookPrefsPersistKind::kBackground);
    if (!arg) {
        return;
    }
    arg->ival = mode;
    StartPersist(arg);
}

int BookReaderPrefsPageTurn() {
    BookReaderPrefsEnsureLoaded();
    return s_page_turn;
}

void BookReaderPrefsSetPageTurn(int mode) {
    if (!BOARD_SUPPORTS_PAGE_WIPE) {
        mode = kBookReaderPageTurnSlide;
    }
    if (mode != kBookReaderPageTurnSlide && mode != kBookReaderPageTurnWipe) {
        return;
    }
    BookReaderPrefsEnsureLoaded();
    s_page_turn = mode;
    auto* arg = NewPersistArg(BookPrefsPersistKind::kPageTurn);
    if (!arg) {
        return;
    }
    arg->ival = mode;
    StartPersist(arg);
}

int BookReaderPrefsWritingMode() {
    BookReaderPrefsEnsureLoaded();
    return s_writing_mode;
}

void BookReaderPrefsSetWritingMode(int mode) {
    if (mode != kBookReaderWritingHorizontal && mode != kBookReaderWritingVertical) {
        return;
    }
    BookReaderPrefsEnsureLoaded();
    s_writing_mode = mode;
    auto* arg = NewPersistArg(BookPrefsPersistKind::kWritingMode);
    if (!arg) {
        return;
    }
    arg->ival = mode;
    StartPersist(arg);
}

int BookReaderPrefsTextAlign() {
    BookReaderPrefsEnsureLoaded();
    return s_text_align;
}

void BookReaderPrefsSetTextAlign(int align) {
    if (align != kBookReaderTextAlignLeft && align != kBookReaderTextAlignJustify) {
        return;
    }
    BookReaderPrefsEnsureLoaded();
    if (s_text_align == align) {
        return;
    }
    s_text_align = align;
    auto* arg = NewPersistArg(BookPrefsPersistKind::kTextAlign);
    if (!arg) {
        return;
    }
    arg->ival = align;
    StartPersist(arg);
}

int BookReaderPrefsTextDirection() {
    BookReaderPrefsEnsureLoaded();
    return s_text_direction;
}

void BookReaderPrefsSetTextDirection(int direction) {
    if (direction != kBookReaderTextLeftToRight && direction != kBookReaderTextRightToLeft) {
        return;
    }
    BookReaderPrefsEnsureLoaded();
    if (s_text_direction == direction) {
        return;
    }
    s_text_direction = direction;
    auto* arg = NewPersistArg(BookPrefsPersistKind::kTextDirection);
    if (!arg) {
        return;
    }
    arg->ival = direction;
    StartPersist(arg);
}

int BookReaderPrefsInlineImages() {
    BookReaderPrefsEnsureLoaded();
    return s_inline_images;
}

void BookReaderPrefsSetInlineImages(int value) {
    BookReaderPrefsEnsureLoaded();
    value = value != 0;
    if (s_inline_images == value) {
        return;
    }
    s_inline_images = value;
    auto* arg = NewPersistArg(BookPrefsPersistKind::kInlineImages);
    if (!arg) {
        return;
    }
    arg->ival = value;
    StartPersist(arg);
}

int BookReaderPrefsShakeTurn() {
    BookReaderPrefsEnsureLoaded();
    return s_shake_turn;
}

void BookReaderPrefsSetShakeTurn(int value) {
    BookReaderPrefsEnsureLoaded();
    value = value != 0;
    if (s_shake_turn == value) {
        return;
    }
    s_shake_turn = value;
    auto* arg = NewPersistArg(BookPrefsPersistKind::kShakeTurn);
    if (!arg) {
        return;
    }
    arg->ival = value;
    StartPersist(arg);
}

int BookReaderPrefsShakeLevel() {
    BookReaderPrefsEnsureLoaded();
    return s_shake_level;
}

void BookReaderPrefsSetShakeLevel(int value) {
    BookReaderPrefsEnsureLoaded();
    value = std::clamp(value, 1, 5);
    if (s_shake_level == value) {
        return;
    }
    s_shake_level = value;
    auto* arg = NewPersistArg(BookPrefsPersistKind::kShakeLevel);
    if (!arg) {
        return;
    }
    arg->ival = value;
    StartPersist(arg);
}

int BookReaderPrefsAutoTurnSeconds() {
    BookReaderPrefsEnsureLoaded();
    return s_auto_turn_seconds;
}

void BookReaderPrefsSetAutoTurnSeconds(int seconds) {
    BookReaderPrefsEnsureLoaded();
    seconds = std::clamp(seconds, BOARD_READER_AUTO_TURN_MIN_SECONDS, BOARD_READER_AUTO_TURN_MAX_SECONDS);
    if (s_auto_turn_seconds == seconds) {
        return;
    }
    s_auto_turn_seconds = seconds;
    auto* arg = NewPersistArg(BookPrefsPersistKind::kAutoTurnSeconds);
    if (!arg) {
        return;
    }
    arg->ival = seconds;
    StartPersist(arg);
}

int BookReaderPrefsTextValue(BookTextOption option) {
    BookReaderPrefsEnsureLoaded();
    const int index = static_cast<int>(option);
    return index >= 0 && index < kTextOptionCount ? s_text_values[index] : 0;
}

bool BookReaderPrefsSetTextValue(BookTextOption option, int value) {
    BookReaderPrefsEnsureLoaded();
    const int index = static_cast<int>(option);
    if (index < 0 || index >= kTextOptionCount || value < kTextMin[index] || value > kTextMax[index]) {
        return false;
    }
    if (s_text_values[index] == value) {
        return false;
    }
    s_text_values[index] = value;
    SaveTextValues();
    return true;
}

TextStyle BookReaderPrefsTextStyle() {
    BookReaderPrefsEnsureLoaded();
    TextStyle style;
    style.inline_images = s_inline_images != 0;
    style.justify = s_text_align == kBookReaderTextAlignJustify;
    style.line_gap = s_text_values[2];
    style.letter_gap = s_text_values[1];
    style.indent_chars = s_text_values[4];
    style.underline_offset = s_text_values[5];
    style.aa_strength = s_text_values[6];
    return style;
}

void BookReaderPrefsResetReading() {
    BookReaderPrefsEnsureLoaded();
    std::copy_n(kTextDefaults, kTextOptionCount, s_text_values);
    SaveTextValues();
    BookReaderPrefsSetSpacingPreset(kDefaultSpacingPreset);
    BookReaderPrefsSetParaGapPreset(kDefaultParaGapPreset);
    BookReaderPrefsSetMarginPreset(kDefaultMarginPreset);
    BookReaderPrefsSetFooterMask(kBookReaderFooterDefault);
    BookReaderPrefsSetFooterLayout(kBookReaderFooterBottom);
    BookReaderPrefsSetAntialias(BOARD_READER_AA_DEFAULT);
    BookReaderPrefsSetUnderlineMode(0);
    BookReaderPrefsSetOrient(kBookReaderOrientPortrait);
    BookReaderPrefsSetBackground(0);
    BookReaderPrefsSetWritingMode(kBookReaderWritingHorizontal);
    BookReaderPrefsSetTextAlign(kBookReaderTextAlignJustify);
    BookReaderPrefsSetInlineImages(1);
    BookReaderPrefsSetTextDirection(kBookReaderTextLeftToRight);
    BookReaderPrefsSetPageTurn(BOARD_READER_PAGE_TURN_DEFAULT);
    BookReaderPrefsSetShakeTurn(0);
    BookReaderPrefsSetShakeLevel(3);
    BookReaderPrefsSetAutoTurnSeconds(BOARD_READER_AUTO_TURN_SECONDS);
}
