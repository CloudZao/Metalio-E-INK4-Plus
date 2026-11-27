#pragma once
#include <cstddef>
#include <cstdint>
#include "config.h"

static_assert(BOARD_READER_MARGIN_MIN >= 0 && BOARD_READER_MARGIN_MIN <= BOARD_READER_MARGIN_MAX);
static_assert(BOARD_READER_LINE_GAP_MIN >= 0 && BOARD_READER_LINE_GAP_MIN <= BOARD_READER_LINE_GAP_MAX);
static_assert(BOARD_READER_LINE_SPACING_PERCENT >= 0 && BOARD_READER_LINE_SPACING_PERCENT <= 100);
static_assert(BOARD_READER_PARA_GAP_MIN >= 0 && BOARD_READER_PARA_GAP_MIN <= BOARD_READER_PARA_GAP_DEFAULT &&
              BOARD_READER_PARA_GAP_DEFAULT <= BOARD_READER_PARA_GAP_MAX);
static_assert(BOARD_READER_AA_DEFAULT == 0 || BOARD_READER_AA_DEFAULT == 1);
static_assert(BOARD_READER_AA_STRENGTH_DEFAULT >= 0 && BOARD_READER_AA_STRENGTH_DEFAULT <= 2);
static_assert(BOARD_READER_AUTO_TURN_MIN_SECONDS > 0 &&
              BOARD_READER_AUTO_TURN_MIN_SECONDS <= BOARD_READER_AUTO_TURN_SECONDS &&
              BOARD_READER_AUTO_TURN_SECONDS <= BOARD_READER_AUTO_TURN_MAX_SECONDS);
static_assert(BOARD_READER_TOC_ROWS >= 2);
static_assert(BOARD_READER_FOOTER_MASK_DEFAULT >= 0 && BOARD_READER_FOOTER_MASK_DEFAULT <= 15);
static_assert(BOARD_READER_PAGE_TURN_DEFAULT == 0 || (BOARD_SUPPORTS_PAGE_WIPE && BOARD_READER_PAGE_TURN_DEFAULT == 2));

inline constexpr int kBookReaderLineSpacingPercent[] = {0, BOARD_READER_LINE_SPACING_PERCENT, 25,
                                                        40};  // 字体行框百分比，索引兼容NVS
inline constexpr int kBookReaderParagraphGapPx[] = {32, BOARD_READER_PARA_GAP_DEFAULT, 80,
                                                    BOARD_READER_PARA_GAP_MAX};  // 额外段距像素，索引兼容NVS

#ifdef __cplusplus
extern "C" {
#endif

// 阅读偏好（NVS namespace "reader"）：
// - font：SD fonts 目录下 .ef 或 fonts_ttf 目录下 TTF/OTF 文件名
// - font_size：直接加载 TTF/OTF 的像素字号；.ef 为 0
// - spacing：行距档位，独立于段距
// - para_gap：段距档位，保留持久化索引；标准档默认值由板级配置提供
// - margin / margin_px：旧成套边距及统一像素值，供未单独设置的边继承
// - margin_top / margin_bottom / margin_left / margin_right：四边独立像素值
// - foot_msk：正文底栏展示位图（0=整条隐藏；兼容旧 hide_prog）
// - foot_layout：信息布局，0 全部底部、1 上下分栏
// - aa：阅读真 4 灰抗锯齿（L8 sidecar + 灰 LUT）
// - underline：正文下划线 0关 / 1实线 / 2虚线
// - shelf_view：书架封面网格 / 文件夹列表
// - shelf_pct：书架/首页封面与列表是否显示进度角标（默认关）
// - orient：正文方向 -1自动 / 0竖屏 / 1左横 / 2右横 / 3倒置竖屏（仅阅读页；离开后恢复竖屏）
// - text_dir：竖排列序 0从左往右 / 1从右往左
// - text_align：正文对齐 0左对齐（竖排顶端对齐）/ 1两端对齐（默认）
// 读走 RAM 缓存；首次 Ensure 须在 DRAM 栈；落盘在内部 RAM 栈任务。

enum {
    kBookReaderSpacingPresetCount = sizeof(kBookReaderLineSpacingPercent) / sizeof(kBookReaderLineSpacingPercent[0]),
    kBookReaderParaGapPresetCount = sizeof(kBookReaderParagraphGapPx) / sizeof(kBookReaderParagraphGapPx[0]),
    kBookReaderMarginPresetCount = 4,
    kBookReaderUnderlineOff = 0,
    kBookReaderUnderlineSolid = 1,
    kBookReaderUnderlineDashed = 2,
    kBookReaderUnderlineModeCount = 3,
    kBookReaderFooterTitle = 1 << 0,     // 章节名称
    kBookReaderFooterProgress = 1 << 1,  // 百分比 + 页码
    kBookReaderFooterBattery = 1 << 2,   // 电量
    kBookReaderFooterTime = 1 << 3,      // 时间
    kBookReaderFooterAll = kBookReaderFooterTitle | kBookReaderFooterProgress | kBookReaderFooterBattery |
        kBookReaderFooterTime,
    kBookReaderFooterDefault = BOARD_READER_FOOTER_MASK_DEFAULT,
};

enum {
    kBookReaderFooterBottom = 0, // 信息全部置于底部
    kBookReaderFooterSplit = 1, // 章节与百分比在顶部，时间、电量与页数在底部
};

enum {
    kBookReaderShelfViewCover = 0,  // 封面网格
    kBookReaderShelfViewList = 1,   // 文件夹列表
};

enum {
    kBookReaderOrientAuto = -1,         // 根据重力方向自动旋转
    kBookReaderOrientPortrait = 0,      // 竖屏
    kBookReaderOrientLandLeft = 1,      // 左横
    kBookReaderOrientLandRight = 2,     // 右横
    kBookReaderOrientPortraitDown = 3,  // 倒置竖屏
    kBookReaderOrientCount = 4,         // 手动方向档位数
};

/** @brief 确保阅读偏好已从 NVS 加载 */
void BookReaderPrefsEnsureLoaded(void);

/** @brief 当前字体文件名（仅 basename）；未 Ensure 时返回默认值。永不返回 NULL。 */
const char* BookReaderPrefsFontFile(void);

/** @brief 设置正文字体并写入NVS；TTF/OTF字号受产品资源规格约束，.ef使用默认值零。 */
void BookReaderPrefsSetFontFile(const char* filename, int size_px = 0);

/** @brief 当前 TTF/OTF 的像素字号；.ef 返回 0。 */
int BookReaderPrefsFontSize(void);

/** @brief 正文字体完整路径 */
char* BookReaderPrefsFontFullPath(char* out, size_t out_len);

/** @brief 行距档位 */
int BookReaderPrefsSpacingPreset(void);

/** @brief 设置行距档位 */
void BookReaderPrefsSetSpacingPreset(int preset);



/** @brief 行间留白占字体行框的首选百分比，实际间距按内容区自适应。 */
int BookReaderPrefsLineSpacingPercent(void);

/** @brief 段距档位 */
int BookReaderPrefsParaGapPreset(void);

/** @brief 设置并保存段距档位 */
void BookReaderPrefsSetParaGapPreset(int preset);

/** @brief 普通行距之外的额外段距，单位像素，标准档默认值由板级配置提供。 */
int BookReaderPrefsParaGap(void);

/** @brief 页边距档位 0..N-1 */
int BookReaderPrefsMarginPreset(void);

/** @brief 设置边距档位 */
void BookReaderPrefsSetMarginPreset(int preset);



/** @brief 左边距 */
int BookReaderPrefsMarginLeft(void);

/** @brief 右边距 */
int BookReaderPrefsMarginRight(void);

/** @brief 上边距 */
int BookReaderPrefsMarginTop(void);

/** @brief 下边距 */
int BookReaderPrefsMarginBottom(void);

/** @brief 四边边距 */
void BookReaderPrefsMarginBox(int preset, int* left, int* right, int* top, int* bottom);

/** @brief true=底栏整条隐藏（foot_msk==0） */
int BookReaderPrefsHideProgress(void);

/** @brief 正文底栏展示位图（见 kBookReaderFooter*） */
int BookReaderPrefsFooterMask(void);

/** @brief 设置底栏信息位掩码 */
void BookReaderPrefsSetFooterMask(int mask);

/** @brief 获取阅读信息布局，默认全部底部。 */
int BookReaderPrefsFooterLayout(void);

/** @brief 设置阅读信息布局并异步保存。 */
void BookReaderPrefsSetFooterLayout(int layout);

/** @brief 切换单比特；返回最新 mask */
int BookReaderPrefsToggleFooterBit(int bit);

/** @brief 阅读抗锯齿偏好（默认关闭） */
int BookReaderPrefsAntialias(void);

/** @brief 设置并保存阅读抗锯齿开关 */
void BookReaderPrefsSetAntialias(int enabled);



/** @brief 正文下划线：kBookReaderUnderlineOff / Solid / Dashed */
int BookReaderPrefsUnderlineMode(void);

/** @brief 设置下划线模式 */
void BookReaderPrefsSetUnderlineMode(int mode);

/** @brief 书架展示：kBookReaderShelfViewCover / List */
int BookReaderPrefsShelfView(void);

/** @brief 设置书架视图模式 */
void BookReaderPrefsSetShelfView(int view);

/** @brief 正文方向模式，包含自动及手动方向 */
int BookReaderPrefsOrient(void);

/** @brief 设置正文方向 */
void BookReaderPrefsSetOrient(int orient);

/** @brief 1=显示书架/首页进度角标；默认 0 */
int BookReaderPrefsShowShelfProgress(void);

/** @brief 设置是否显示进度角标 */
void BookReaderPrefsSetShowShelfProgress(int enabled);

#ifdef __cplusplus
}
#endif

/** @brief 获取阅读背景：0 白底，1 方格，2 点阵，3 自定义壁纸。 */
int BookReaderPrefsBackground();

/** @brief 设置阅读背景模式并持久化。 */
void BookReaderPrefsSetBackground(int mode);

// 翻页方式沿用已保存的数值，默认保持原有行为。
enum {
    kBookReaderPageTurnSlide = 0,   // 左右滑动
    kBookReaderPageTurnRipple = 1,  // 水波纹
    kBookReaderPageTurnWipe = 2,    // 按翻页方向推白
};

/** @brief 获取已保存的正文翻页过渡方式。 */
int BookReaderPrefsPageTurn();

/** @brief 设置并保存正文翻页过渡方式。 */
void BookReaderPrefsSetPageTurn(int mode);

enum {
    kBookReaderWritingHorizontal = 0,  // 横排
    kBookReaderWritingVertical = 1,    // 竖排
};

/** @brief 正文横排或竖排模式，与屏幕旋转方向独立 */
int BookReaderPrefsWritingMode();

/** @brief 设置并保存正文排版模式 */
void BookReaderPrefsSetWritingMode(int mode);

enum {
    kBookReaderTextAlignLeft = 0, // 横排左对齐、竖排顶端对齐
    kBookReaderTextAlignJustify = 1, // 两端对齐
};

/** @brief 获取正文对齐方式，默认两端对齐。 */
int BookReaderPrefsTextAlign();

/** @brief 设置正文对齐方式并异步保存。 */
void BookReaderPrefsSetTextAlign(int align);

enum {
    kBookReaderTextLeftToRight = 0,  // 从左往右
    kBookReaderTextRightToLeft = 1,  // 从右往左
};

/** @brief 竖排文字的列方向，默认从左往右 */
int BookReaderPrefsTextDirection();

/** @brief 设置并保存竖排列方向 */
void BookReaderPrefsSetTextDirection(int direction);
/** @brief 获取页内插图开关，默认开启。 */
int BookReaderPrefsInlineImages();

/** @brief 设置并保存页内插图开关。 */
void BookReaderPrefsSetInlineImages(int value);

/** @brief 读取晃动翻页开关，默认关闭 */
int BookReaderPrefsShakeTurn();

/** @brief 设置并保存晃动翻页开关 */
void BookReaderPrefsSetShakeTurn(int value);

/** @brief 读取晃动翻页灵敏度，1–5 级，默认 3，越大越灵敏 */
int BookReaderPrefsShakeLevel();

/** @brief 设置并保存晃动翻页灵敏度 */
void BookReaderPrefsSetShakeLevel(int value);

/** @brief 获取自动翻页间隔秒数，范围 5–45 秒；启用状态仅在当前会话生效。 */
int BookReaderPrefsAutoTurnSeconds();

/** @brief 设置并保存自动翻页间隔秒数。 */
void BookReaderPrefsSetAutoTurnSeconds(int seconds);

#include "business/reader/reader_types.h"

inline constexpr const char* kBookReaderDefaultFontFile =
    BOARD_READER_FONT_FILE;  // 产品默认阅读位图字体

enum class BookTextOption {
    ParaGap,          // 段距
    LetterGap,        // 字距
    LineGap,          // 行距
    Margin,           // 旧统一边距，保留配置兼容
    Indent,           // 首行缩进字数
    UnderlineOffset,  // 下划线偏移
    AaStrength,       // 抗锯齿强度
    MarginTop,        // 上边距
    MarginBottom,     // 下边距
    MarginLeft,       // 左边距
    MarginRight,      // 右边距
    Count,            // 参数数量
};

/** @brief 读取连续排版参数；段距、行距和边距的负值表示沿用旧档位。 */
int BookReaderPrefsTextValue(BookTextOption option);

/** @brief 更新连续排版参数并提交异步保存。 */
bool BookReaderPrefsSetTextValue(BookTextOption option, int value);

/** @brief 获取供分页及绘制共享的排版参数快照。 */
TextStyle BookReaderPrefsTextStyle();

/** @brief 恢复阅读设置，不改变当前字体及书架偏好。 */
void BookReaderPrefsResetReading();
