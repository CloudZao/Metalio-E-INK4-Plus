#pragma once

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// 阅读偏好（NVS namespace "reader"）：
// - font：SD fonts 目录下 .ef 文件名
// - spacing：行距档位（行距为主；段距随档略高于行距，不随档大幅跳变）
// - margin：页边距档位（左右/上下成套）
// - foot_msk：正文底栏展示位图（0=整条隐藏；兼容旧 hide_prog）
// - aa：阅读真 4 灰抗锯齿（L8 sidecar + 灰 LUT）
// - underline：正文下划线 0关 / 1实线 / 2虚线
// - shelf_view：书架封面网格 / 文件夹列表
// - shelf_pct：书架/首页封面与列表是否显示进度角标（默认关）
// - orient：正文方向 0竖屏 / 1左横 / 2右横（仅阅读页；离开后恢复竖屏）
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
    kBookReaderOrientPortrait = 0,  // 竖屏
    kBookReaderOrientLandLeft = 1,  // 左横
    kBookReaderOrientLandRight = 2, // 右横
    kBookReaderOrientCount = 3, // 方向档位数
};

/** @brief 确保阅读偏好已从 NVS 加载 */
void BookReaderPrefsEnsureLoaded(void);

/** @brief 当前字体文件名（仅 basename）；未 Ensure 时返回默认值。永不返回 NULL。 */
const char* BookReaderPrefsFontFile(void);
/** @brief 设置正文字体文件名并写入 NVS */
void BookReaderPrefsSetFontFile(const char* filename);
/** @brief 正文字体完整路径 */
char* BookReaderPrefsFontFullPath(char* out, size_t out_len);

/** @brief 行距档位 */
int BookReaderPrefsSpacingPreset(void);
/** @brief 设置行距档位 */
void BookReaderPrefsSetSpacingPreset(int preset);
/** @brief 行距档位文案 */
const char* BookReaderPrefsSpacingLabel(int preset);
/** @brief 行距像素 */
int BookReaderPrefsLineGap(void);
/** @brief 段距像素 */
int BookReaderPrefsParaGap(void);
/** @brief 同时取行距与段距 */
void BookReaderPrefsSpacingGaps(int preset, int* line_gap, int* para_gap);

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

/** @brief 抗锯齿偏好（470 阅读展示已强制关；保留读写兼容 NVS） */
int BookReaderPrefsAntialias(void);
/** @brief 设置抗锯齿开关（阅读侧不再生效） */
void BookReaderPrefsSetAntialias(int enabled);
/**
 * @brief 按是否正文沉浸同步 epd 字形 AA（偏好且 immersive 才开）
 * @return 运行时 AA 相对同步前是否变化（1=变，用于双平面 seed）
 */
int BookReaderPrefsSyncGlyphAa(int immersive_reading);

/** @brief 正文下划线：kBookReaderUnderlineOff / Solid / Dashed */
int BookReaderPrefsUnderlineMode(void);
/** @brief 设置下划线模式 */
void BookReaderPrefsSetUnderlineMode(int mode);

/** @brief 书架展示：kBookReaderShelfViewCover / List */
int BookReaderPrefsShelfView(void);
/** @brief 设置书架视图模式 */
void BookReaderPrefsSetShelfView(int view);

/** @brief 正文方向：kBookReaderOrientPortrait / LandLeft / LandRight */
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
