#pragma once

#include "assistant_screen.h"
#include "ui_scale.h"

#include "boot_key_handler.h"
#include "power_policy.h" // kBootLongPressMs（屏触 PTT 与 BOOT 同阈值）
#include "reader_types.h"
#include "screen_common.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>

constexpr const char* TAG = "AssistantScreen";
constexpr const char* kIdleHintAssetZh = "ic_s_assistant_hint.a2i1";  // 无会话时全屏提示图：中文走 resources；英文嵌入固件（可随 app OTA）。
constexpr int kIdleHintMaxW = DISPLAY_CONTENT_W; // 原 397 全屏 480×800
constexpr int kIdleHintMaxH = DISPLAY_CONTENT_H;

constexpr uint32_t kTouchPttLongPressMs = ::kBootLongPressMs;  // 屏触 PTT 臂听阈值：与 BOOT 共用 kBootLongPressMs，勿各写各的。
constexpr uint16_t kUiFontSize = 30;
constexpr uint16_t kUiFontBpp = 2;
constexpr uint16_t kUiFontBoldBpp = 4;

// 源值对齐 397 assistant_screen；经 UiSx/UiSy 同比例到 470
constexpr lv_coord_t kContentPadX = UiSx(8);
constexpr lv_coord_t kContentPadY = UiSy(8);
constexpr lv_coord_t kFooterH = UiSy(36);
constexpr lv_coord_t kBlockGap = UiSy(8); // 与 a2ui Column 默认 pad_row 一致，分页高度估算才准
constexpr lv_coord_t kDefaultImgW = UiSx(440);
constexpr lv_coord_t kDefaultImgH = UiSy(280);
constexpr lv_coord_t kDefaultSpacerH = UiSy(16);
constexpr lv_coord_t kDefaultImgMinH = UiSy(40);
constexpr lv_coord_t kCardPad = UiSy(12);
constexpr lv_coord_t kButtonH = UiSy(56);
constexpr lv_coord_t kBadgeExtra = UiSy(8);
constexpr lv_coord_t kProgressExtra = UiSy(6);
constexpr lv_coord_t kHeaderExtra = UiSy(10);
constexpr lv_coord_t kHeaderSubGap = UiSy(2);
constexpr lv_coord_t kListItemExtra = UiSy(20);
constexpr lv_coord_t kDividerH = UiSy(2);
constexpr lv_coord_t kBootColGap = UiSy(10);
constexpr lv_coord_t kBootColPadding = UiSx(8);
constexpr lv_coord_t kPttWaveMinBarH = UiSy(12);
constexpr int kMaxPages = 1500;  // UI 会话最多保留页数；超出从 flow 头裁，整表重分页
constexpr uint64_t kStreamCoalesceUs = 400000;  // 流式包合并窗口：主循环不再每包重排，翻页才有空档
constexpr int64_t kStreamPaintMinUs = 500000;  // 流式跟刷最少间隔（仅影响页）；翻页本身不节流

constexpr int kPttWaveBars = 27; // 相对原 11 条约 2.5 倍横向跨度
constexpr int kPttWaveFrames = 16;
constexpr lv_coord_t kPttWaveBarW = UiSx(2);
constexpr lv_coord_t kPttWaveBarGap = UiSx(2);
constexpr uint32_t kPttWaveFrameMs = 55;
// 单周期高度曲线（相对 0..44），按 ptt_wave_max_h 等比缩放；中间条先动、向两侧扩散。
constexpr uint8_t kPttWaveCurve[kPttWaveFrames] = {
    12, 16, 22, 30, 38, 44, 42, 36, 28, 20, 14, 10, 12, 18, 26, 34,
};

// 会话 flow/pages 与其中字符串一律 SPIRAM，减轻内部 DRAM。
template <typename T>
struct Assistant_SpirAlloc {
    using value_type = T;
    /** @brief 默认构造 */
    Assistant_SpirAlloc() noexcept = default;
    template <typename U>
    /** @brief 跨类型构造（无状态分配器） */
    Assistant_SpirAlloc(const Assistant_SpirAlloc<U>&) noexcept {}
    /** @brief 分配 n 个元素（优先 SPIRAM） */
    T* allocate(std::size_t n) {
        const size_t bytes = n * sizeof(T);
        void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p == nullptr) {
            p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
        }
        if (p == nullptr) {
            ESP_LOGE(TAG, "Assistant_SpirAlloc OOM %u bytes", static_cast<unsigned>(bytes));
        }
        return static_cast<T*>(p);
    }
    /** @brief 释放先前分配的内存 */
    void deallocate(T* p, std::size_t) noexcept {
        heap_caps_free(p);
    }
};
/** @brief 无状态分配器恒等比较 */
template <typename T, typename U>
constexpr bool operator==(const Assistant_SpirAlloc<T>&, const Assistant_SpirAlloc<U>&) {
    return true;
}
/** @brief 无状态分配器不等比较 */
template <typename T, typename U>
constexpr bool operator!=(const Assistant_SpirAlloc<T>&, const Assistant_SpirAlloc<U>&) {
    return false;
}

using SpirString = std::basic_string<char, std::char_traits<char>, Assistant_SpirAlloc<char>>;
template <typename T>
using SpirVec = std::vector<T, Assistant_SpirAlloc<T>>;

struct SpanRun {
    SpirString text;
    bool bold = false;
};

enum class FlowKind : uint8_t {
    Text,
    RichText,
    Math,
    Header,
    ListItem,
    Badge,
    Button,
    Status,
    Progress,
    Image,
    Divider,
    Spacer,
    CardBegin,
    CardEnd,
    MsgGap,  // 多轮之间的间距（不渲染控件）
};

struct FlowItem {
    FlowKind kind = FlowKind::Text;
    SpirString text;
    SpirString text2;
    SpirString variant = "body";
    SpirString image_url;
    SpirString action_name;
    SpirVec<SpanRun> spans;
    bool bold = false;
    bool done = false;
    bool wrap = true;
    bool center = false;     // Image/Math：父 Column/Row 或自身 align=center
    bool img_border = true;  // Image：与 a2ui 默认一致；false 时重建也要带上
    int img_w = kDefaultImgW;
    int img_h = kDefaultImgH;
    int spacer_h = kDefaultSpacerH;
    int pct = 0;
    bool math_display = false;
};

using FlowList = SpirVec<FlowItem>;
using PageList = SpirVec<FlowList>;

enum class IngestResult { kNone, kAppended, kClear };

struct AssistantUiState {
    std::atomic<bool> active{false};  // 百问页在前台（供主循环等非 LVGL 线程读；禁止在那些线程 lv_obj_is_valid）

    struct Ptt {
        std::atomic<bool> touch_ptt_held{false};  // 屏内 hit 层长按已臂听且未松手
        lv_obj_t* ptt_wave = nullptr;           // 状态栏居中录音波形（代码绘制竖条）
        lv_obj_t* touch_ptt_hit = nullptr;      // 全屏透明 PTT hit；盖板 VK 不经此层
        lv_timer_t* ptt_wave_timer = nullptr;
        lv_timer_t* touch_ptt_arm_timer = nullptr;  // 屏触 one-shot 臂听；离页/松手必须删
        bool touch_ptt_finger_down = false;         // hit 层 PRESSED 尚未抬起（LVGL 任务内）
        bool touch_suppress_click = false;          // 长按 PTT 臂听后抑制紧随 CLICKED
        bool ptt_wave_visible = false;
        lv_coord_t ptt_wave_max_h = 20;  // 竖条最大高度（Create 时按状态栏高度的约一半写入）
        lv_obj_t* ptt_wave_bars[kPttWaveBars] = {};
        uint8_t ptt_wave_frame = 0;
    } ptt;

    struct Chrome {
        lv_obj_t* scr = nullptr;
        lv_obj_t* content = nullptr;  // a2ui host：只渲染当前页
        lv_obj_t* page_label = nullptr;
        lv_obj_t* hint_img = nullptr;  // 无会话时全屏提示图
        lv_obj_t* status_label = nullptr;       // 状态栏居中时钟/文案
        lv_obj_t* notification_label = nullptr;
        reader::RasterImage* hint_raster = nullptr;  // 由 hint_img DELETE 释放
    } chrome;

    struct Layout {
        bool a2ui_ready = false;
        bool has_a2ui_content = false;
        bool replaying_history = false;
        const lv_font_t* ui_font = nullptr;
        const lv_font_t* ui_font_bold = nullptr;
        lv_coord_t viewport_w = 0;
        lv_coord_t viewport_h = 0;
        FlowList flow;
        PageList pages;
        SpirVec<int> flow_first_page;  // 与 flow 等长：每项首次落入的页码；-1 未入页（如 MsgGap）
        SpirVec<size_t> page_flow_begin;  // 与 pages 等长：该页开始填充时的 flow 下标
        size_t building_flow_fi = 0;  // 正在排版的 flow 下标
        size_t open_page_flow_begin = static_cast<size_t>(-1);
        int page_index = 0;
        int pin_flow_idx = -1;  // 本轮用户 ASR 在 flow 中的起点；-1=无
    } layout;

    struct Stream {
        std::mutex session_mu;  // 保护 flow / pages / 页码；重排不持 LVGL 锁
        FlowList coalesce_buf;
        bool coalesce_jump = false;
        bool coalesce_pending = false;
        esp_timer_handle_t coalesce_timer = nullptr;
        int64_t last_stream_paint_us = 0;
        ScreenPaintCoalesce page_paint{};  // 盖板键翻页合并绘制
    } stream;
};


/** @brief 单例 UI 状态；仅在 assistant_screen.cc 定义。 */
AssistantUiState& Assistant_State();

/** @brief 跨子模块钩子 */
const lv_font_t* Assistant_UiFont();
/** @brief UI 粗体字（fontpack） */
const lv_font_t* Assistant_UiFontBold();
/** @brief 禁用对象滚动条与滚动 */
void Assistant_DisableScroll(lv_obj_t* obj);
/** @brief 取字体行高（含行距估算） */
lv_coord_t Assistant_FontLineHeight(const lv_font_t* font);
/** @brief 单码点字形宽度 */
lv_coord_t Assistant_GlyphWidth(const lv_font_t* font, uint32_t cp);
/** @brief 按 variant 名返回字距 */
lv_coord_t Assistant_VariantLetterSpace(const char* variant);
/** @brief 按粗体标志选择 UI 字体 */
const lv_font_t* Assistant_FontFor(bool bold);
/** @brief 确保 a2ui 宿主已挂到屏幕 */
void Assistant_EnsureA2ui(lv_obj_t* scr);
/** @brief 隐藏空状态提示图 */
void Assistant_HideIdleHint();
/** @brief 显示空状态全屏提示图 */
void Assistant_ShowIdleHint();
/** @brief 解码空状态提示图到 Image 控件 */
void Assistant_LoadIdleHintImage(lv_obj_t* img);
/** @brief BOOT 或屏触任一源仍按住 */
bool Assistant_IsAnyPttHeld();
/** @brief 是否应显示 PTT 波形（Listening 且仍按住） */
bool Assistant_IsPttWaveWanted();
/** @brief 若已无 PTT 按住则停止 Listening */
void Assistant_StopListeningIfNoPttHeld(const char* why);
/** @brief 复位屏触 PTT 臂听状态 */
void Assistant_ResetTouchPttState();
/** @brief 请求同步状态栏 PTT/波形 */
void Assistant_RequestSyncPttOverlay();
/** @brief 显示或隐藏状态栏波形 */
void Assistant_ApplyPttWave(bool show);
/** @brief 停止状态栏 PTT 波形动画 */
void Assistant_StopPttWaveAnim();
/** @brief 创建全屏触控 PTT 命中层 */
lv_obj_t* Assistant_CreateTouchPttHitLayer(lv_obj_t* scr);
/** @brief 创建状态栏波形宿主控件 */
lv_obj_t* Assistant_CreatePttWaveHost(lv_obj_t* parent, lv_coord_t bar_h);
/** @brief 翻页；dir=-1 上一页，+1 下一页 */
bool Assistant_TurnPage(int dir);
/** @brief 按内容区几何全量重分页 */
void Assistant_RebuildPages();
/** @brief 仅重分页尾部（流式跟刷） */
int Assistant_RebuildPagesTail();
/** @brief 总页超过上限时从 flow 头裁掉旧内容 */
bool Assistant_TrimFlowToMaxPages();
/** @brief 将当前页索引钳到合法范围 */
void Assistant_ClampPageIndex();
/** @brief 刷新页码指示 */
void Assistant_UpdatePageIndicator();
/** @brief 立即渲染当前页到 a2ui */
void Assistant_RenderCurrentPage();
/** @brief 请求异步重绘当前页 */
void Assistant_RequestRenderCurrentPage();
/** @brief 清空当前会话 flow/pages 与相关 UI */
void Assistant_ClearSession();
/** @brief 流式分片：排队或立即展示 */
void Assistant_QueueOrShowChunk(FlowList&& chunk, bool jump_to_new);
/** @brief 从本地会话存储回放历史到 flow */
void Assistant_ReplayHistoryFromStore();
/** @brief 解析 A2UI 消息为 FlowItem 列表 */
IngestResult Assistant_IngestA2uiMessage(cJSON* msg, FlowList& out);
