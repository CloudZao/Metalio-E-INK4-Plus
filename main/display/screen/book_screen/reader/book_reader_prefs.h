#pragma once

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// 阅读偏好（NVS namespace "reader"）：
// - font：SD fonts 目录下 .ef 或 fonts_ttf 目录下 TTF/OTF 文件名
// - font_size：直接加载 TTF/OTF 的像素字号；.ef 为 0
// - spacing：行距档位（行距为主；段距随档略高于行距，不随档大幅跳变）
// - margin：页边距档位（左右/上下成套）
// - foot_msk：正文底栏展示位图（0=整条隐藏；兼容旧 hide_prog）
// - aa：阅读真 4 灰抗锯齿（L8 sidecar + 灰 LUT）
// - underline：正文下划线 0关 / 1实线 / 2虚线
// - shelf_view：书架封面网格 / 文件夹列表
// - shelf_pct：书架/首页封面与列表是否显示进度角标（默认关）
// - orient：正文方向 -1自动 / 0竖屏 / 1左横 / 2右横 / 3倒置竖屏（仅阅读页；离开后恢复竖屏）
// - text_dir：竖排列序 0从左往右 / 1从右往左
// 读走 RAM 缓存；首次 Ensure 须在 DRAM 栈；落盘在内部 RAM 栈任务。

enum {
    kBookReaderSpacingPresetCount = 4,
    kBookReaderMarginPresetCount = 4,
    kBookReaderUnderlineOff = 0,
    kBookReaderUnderlineSolid = 1,
    kBookReaderUnderlineDashed = 2,
    kBookReaderUnderlineModeCount = 3,
    kBookReaderFooterTitle = 1 << 0,     // 章节名称
    kBookReaderFooterProgress = 1 << 1,  // 百分比 + 页码
    kBookReaderFooterBattery = 1 << 2,   // 电量
    kBookReaderFooterTime = 1 << 3,      // 时间
    kBookReaderFooterAll = kBookReaderFooterTitle | kBookReaderFooterProgress |
                           kBookReaderFooterBattery | kBookReaderFooterTime,
    kBookReaderFooterDefault = kBookReaderFooterTitle | kBookReaderFooterProgress,
};

enum {
    kBookReaderShelfViewCover = 0, // 封面网格
    kBookReaderShelfViewList = 1,  // 文件夹列表
};

enum {
    kBookReaderOrientAuto = -1, // 根据重力方向自动旋转
    kBookReaderOrientPortrait = 0,  // 竖屏
    kBookReaderOrientLandLeft = 1,  // 左横
    kBookReaderOrientLandRight = 2, // 右横
    kBookReaderOrientPortraitDown = 3, // 倒置竖屏
    kBookReaderOrientCount = 4, // 手动方向档位数
};

/** @brief 确保阅读偏好已从 NVS 加载 */
void BookReaderPrefsEnsureLoaded(void);

/** @brief 当前字体文件名（仅 basename）；未 Ensure 时返回默认值。永不返回 NULL。 */
const char* BookReaderPrefsFontFile(void);
/** @brief 设置正文字体并写入 NVS；TTF/OTF 需指定 20–80px 字号，.ef 使用默认值 0。 */
void BookReaderPrefsSetFontFile(const char* filename, int size_px = 0);
/** @brief 当前 TTF/OTF 的像素字号；.ef 返回 0。 */
int BookReaderPrefsFontSize(void);
/** @brief 正文字体完整路径 */
char* BookReaderPrefsFontFullPath(char* out, size_t out_len);

/** @brief 行距档位 */
int BookReaderPrefsSpacingPreset(void);
/** @brief 设置行距档位 */
void BookReaderPrefsSetSpacingPreset(int preset);
/** @brief 行距档位文案 */
const char* BookReaderPrefsSpacingLabel(int preset);
/** @brief 行间留白占字体行框的首选百分比，实际间距按内容区自适应。 */
int BookReaderPrefsLineSpacingPercent(void);
/** @brief 段距像素 */
int BookReaderPrefsParaGap(void);

/** @brief 页边距档位 0..N-1 */
int BookReaderPrefsMarginPreset(void);
/** @brief 设置边距档位 */
void BookReaderPrefsSetMarginPreset(int preset);
/** @brief 边距档位文案 */
const char* BookReaderPrefsMarginLabel(int preset);
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
/** @brief 切换单比特；返回最新 mask */
int BookReaderPrefsToggleFooterBit(int bit);

/** @brief 阅读抗锯齿偏好（默认关闭） */
int BookReaderPrefsAntialias(void);
/** @brief 设置并保存阅读抗锯齿开关 */
void BookReaderPrefsSetAntialias(int enabled);
/**
 * @brief 按正文是否可见及阅读偏好同步 EPD 字形 AA 波形提示
 */
void BookReaderPrefsSyncGlyphAa(int text_visible);

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

// Reading background: 0 white, 1 grid, 2 dots.
int BookReaderPrefsBackground();
void BookReaderPrefsSetBackground(int mode);

// 翻页方式沿用已保存的数值，默认保持原有行为。
enum {
    kBookReaderPageTurnSlide = 0, // 左右滑动
    kBookReaderPageTurnRipple = 1, // 水波纹
    kBookReaderPageTurnWipe = 2, // 按翻页方向推白
};
int BookReaderPrefsPageTurn();
void BookReaderPrefsSetPageTurn(int mode);

enum {
    kBookReaderWritingHorizontal = 0, // 横排
    kBookReaderWritingVertical = 1, // 竖排
};
/** @brief 正文横排或竖排模式，与屏幕旋转方向独立 */
int BookReaderPrefsWritingMode();
/** @brief 设置并保存正文排版模式 */
void BookReaderPrefsSetWritingMode(int mode);

enum {
    kBookReaderTextLeftToRight = 0, // 从左往右
    kBookReaderTextRightToLeft = 1, // 从右往左
};
/** @brief 竖排文字的列方向，默认从左往右 */
int BookReaderPrefsTextDirection();
/** @brief 设置并保存竖排列方向 */
void BookReaderPrefsSetTextDirection(int direction);

// Auto page-turn interval in seconds (5..45); enabling is session-local.
int BookReaderPrefsAutoTurnSeconds();
void BookReaderPrefsSetAutoTurnSeconds(int seconds);
