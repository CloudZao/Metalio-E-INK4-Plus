#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int x;     // 逻辑竖屏 X
    int y;     // 逻辑竖屏 Y
    int raw_x; // 芯片原始 X
    int raw_y; // 芯片原始 Y
} metalio_touch_point_t;

typedef struct {
    bool ok;
    uint32_t samples, read_errors, irqs;
    int last_error, raw_x, raw_y, irq_level, pause_depth;
    uint32_t raw_zero, raw_down, raw_up, raw_contact, raw_reserved, raw_multi;
    uint32_t duplicate_down, recovered_edges, explicit_releases, id_changes;
    uint32_t invalid_reports, boundary_suppressed;
    uint32_t max_valid_gap_us, max_i2c_wait_us, max_i2c_read_us;
} metalio_touch_stats_t;
metalio_touch_stats_t metalio_touch_stats(void);

/**
 * @brief 采样回调：count=0 抬起；>0 按下（raw/logical 有效）
 * @note 在 touch_feed 任务调用，可做盖板键；勿长时间占 I2C/LVGL
 * @note count=0也可能是遗漏UP的新DOWN恢复边界；场景重武装时必须同步查询
 *       metalio_touch_sample_confirms_release()，不能把合成边界当真实松手。
 */
typedef void (*metalio_touch_sample_cb_t)(uint8_t count, int raw_x, int raw_y, int lx, int ly);

/** Only query synchronously inside sample_cb. A synthetic recovered release
 *  ends the old gesture but is not proof that all fingers left a scene fence. */
bool metalio_touch_sample_confirms_release(void);
// Query only inside sample_cb, under the consumer's input fence if needed.
// Rejects a sampled report overtaken by a pause/resume generation boundary.
bool metalio_touch_sample_is_current(void);

/**
 * @brief 复位并探测 FT6336U；GPIO5 下降沿中断 + 独立 feed 任务
 * @return true 芯片有应答
 */
bool metalio_touch_init(void);

/**
 * @brief 注册采样回调（可空）；盖板键 / 早震等挂这里
 */
void metalio_touch_set_sample_cb(metalio_touch_sample_cb_t cb);

/**
 * @brief 读屏内触点（无 I2C；由 feed 更新）
 * @return true 应报 PRESSED（含刷屏期间错过的短按回放）
 * @note 短按若整段落在 LVGL 未轮询窗口，会先回放一次 PRESSED，再 RELEASED
 */
bool metalio_touch_read(metalio_touch_point_t* out);

/**
 * @brief 是否还有未交付的 down/up（含短按回放的待 RELEASED）
 */
bool metalio_touch_has_pending_edges(void);

/**
 * @brief 供 indev：标记盖板键占用，屏内点不喂 LVGL
 */
void metalio_touch_set_ui_suppress(bool suppress);

/**
 * @brief 暂停/恢复 touch_feed（待机 Park/GC16 与浅睡前；对齐 397 PauseStandbyBackgroundWork）
 * @note Flash/锁屏边界清旧UI回放；恢复后须真实松手重武装。普通快刷不调用暂停。
 */
void metalio_touch_set_feed_paused(bool paused);

/**
 * @brief 触控芯片是否已探测成功
 */
bool metalio_touch_ok(void);

#ifdef __cplusplus
}
#endif

