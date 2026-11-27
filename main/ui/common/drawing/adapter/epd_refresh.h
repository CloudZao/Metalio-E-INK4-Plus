#pragma once
#include <cstdint>
#include "config.h"

enum class EpdRefreshProfile : int {
    Clear = 0,     // 清晰档
    Balanced = 1,  // 均衡档
    Fast = 2,      // 快速档
};

/** @brief 检查板级声明是否支持该刷新档位；后端另行校验波形资源。 */
static constexpr bool EpdProfileSupported(EpdRefreshProfile profile) {
    const int value = static_cast<int>(profile);
    return value >= 0 && value <= 2 && (BOARD_REFRESH_PROFILE_MASK & (1 << value)) != 0;
}
static_assert(EpdProfileSupported(static_cast<EpdRefreshProfile>(BOARD_REFRESH_PROFILE_DEFAULT)));

/** @brief 读取当前刷新档位。 */
EpdRefreshProfile EpdGetRefreshProfile();

/** @brief 设置本板支持的刷新档位，不支持时返回false。 */
bool EpdSetRefreshProfile(EpdRefreshProfile profile);

enum class EpdBlackDepth : int {
    Default = 0,  // −1200 mV
    Medium = 1,   // −1600 mV
    Strong = 2,   // −2000 mV
};

/** @brief 获取无有效校准记录时使用的黑色深度档位 */
EpdBlackDepth EpdGetBlackDepth();

/** @brief 设置并启用黑色深度备用档位，已有有效校准值时拒绝修改 */
bool EpdSetBlackDepth(EpdBlackDepth depth);

/** @brief 将已校验的负毫伏校准值投递给显示线程 */
bool EpdSetVcomMv(int vcom_mv);

/** @brief 获取显示线程当前采用的负毫伏值 */
int EpdGetVcomMv();

enum class EpdEffect : int {
    None,    // 无附加动画
    Page,    // 分步翻页
    Ripple,  // 水波纹
    Wipe,    // 按翻页方向推白
};

enum class EpdPattern : int {
    White,    // 白底
    Black,    // 黑底
    Checker,  // 棋盘格
    Gray,     // 灰阶
    Lines,    // 线条
    Text,     // 文字
    Dither,   // 抖点
};
struct EpdRefreshStats {
    uint32_t submitted;        // 已提交次数
    uint32_t completed;        // 已完成次数
    uint32_t coalesced;        // 已合并次数
    uint32_t updates;          // 物理刷新次数
    uint32_t errors;           // 刷新失败次数
    uint32_t last_ms;          // 最近刷新耗时，毫秒
    int last_error;            // 最近错误码
    int mode;                  // 后端刷新模式
    int partials;              // 局部刷新计数
    bool busy;                 // 正在刷新
    bool test_active;          // 测试模式已启用
    uint32_t tests_submitted;  // 已提交测试次数
    uint32_t tests_completed;  // 已完成测试次数
    uint32_t test_ms;          // 测试耗时，毫秒
    uint32_t test_frames;      // 测试帧数
    int test_mode;             // 测试刷新模式
};

/** @brief 初始化选中后端的刷新协调。 */
bool EpdRefreshInit();

/** @brief 获取界面独占的绘制缓冲，调用方不得释放。 */
uint8_t* EpdRenderBuffer();
struct EpdFrameBuffer {
    uint8_t* pixels;     // UI上下文借用的像素缓冲
    int width;           // 面板物理列数
    int height;          // 面板物理行数
    int stride;          // 每行字节数
    int bits_per_pixel;  // I1高位在前或EPDiy的4位像素
};

/** @brief 获取当前后端的绘制缓冲与布局，仅在UI上下文或持显示锁时使用 */
EpdFrameBuffer EpdGetFrameBuffer();

/** @brief 提交当前画面，返回供等待物理完成使用的序号。 */
uint32_t EpdSubmit(bool full, bool stream = false, bool restore_light = false);

/** @brief 等待指定提交物理完成，失败或超时返回false。 */
bool EpdWait(uint32_t sequence, uint32_t timeout_ms = 10000);

/** @brief 读取刷新统计快照。 */
EpdRefreshStats EpdGetStats();

/** @brief 尝试进入 UI 构建/绘制区；同任务允许嵌套，失败时保留请求稍后重试。 */
bool EpdTryBeginUiRender();

/** @brief 结束本任务的一层构建/绘制；全部退出后才可等待物理刷新。 */
void EpdEndUiRender();

/** @brief 设置当前显示效果，具体支持范围由后端限定。 */
void EpdSetEffect(EpdEffect effect);

/** @brief 设置阅读翻页效果。 */
void EpdSetReaderEffect(EpdEffect effect);

struct EpdPageTurnArea {
    int x = 0; // 正文区左上角横坐标
    int y = 0; // 正文区左上角纵坐标
    int width = 0; // 正文区宽度，像素
    int height = 0; // 正文区高度，像素
};

/** @brief 请求按指定方向翻页。 */
void EpdRequestPageTurn(int direction, const EpdPageTurnArea& area);

/** @brief 取消尚未完成的翻页效果。 */
void EpdCancelPageTurn();

/**
 * @brief 请求画面切换，下一次提交按当前后端能力刷新。
 * @param full_gc 请求后端支持的完整清理刷新；不支持推白的后端不增加推白。
 */
void EpdRequestScreenChange(bool full_gc = false);

/** @brief 请求下一次提交时统一整页纯黑像素的推黑驱动，不执行整页擦白。 */
void EpdRequestBlackEqualization();

/** @brief 下一次 EpdSubmit 完成投递后调用一次。用于等进页帧提交后再启动解码。 */
void EpdAfterNextSubmit(void (*fn)());

/** @brief 启用或停止显示测试状态。 */
void EpdSetTestActive(bool active);

/** @brief 提交显示测试请求，无法接纳时返回false。 */
bool EpdTestRequest(EpdPattern pattern, int mode, bool clean = false, int repeats = 1);

/** @brief 停止显示测试。 */
void EpdTestStop();

// 滚动帧与收尾帧的波形由所选后端决定。

/** @brief 更新界面滚动状态。 */
void EpdSetUiScrolling(bool scrolling);

/** @brief 请求在滚动结束后完成后端收尾刷新。 */
void EpdRequestUiScrollSettle();
