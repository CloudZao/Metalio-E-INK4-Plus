#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdatomic.h>
#include <stdint.h>

#include "../epdiy.h"
#include "line_queue.h"
#include "lut.h"
#include "sdkconfig.h"

#define NUM_RENDER_THREADS 2

typedef struct {
    EpdRect area;
    EpdRect crop_to;
    const bool* drawn_lines;
    const uint8_t* data_ptr;

    /// The display width for quick access.
    int display_width;
    /// The display height for quick access.
    int display_height;

    /// index of the next line of data to process
    atomic_int lines_prepared;
    volatile int lines_consumed;
    int lines_total;

    int current_frame; // 正在供数的波形相位。
    int scan_frame; // 当前扫描的相位，供数可提前进入下一相。
    /// number of frames in the current update cycle
    int cycle_frames;
    // First streaming fault; written without logging/allocation in the ISR.
    int fault_reason,fault_phase,fault_line,fault_thread,fault_ready;
    int fault_write[NUM_RENDER_THREADS],fault_read[NUM_RENDER_THREADS];

    TaskHandle_t feed_tasks[NUM_RENDER_THREADS];
    SemaphoreHandle_t feed_done_smphr[NUM_RENDER_THREADS];
    SemaphoreHandle_t feed_space[NUM_RENDER_THREADS]; // 队列满时阻塞供数，避免占住更新任务所在内核。
    atomic_bool feed_waiting[NUM_RENDER_THREADS]; // 满队列等待已发布，供 ISR 选择性唤醒。
    SemaphoreHandle_t prefill_done; // 每相两份预填完成信号。
    SemaphoreHandle_t frame_done;
    // Set by the update owner; feeders must acknowledge through feed_done
    // before any per-update buffer can be released or reused.
    atomic_bool feed_cancelled;

    /// index of the waveform mode when using vendor waveforms.
    /// This is not necessarily the mode number if the waveform header
    // only contains a selection of modes!
    int waveform_index;
    /// waveform range when using vendor waveforms
    int waveform_range;
    /// Draw time for the current frame in 1/10ths of us.
    int frame_time;

    const int* phase_times;

    const EpdWaveform* waveform;
    enum EpdDrawMode mode;
    enum EpdDrawError error;

    // Lookup table size.
    size_t conversion_lut_size;
    // Lookup table space.
    uint8_t* conversion_lut;
    uint8_t* spatial_luts;
    uint8_t* spatial_bands;
    bool spatial_by_y;

    /// LUT lookup function. Must not be NULL.
    lut_func_t lut_lookup_func;
    /// LUT building function. Must not be NULL
    lut_build_func_t lut_build_func;

    /// 每线程一条短环形行队列（流式预填）
    LineQueue_t line_queues[NUM_RENDER_THREADS];
    uint8_t* line_threads;
    uint8_t* line_ready; // 已提交行的相位序号加一；后续相同样证明前相已提交。

    // Output line mask
    uint8_t* line_mask;
    bool line_mask_active; // 存在列更新限制时才应用输出掩码。

    /// track line skipping when working in old i2s mode
    int skipping;

    /// line buffer when using epd_push_pixels
    uint8_t* static_line_buffer;
} RenderContext_t;

/**
 * @brief 根据渲染上下文计算行跨度、数据起点、纵向范围和像素打包参数
 */
void get_buffer_params(
    RenderContext_t* ctx,
    int* bytes_per_line,
    const uint8_t** start_ptr,
    int* min_y,
    int* max_y,
    int* pixels_per_byte
);

/**
 * @brief 准备下一相的查找表和供数状态
 * @note LCD 扫描计数由扫描任务单独维护，允许扫描与下一相供数重叠。
 */
void prepare_context_for_next_frame(RenderContext_t* ctx);

/**
 * @brief 将列更新标记展开为每像素两位的输出掩码
 * @note 列更新标记为空时生成全 FF 掩码，不限制输出。
 */
void __attribute__((noinline)) epd_populate_line_mask(
    uint8_t* line_mask, const uint8_t* dirty_columns, int mask_len
);
