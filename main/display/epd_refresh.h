#pragma once
#include <cstdint>

enum class EpdRefreshProfile : int { Clear = 0, Balanced = 1, Fast = 2, Extreme = 3 };
EpdRefreshProfile EpdGetRefreshProfile();
bool EpdSetRefreshProfile(EpdRefreshProfile profile);

enum class EpdBlackDepth : int { Default = 0, Medium = 1, Strong = 2 };
EpdBlackDepth EpdGetBlackDepth();
bool EpdSetBlackDepth(EpdBlackDepth depth);

enum class EpdEffect : int {
    None, // 无附加动画
    Page, // 分步翻页
    Ripple, // 水波纹
    Wipe, // 按翻页方向推白
};
enum class EpdPattern : int { White, Black, Checker, Gray, Lines, Text, Dither };
struct EpdRefreshStats {
    uint32_t submitted, completed, coalesced, updates, errors, last_ms;
    int last_error, mode, partials;
    bool busy, test_active;
    uint32_t tests_submitted, tests_completed, test_ms, test_frames;
    int test_mode;
};
bool EpdRefreshInit();
uint8_t* EpdRenderBuffer(); // UI task owns this buffer; driver never reads it directly.
uint32_t EpdSubmit(bool full, bool stream = false, bool restore_light = false);
bool EpdWait(uint32_t sequence, uint32_t timeout_ms = 10000);
EpdRefreshStats EpdGetStats();
void EpdSetEffect(EpdEffect effect);
void EpdSetReaderEffect(EpdEffect effect);
void EpdRequestPageTurn(int direction);
void EpdCancelPageTurn();
/**
 * @brief 请求切换画面，下一次提交时执行推白。
 * @param full_gc 先使用完整 GC16 全刷清理白屏，再推白显示目标画面。
 */
void EpdRequestScreenChange(bool full_gc = false);
/** @brief 下一次 EpdSubmit 完成投递后调用一次。用于等进页帧提交后再启动解码。 */
void EpdAfterNextSubmit(void (*fn)());
void EpdSetTestActive(bool active);
bool EpdTestRequest(EpdPattern pattern, int mode, bool clean = false, int repeats = 1);
void EpdTestStop();

// UI scroll frames use DU; request GL16 settling on the next committed frame.
void EpdSetUiScrolling(bool scrolling);
void EpdRequestUiScrollSettle();
