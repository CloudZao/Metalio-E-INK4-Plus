#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "ui/common/runtime/page_lifecycle.h"
#include "boards/common/board/board.h"
#include "ui/apps/workbuddy/workbuddy_screen.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "assets/lang_workbuddy.h"
#include "business/workbuddy/workbuddy_session.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "ui/common/drawing/adapter/lv_adapter_display.h"
#include "ui/common/drawing/font/font_awesome.h"
#include "ui/common/drawing/font/ui_fonts.h"
#include "ui/common/drawing/screen_draw.h"
#include "ui/common/runtime/haptic_feedback.h"
#include "ui/common/runtime/input/boot_key_handler.h"
#include "ui/common/widgets/screen_common.h"
#include "ui/common/runtime/modal.h"
#include "ui/common/widgets/screen_text.h"
#include "ui/common/runtime/input/vk_key_handler.h"
#include "ui/common/runtime/input/vk_page_repeat.h"
#include "ui/common/widgets/ui_qrcode.h"
#include "ui/common/drawing/ui_scale.h"
#include "ui/apps/workbuddy/workbuddy_chat.h"
#include "ui/common/drawing/ui_theme.h"
#include "ui/common/runtime/screen_common_nav.h"
#include "ui/common/runtime/lvgl/ui_dispatch.h"

LV_FONT_DECLARE(font_awesome_30_1);

static UiPageLifecycle s_page_lifecycle;

static constexpr const char* kTag = "WorkBuddyScreen";
static constexpr int32_t kContentPad = UiSx(8);
static constexpr int32_t kTouchSlop = UiSx(24);
static constexpr int64_t kMinResultPaintUs = 1500000;
static constexpr int64_t kMinTranscriptPaintUs = 1000000;
static constexpr int kWaveBars = 27;  // 沿用百问AI波形；实时正文出字时降低动画频率，仅更新顶部小区域，不表示实际音量。
static constexpr int kWaveFrames = 16;
static constexpr uint32_t kWaveFrameMs = 200;
static constexpr uint8_t kWaveCurve[kWaveFrames] = {12, 16, 22, 30, 38, 44, 42, 36, 28, 20, 14, 10, 12, 18, 26, 34};
static const lv_style_prop_t kNoTransitionProperties[] = {LV_STYLE_PROP_INV};  // 有效的空动画属性列表
/**
 * @brief 本页面使用的有效空过渡，描述符和属性数组均保持到程序结束。
 * 当前LVGL在状态切换时直接读取transition->props，写入nullptr会导致按压崩溃。
 */
static const lv_style_transition_dsc_t kNoTransition = {kNoTransitionProperties, nullptr, lv_anim_path_linear, 0, 0};
static UiModal s_menu_modal;
static UiModal s_binding_modal;

static const char* ConnectionText(WorkBuddyConnectionState state) {
    switch (state) {
        case WorkBuddyConnectionState::Querying:
            return LangString_WB_STATUS_QUERYING;
        case WorkBuddyConnectionState::Failed:
            return LangString_WB_STATUS_FAILED;
        case WorkBuddyConnectionState::Online:
            return LangString_WB_STATUS_ONLINE;
        case WorkBuddyConnectionState::Offline:
            return LangString_WB_STATUS_OFFLINE;
        default:
            return LangString_WB_STATUS_UNKNOWN;
    }
}

/** @brief 跨任务传递按键意图；输入任务不得读取页面或调用 LVGL。 */
enum class WorkbuddyScreenInputCommand {
    Home,      // 返回应用首页
    Back,      // 盖板返回键：上一页正文，无上一页时退出应用
    Previous,  // 上一页正文
    Next,      // 下一页正文
    BootLong,  // BOOT 长按开始录音
    BootUp,    // BOOT 松开结束录音
    Repeat     // 盖板键长按连翻
};

/** @brief 只携带页面代号和按键，不携带可能随离页释放的控件指针。 */
struct WorkbuddyScreenInputMessage {
    uint32_t generation;                  // 输入所属页面代号
    WorkbuddyScreenInputCommand command;  // 待处理的按键动作
    int page_delta;                       // 连翻步长，其余命令为 0
};

static constexpr UBaseType_t kInputQueueLength = 16;
static StaticQueue_t s_input_queue_control;
static uint8_t s_input_queue_storage[kInputQueueLength * sizeof(WorkbuddyScreenInputMessage)];
static QueueHandle_t s_input_queue = nullptr;
static std::atomic<uint32_t> s_active_input_generation{0};
static std::atomic<uint32_t> s_wave_generation{0};  // 只向显示驱动暴露拥有波形的页面代号，不跨线程传递控件指针。
static std::atomic<uint32_t> s_input_overflow_generation{0};
static uint32_t s_next_input_generation = 0;  // 仅 LVGL 线程递增，0 表示页面不接收输入。

/**
 * @brief 非阻塞投递按键，固定队列不为连续翻页分配堆内存。
 * @param command 按键动作。
 * @param page_delta 连翻步长，普通按键使用默认值 0。
 * @return 无；队列满时由界面线程停止录音并丢弃积压输入。
 */
static void QueueInput(WorkbuddyScreenInputCommand command, int page_delta = 0) {
    const uint32_t generation = s_active_input_generation.load(std::memory_order_acquire);
    if (generation == 0) {
        return;
    }
    const WorkbuddyScreenInputMessage message{generation, command, page_delta};
    if (xQueueSend(s_input_queue, &message, 0) != pdPASS) {
        // 不能丢失 BOOT 松开后仍让录音继续，溢出统一执行安全收尾。
        s_input_overflow_generation.store(generation, std::memory_order_release);
    }
}

/** @brief 授权任务独立于录音任务，不持有 LVGL 指针，也不提前申请录音缓冲。 */
/** @brief 页面控件与定时器，只允许 LVGL 线程访问。 */
struct WorkbuddyScreenPage {
    uint32_t input_generation = 0;                                // 页面独立输入代号
    int64_t last_result_paint_us = 0;                             // 上次正文更新的单调时钟时间，微秒
    bool touch_active = false;                                    // 是否仍跟踪本次屏幕按压
    bool touch_ptt_held = false;                                  // 屏触是否已经进入长按录音
    bool touch_suppress_click = false;                            // 本次松手后是否抑制短点翻页
    int64_t touch_pressed_us = 0;                                 // 屏幕按下的时间，微秒
    lv_timer_t* touch_arm_timer = nullptr;                        // 等待长按阈值的一次性定时器
    lv_obj_t* touch_hit = nullptr;                                // 正文录音与翻页热区
    lv_point_t touch_start{};                                     // 本次按压起点
    lv_point_t touch_last{};                                      // 最近一次按压坐标
    lv_obj_t* root = nullptr;                                     // 页面根对象
    lv_obj_t* status_clock = nullptr;                             // 共用状态栏的时钟标签
    lv_obj_t* status_notification = nullptr;                      // 共用状态栏的通知标签
    lv_obj_t* wave = nullptr;                                     // 倾听波形容器
    lv_obj_t* wave_bars[kWaveBars]{};                             // 倾听动画的短线控件
    lv_timer_t* wave_timer = nullptr;                             // 倾听动画定时器
    lv_coord_t wave_max_height = 0;                               // 波形可用高度，像素
    uint8_t wave_frame = 0;                                       // 当前波形动画帧
    bool wave_visible = false;                                    // 波形是否已显示
    lv_obj_t* content = nullptr;                                  // 对话内容容器
    WorkBuddyChat chat;                                           // 有界消息、气泡和分页
    uint32_t next_turn_id = 0;                                    // 本页的语音轮次序号
    bool final_transcript_painted = false;                        // 当前轮最终文字是否已经定位
    lv_obj_t* title = nullptr;                                    // 居中的 30 号粗体标题
    lv_obj_t* connection_label = nullptr;                         // 现有中文最小档 25 号常规字连接状态
    lv_obj_t* menu_button = nullptr;                              // 标题右侧竖向三点菜单
    lv_obj_t* menu_overlay = nullptr;                             // 可点击关闭的透明菜单遮罩
    lv_obj_t* binding_dialog = nullptr;                           // 保留底层内容的重新绑定确认层
    lv_obj_t* local_messages_button = nullptr;                    // 同步消息菜单项
    lv_obj_t* action_label = nullptr;                             // 助理消息按钮的当前文字
    lv_obj_t* refresh_button = nullptr;                           // 二维码刷新按钮
    lv_obj_t* refresh_label = nullptr;                            // 刷新状态文字
    lv_obj_t* qr_frame = nullptr;                                 // 共用样式的二维码外框
    lv_obj_t* switch_account_button = nullptr;                    // 重新绑定菜单项
    bool reauthorization_required = false;                        // 当前凭证或权限需要重新绑定
    lv_obj_t* qr_panel = nullptr;                                 // 扫码页面及刷新操作容器
    lv_obj_t* qr = nullptr;                                       // 二维码内容控件
    lv_obj_t* qr_hint = nullptr;                                  // 二维码下方的操作或错误提示
    std::string painted_url;                                      // 已经编码的授权地址
    std::shared_ptr<WorkBuddyAuthorization> authorization;  // 当前页面的授权上下文
    int64_t authorization_retry_at_us = 0;                        // 本地任务资源不足时的重试时间，仅由页面节拍推进
    uint32_t painted_auth_revision = 0;                           // 已经渲染的授权快照版本
    // 页面停留期间软保网；仍允许系统按无操作时间进入待机，避免每轮提问重新连 Wi-Fi。
    std::optional<PowerNeedHold> network_hold;                        // 页面原位持有前台保网引用，待机或离页时归还
    std::shared_ptr<WorkBuddyTurn> turn;                        // 当前语音或助理消息请求
    std::weak_ptr<WorkBuddyTurn> painted_turn;                  // 当前显示内容对应的请求
    WorkBuddyPhase painted_phase = WorkBuddyPhase::Idle;  // 当前显示的处理阶段
    uint32_t painted_result_revision = 0;                             // 已经显示的结果版本
    bool closed = false;                                              // 页面是否已完成离页清理
};

static WorkbuddyScreenPage* s_current_page = nullptr;  // 仅 LVGL 线程访问当前页面
static bool s_standby = false;                         // 待机覆盖层暂停交互和绘制

/**
 * @brief 使用真实采样和按压状态判断倾听，避免麦克风准备失败时仍显示动画。
 * @param page 当前LVGL页面。
 * @return 是否仍在前台持续采音。
 */
static bool IsListening(const WorkbuddyScreenPage* page) {
    const auto& turn = page->turn;
    return !page->closed && s_active_input_generation.load() == page->input_generation && turn &&
           turn->phase.load() == WorkBuddyPhase::Recording && turn->samples.load() > 0 && turn->held.load() &&
           !turn->finish_requested.load() && !turn->gate.IsCancelled() && (TouchUiFingerIsDown() || BootKey_IsHeld());
}

/**
 * @brief 绘制与百问AI相同的对称波浪，仅修改27条短线的高度。
 * @param page 已创建波形控件的页面。
 * @return 无；不修改正文或触发整页强制刷新。
 */
static void PaintListeningWave(WorkbuddyScreenPage* page) {
    for (int i = 0; i < kWaveBars; ++i) {
        const int phase = (page->wave_frame + std::abs(i - kWaveBars / 2)) % kWaveFrames;
        // 统一使用LVGL坐标类型；Xtensa上的lv_coord_t可能是long，不能与int字面量混合推导模板。
        const lv_coord_t height = std::max<lv_coord_t>(2, kWaveCurve[phase] * page->wave_max_height / 44);
        lv_obj_set_height(page->wave_bars[i], height);
    }
}

/** @brief 同步动画可见性并管理定时器；@param page 当前页面；@param show 是否显示；@return 无。 */
static void SetListeningWave(WorkbuddyScreenPage* page, bool show);

/**
 * @brief 仅在实际采音时推进波浪；松手或采集结束后及时释放动画定时器。
 * @param timer 页面独占的LVGL动画定时器。
 * @return 无；所有控件操作均在LVGL任务内执行。
 */
static void OnListeningWave(lv_timer_t* timer) {
    auto* page = static_cast<WorkbuddyScreenPage*>(lv_timer_get_user_data(timer));
    if (!IsListening(page)) {
        SetListeningWave(page, false);
        return;
    }
    page->wave_frame = static_cast<uint8_t>((page->wave_frame + 1) % kWaveFrames);
    PaintListeningWave(page);
}

/**
 * @brief 切换倾听动画与状态栏文字，动画结束后归还中央区域。
 * @param page 当前页面，控件尚未删除。
 * @param show 是否显示倾听波形。
 * @return 无；隐藏时销毁定时器，离页后的旧页面不能清除新页面的可见标记。
 */
static void SetListeningWave(WorkbuddyScreenPage* page, bool show) {
    if (show == page->wave_visible) {
        return;
    }
    page->wave_visible = show;
    if (show) {
        s_wave_generation.store(page->input_generation);
        lv_obj_add_flag(page->status_clock, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(page->status_notification, LV_OBJ_FLAG_HIDDEN);
        page->wave_frame = 0;
        PaintListeningWave(page);
        lv_obj_remove_flag(page->wave, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(page->wave);
        page->wave_timer = lv_timer_create(OnListeningWave, kWaveFrameMs, page);
        if (!page->wave_timer) {
            ESP_LOGW(kTag, "Listening wave timer unavailable");
        }
    } else {
        if (page->wave_timer) {
            lv_timer_delete(page->wave_timer);
            page->wave_timer = nullptr;
        }
        lv_obj_add_flag(page->wave, LV_OBJ_FLAG_HIDDEN);
        uint32_t generation = page->input_generation;
        s_wave_generation.compare_exchange_strong(generation, 0);
        // 动画期间若有新系统通知，结束时保留它，等待原通知定时器恢复时钟。
        if (lv_obj_has_flag(page->status_notification, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(page->status_clock, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/**
 * @brief 在状态栏中央建立倾听波形，控件不拦截全屏长按与松手事件。
 * @param page 新建页面。
 * @param bar 页面拥有的状态栏，提供中央文字和高度。
 * @return 无；初始保持隐藏，首个 Opus 包到达后才启动动画。
 */
static void CreateListeningWave(WorkbuddyScreenPage* page, const EpdStatusBar& bar) {
    page->status_clock = bar.status_label;
    page->status_notification = bar.notification_label;
    page->wave_max_height = std::max<lv_coord_t>(12, bar.height / 2);
    page->wave = lv_obj_create(page->root);
    lv_obj_remove_style_all(page->wave);
    lv_obj_set_size(page->wave, kWaveBars * 4 - 2, page->wave_max_height);
    lv_obj_align(page->wave, LV_ALIGN_TOP_MID, 0, (bar.height - page->wave_max_height) / 2);
    lv_obj_set_flex_flow(page->wave, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(page->wave, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(page->wave, 2, 0);
    lv_obj_remove_flag(page->wave, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    for (int i = 0; i < kWaveBars; ++i) {
        auto* line = lv_obj_create(page->wave);
        page->wave_bars[i] = line;
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, 2, 2);
        lv_obj_set_style_bg_color(line, UiColorForeground(), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(line, 1, 0);
        lv_obj_remove_flag(line, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    }
    lv_obj_add_flag(page->wave, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 仅在文字变化时更新标签，避免状态轮询产生无效的墨水屏脏区。
 * @param label 已创建的标签。
 * @param value 新文字，不允许为空指针。
 * @return 无。
 */
static void SetLabelIfChanged(lv_obj_t* label, const char* value) {
    if (label && std::strcmp(lv_label_get_text(label), value) != 0) {
        lv_label_set_text(label, value);
    }
}

/** @brief 显示居中的授权或连接提示，不修改已保存的对话。 */
static void ShowIntro(WorkbuddyScreenPage* page, std::string text) {
    page->chat.ShowIntro(text);
}

/** @brief 仅在对话可见且没有菜单或确认框时处理分页。 */
static bool TurnTextPage(WorkbuddyScreenPage* page, int delta) {
    if (lv_obj_has_flag(page->content, LV_OBJ_FLAG_HIDDEN) ||
        !lv_obj_has_flag(page->menu_overlay, LV_OBJ_FLAG_HIDDEN) ||
        !lv_obj_has_flag(page->binding_dialog, LV_OBJ_FLAG_HIDDEN)) {
        return false;
    }
    return page->chat.TurnPage(delta);
}

/**
 * @brief 判断当前页面是否已有录音准备、采集或网络请求，防止并发提交。
 * @param page 当前页面，允许为空。
 * @return 存在未完成对话时为 true。
 */
static bool IsBusy(const WorkbuddyScreenPage* page) {
    if (!page || !page->turn) {
        return false;
    }
    const WorkBuddyPhase phase = page->turn->phase.load();
    return page->turn->stream_running.load() || phase == WorkBuddyPhase::Preparing ||
           phase == WorkBuddyPhase::Recording || phase == WorkBuddyPhase::Recognizing ||
           phase == WorkBuddyPhase::Sending || phase == WorkBuddyPhase::Waiting;
}

/**
 * @brief 新提问或手动查看历史前停止上一轮后台读取，不取消电脑已经受理的任务。
 * @param page 当前LVGL页面。
 * @return 无；实际关闭套接字交给清理任务，不阻塞界面。
 */
static void CancelPreviousWatch(WorkbuddyScreenPage* page) {
    if (!page || !page->turn || !page->turn->watch_running.load()) {
        return;
    }
    auto previous = page->turn;
    WorkBuddyCancelTurn(previous);
}

/**
 * @brief 分别更新助理消息、重新绑定和二维码刷新入口。
 * @param page 当前有效页面。
 * @return 无。
 */
static void UpdateInputActions(WorkbuddyScreenPage* page) {
    if (!page || !page->authorization) {
        return;
    }
    const auto auth = page->authorization->phase.load();
    const bool authorized = auth == WorkBuddyAuthPhase::Authorized;
    const bool busy = IsBusy(page);
    const bool authorizing =
        auth == WorkBuddyAuthPhase::Checking || auth == WorkBuddyAuthPhase::Authorizing;
    const bool refreshing = page->authorization->refresh_requested.load();
    const bool running = page->authorization->running.load();
    const auto set_disabled = [](lv_obj_t* button, bool disabled) {
        if (disabled) {
            lv_obj_add_state(button, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(button, LV_STATE_DISABLED);
        }
    };
    SetLabelIfChanged(page->action_label, LangString_WB_SYNC_MESSAGES);
    set_disabled(page->local_messages_button, !authorized || busy || running || page->reauthorization_required);
    set_disabled(page->switch_account_button, busy || running);
    set_disabled(page->menu_button, busy || authorizing);
    SetLabelIfChanged(page->refresh_label, refreshing ? LangString_WB_REFRESHING : LangString_CLOUD_REFRESH);
    if (Board::GetInstance().IsNetworkReady()) {
        lv_obj_remove_flag(page->refresh_button, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(page->refresh_button, LV_OBJ_FLAG_HIDDEN);
    }
    set_disabled(page->refresh_button, authorizing || refreshing);
}

/** @brief 尝试创建授权任务，资源未释放时由现有页面节拍延后重试 */
static void TryStartAuthorization(WorkbuddyScreenPage* page) {
    const int retry_ms = WorkBuddyStartAuthorization(page->authorization);
    page->authorization_retry_at_us = retry_ms ? esp_timer_get_time() + retry_ms * 1000LL : 0;
}

/**
 * @brief 启动或刷新授权任务；换绑刷新保留模式，成功前保留正式凭证及旧账号消息。
 * @param page 当前有效页面。
 * @param create_new 是否跳过旧会话并重新扫码授权。
 * @param switch_account 是否由用户主动请求更换绑定账号，默认普通授权。
 * @return 无；已有授权轮询时只置刷新位，不并发建立授权连接。
 */
static void StartAuthorization(WorkbuddyScreenPage* page, bool create_new, bool switch_account = false) {
    if (!page || page->closed || s_standby) {
        return;
    }
    if (page->authorization && page->authorization->running.load()) {
        if (switch_account) {
            page->authorization->switch_account.store(true);
        }
        page->authorization->refresh_requested.store(true);
        return;
    }
    // 任务创建失败后重试也保留换绑意图，避免退回只允许原账号续授权的普通二维码。
    if (page->authorization) {
        auto previous = page->authorization;
        WorkBuddyCancelAuthorization(previous);
    }
    const bool switching =
        switch_account || (create_new && page->authorization &&
                           page->authorization->phase.load() != WorkBuddyAuthPhase::Authorized &&
                           page->authorization->switch_account.load());
    // 重新授权只会发生在尚未录音或上一轮请求已结束时，清掉旧错误避免授权后再次跳回二维码。
    CancelPreviousWatch(page);
    page->turn.reset();
    page->reauthorization_required = false;
    auto authorization = std::make_shared<WorkBuddyAuthorization>();
    authorization->switch_account.store(switching);
    authorization->refresh_requested.store(create_new);
    authorization->running.store(true);
    page->authorization = authorization;
    page->painted_auth_revision = 0;
    page->painted_phase = WorkBuddyPhase::Idle;
    page->painted_result_revision = 0;
    page->painted_turn.reset();
    page->painted_url.clear();
    lv_obj_add_flag(page->menu_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(page->binding_dialog, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(page->qr_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(page->content, LV_OBJ_FLAG_HIDDEN);
    ShowIntro(page, switching ? LangString_WB_RELINK_PREPARING : LangString_WB_LINK_CHECKING);
    TryStartAuthorization(page);
}

/** @brief 在音频任务启动采音；@param turn 已收到实时连接ready的会话；@return 无。 */

/**
 * @brief 长按后先建立实时识别通道，收到ready再开麦，连接期间提供明确等待提示。
 * @param page 仍然有效的前台页面。
 * @return 无；未授权或已有请求时忽略按压，旧任务不会被重复提交。
 */
static void Begin(WorkbuddyScreenPage* page) {
    if (!page || page->closed || !page->authorization ||
        page->authorization->phase.load() != WorkBuddyAuthPhase::Authorized || page->reauthorization_required ||
        IsBusy(page) || !lv_obj_has_flag(page->menu_overlay, LV_OBJ_FLAG_HIDDEN) ||
        !lv_obj_has_flag(page->binding_dialog, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (page->turn && page->turn->phase.load() == WorkBuddyPhase::Error && !page->turn->is_local_refresh) {
        WorkBuddyFinishTurn(page->turn);
    }
    CancelPreviousWatch(page);
    auto turn = std::make_shared<WorkBuddyTurn>();
    if (++page->next_turn_id == 0) {
        ++page->next_turn_id;
    }
    turn->ui_id = page->next_turn_id;
    page->final_transcript_painted = false;
    page->turn = turn;
    turn->input_held = []() { return TouchUiFingerIsDown() || BootKey_IsHeld(); };
    WorkBuddyBeginTurn(turn);
}

/**
 * @brief 点击时读取PC会话历史，不把“助理消息”误当成旧消息编号之后的增量轮询。
 * @param event 当前页面的助理消息按钮点击事件。
 * @return 无；不发送新指令，原助理任务在离页或断网后仍由电脑继续处理。
 */
static void OnLocalMessages(lv_event_t* event) {
    auto* page = static_cast<WorkbuddyScreenPage*>(lv_event_get_user_data(event));
    if (!page || page->closed || IsBusy(page) || !page->authorization ||
        page->authorization->phase.load() != WorkBuddyAuthPhase::Authorized) {
        return;
    }
    if (page->turn && !page->turn->is_local_refresh && page->turn->phase.load() == WorkBuddyPhase::Error) {
        WorkBuddyFinishTurn(page->turn);
    }
    CancelPreviousWatch(page);
    auto turn = std::make_shared<WorkBuddyTurn>();
    turn->is_local_refresh = true;
    turn->held.store(false);
    turn->finish_scheduled.store(true);
    turn->phase.store(WorkBuddyPhase::Sending);
    page->turn = turn;
    lv_obj_add_flag(page->menu_overlay, LV_OBJ_FLAG_HIDDEN);
    page->painted_phase = WorkBuddyPhase::Idle;
    UpdateInputActions(page);
    WorkBuddyReadMessages(turn);
}

/**
 * @brief 仅当屏触与BOOT都已松开才结束本轮，避免两种录音输入相互打断。
 * @param page 当前页面，允许为空。
 * @return 无；真正停止采集由状态定时器投递到音频任务。
 */
static void Release(WorkbuddyScreenPage* page) {
    if (page && page->turn && !page->touch_ptt_held && !BootKey_IsHeld()) {
        page->turn->held.store(false);
    }
    if (page && !IsListening(page)) {
        SetListeningWave(page, false);
    }
}

/**
 * @brief 取消尚未触发的长按定时器，松手或离页后不允许迟到开始录音。
 * @param page 当前LVGL页面。
 * @return 无。
 */
static void CancelTouchArm(WorkbuddyScreenPage* page) {
    if (page->touch_arm_timer) {
        lv_timer_delete(page->touch_arm_timer);
        page->touch_arm_timer = nullptr;
    }
}

/**
 * @brief 清除本次屏触持有并按多源规则收尾，长按后的点击抑制由调用方保留。
 * @param page 当前LVGL页面。
 * @return 无。
 */
static void EndTouchPress(WorkbuddyScreenPage* page) {
    CancelTouchArm(page);
    page->touch_active = false;
    page->touch_ptt_held = false;
    Release(page);
}

/** @brief 全屏短点翻页及长按录音入口；@param event 触摸事件。 */
static void OnContentTouch(lv_event_t* event);
/** @brief 刷新当前扫码流程的二维码 */
static void OnRefresh(lv_event_t* event);
/** @brief 打开重新绑定确认框；@param event 菜单项点击事件。 */
static void OnSwitchAccount(lv_event_t* event);

/**
 * @brief 离页只置取消位，HTTP 关闭和音频停止交给相应工作任务，避免卡住 LVGL。
 * @param page 即将卸载的页面。
 * @return 无；后台只持有共享请求上下文，随后完成资源回收。
 */
static void Cancel(WorkbuddyScreenPage* page) {
    if (page->closed) {
        return;
    }
    page->closed = true;
    uint32_t generation = page->input_generation;
    if (s_active_input_generation.compare_exchange_strong(generation, 0, std::memory_order_acq_rel)) {
        VkPageRepeatStop();
    }
    EndTouchPress(page);
    SetListeningWave(page, false);
    // 先拆子控件回调，避免父屏删除后按压丢失事件继续引用已释放的 Page。
    if (page->touch_hit) {
        lv_obj_remove_event_cb_with_user_data(page->touch_hit, OnContentTouch, page);
        HapticDetachClick(page->touch_hit);
    }
    if (page->local_messages_button) {
        lv_obj_remove_event_cb_with_user_data(page->local_messages_button, OnLocalMessages, page);
    }
    if (page->refresh_button) {
        lv_obj_remove_event_cb_with_user_data(page->refresh_button, OnRefresh, page);
    }
    if (page->switch_account_button) {
        lv_obj_remove_event_cb_with_user_data(page->switch_account_button, OnSwitchAccount, page);
    }
    if (page->authorization) {
        auto authorization = page->authorization;
        WorkBuddyCancelAuthorization(authorization);
    }
    page->network_hold.reset();
    if (!page->turn) {
        return;
    }
    auto turn = page->turn;
    WorkBuddyCancelTurn(turn);
}

/** @brief 刷新当前扫码会话，不切换绑定意图或删除正式凭证 */
static void OnRefresh(lv_event_t* event) {
    auto* page = static_cast<WorkbuddyScreenPage*>(lv_event_get_user_data(event));
    if (!page || page->closed || !page->authorization) {
        return;
    }
    const auto phase = page->authorization->phase.load();
    if (phase == WorkBuddyAuthPhase::Authorized || phase == WorkBuddyAuthPhase::Checking ||
        phase == WorkBuddyAuthPhase::Authorizing) {
        return;
    }
    StartAuthorization(page, true, page->authorization->switch_account.load());
    UpdateInputActions(page);
}

/**
 * @brief 用户主动获取换绑二维码，停止旧回复读取并清理录音收尾，不删除助理任务。
 * @param event 顶部重新绑定按钮的点击事件。
 * @return 无；忙碌时忽略迟到点击，新账号成功授权后才替换设备凭证。
 */
static void OnConfirmSwitch(lv_event_t* event) {
    auto* page = static_cast<WorkbuddyScreenPage*>(lv_event_get_user_data(event));
    if (!page || page->closed || !page->authorization || IsBusy(page) || page->authorization->running.load()) {
        return;
    }
    lv_obj_add_flag(page->binding_dialog, LV_OBJ_FLAG_HIDDEN);
    page->touch_suppress_click = true;
    EndTouchPress(page);
    if (page->turn) {
        auto previous = page->turn;
        WorkBuddyCancelTurn(previous);
    }
    StartAuthorization(page, true, true);
    UpdateInputActions(page);
}

static void OnCloseOverlay(lv_event_t* event) {
    auto* page = static_cast<WorkbuddyScreenPage*>(lv_event_get_user_data(event));
    if (!page || page->closed) {
        return;
    }
    lv_obj_add_flag(page->menu_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(page->binding_dialog, LV_OBJ_FLAG_HIDDEN);
}

static void OnToggleMenu(lv_event_t* event) {
    auto* page = static_cast<WorkbuddyScreenPage*>(lv_event_get_user_data(event));
    if (!page || page->closed || IsBusy(page)) {
        return;
    }
    EndTouchPress(page);
    if (lv_obj_has_flag(page->menu_overlay, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(page->menu_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(page->menu_overlay);
    } else {
        lv_obj_add_flag(page->menu_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void OnSwitchAccount(lv_event_t* event) {
    auto* page = static_cast<WorkbuddyScreenPage*>(lv_event_get_user_data(event));
    if (!page || page->closed || IsBusy(page)) {
        return;
    }
    lv_obj_add_flag(page->menu_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(page->binding_dialog, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(page->binding_dialog);
}

/** @brief BOOT按下先等待长按阈值，短按不启动麦克风；@return 已消费按键。 */
static bool OnBootDown() {
    return true;
}

/** @brief BOOT满500毫秒后投递录音意图，与屏触共用长按语义；@return 已消费按键。 */
static bool OnBootLong() {
    QueueInput(WorkbuddyScreenInputCommand::BootLong);
    return true;
}

/** @brief BOOT 松开只投递结束意图，不跨线程读取对话指针；@return 已消费按键。 */
static bool OnBootUp() {
    QueueInput(WorkbuddyScreenInputCommand::BootUp);
    return true;
}

/** @brief 消费BOOT单击和双击，避免触发其它应用；@return true。 */
static bool OnBootConsumed() {
    return true;
}

/**
 * @brief 输入任务只投递翻页或返回意图，避免与 LVGL 绘制并发修改控件树。
 * @param key 统一虚拟键名。
 * @return 当前页面是否处理此按键。
 */
static bool OnKey(const char* key) {
    if (!key) {
        return false;
    }
    if (std::strcmp(key, "vk_home") == 0) {
        QueueInput(WorkbuddyScreenInputCommand::Home);
        return true;
    }
    if (std::strcmp(key, "vk_prev") == 0) {
        QueueInput(WorkbuddyScreenInputCommand::Back);
        return true;
    }
    if (std::strcmp(key, "vk_next") == 0) {
        QueueInput(WorkbuddyScreenInputCommand::Next);
        return true;
    }
    return false;
}

/** @brief 连翻只投递步长，控件访问和边界判断仍在 LVGL 线程执行。 */
static bool QueuePageRepeat(int page_delta) {
    if (s_active_input_generation.load(std::memory_order_acquire) == 0) {
        return false;
    }
    QueueInput(WorkbuddyScreenInputCommand::Repeat, page_delta);
    return true;
}

// 对话正文接管纯翻页，首尾边界不触发返回应用首页。
static bool OnPageKey(const char* key, bool long_press) {
    auto* page = s_current_page;
    if (!page || page->closed || lv_obj_has_flag(page->content, LV_OBJ_FLAG_HIDDEN) ||
        !lv_obj_has_flag(page->menu_overlay, LV_OBJ_FLAG_HIDDEN) ||
        !lv_obj_has_flag(page->binding_dialog, LV_OBJ_FLAG_HIDDEN)) {
        return false;
    }
    if (long_press) {
        VkPageRepeatTryStart(key, QueuePageRepeat);
    } else {
        QueueInput(std::strcmp(key, "vk_prev") == 0 ? WorkbuddyScreenInputCommand::Previous :
                                                      WorkbuddyScreenInputCommand::Next);
    }
    return true;
}

/** @brief 复用通用长按连翻节拍，每秒按方向切换十页。 */
static bool OnKeyLongPress(const char* key) {
    if (key && std::strcmp(key, "vk_home") == 0) {
        return OnKey(key);
    }
    return VkPageRepeatTryStart(key, QueuePageRepeat);
}

/** @brief 盖板键松开后停止连翻。 */
static bool OnKeyPressUp(const char* key) {
    return VkPageRepeatOnPressUp(key);
}

/**
 * @brief 同百问AI在长按阈值到达时开始录音，已松手或正在发送时不会迟到开启。
 * @param timer 页面持有的一次性LVGL定时器，触发后由LVGL自动删除。
 * @return 无；先清句柄，避免松手和离页重复释放定时器。
 */
static void ArmTouchRecording(lv_timer_t* timer) {
    auto* page = static_cast<WorkbuddyScreenPage*>(lv_timer_get_user_data(timer));
    page->touch_arm_timer = nullptr;
    if (page->closed || !page->touch_active || page->touch_suppress_click) {
        return;
    }
    if (!TouchUiFingerIsDown()) {
        page->touch_suppress_click = true;
        EndTouchPress(page);
        return;
    }
    // 长按即消费本次点击，即使授权状态刚变化，也不能在松手时误翻页。
    page->touch_suppress_click = true;
    page->touch_ptt_held = true;
    Begin(page);
}

/**
 * @brief 全屏短点分区翻页、长按录音；明显移动取消待触发长按，长按后的点击不翻页。
 * @param event 全屏透明触摸层的按下、移动、松手、失压或点击事件。
 * @return 无；左三分之一上一页，右三分之一区域下一页，中间区域短点不翻页。
 */
static void OnContentTouch(lv_event_t* event) {
    auto* page = static_cast<WorkbuddyScreenPage*>(lv_event_get_user_data(event));
    if (!page || page->closed || lv_event_get_target(event) != page->touch_hit) {
        return;
    }
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_CLICKED) {
        if (!page->touch_suppress_click) {
            if (page->touch_start.x < LV_HOR_RES / 3) {
                QueueInput(WorkbuddyScreenInputCommand::Previous);
            } else if (page->touch_start.x > LV_HOR_RES * 2 / 3) {
                QueueInput(WorkbuddyScreenInputCommand::Next);
            }
        }
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!page->touch_active || code == LV_EVENT_PRESS_LOST ||
            esp_timer_get_time() - page->touch_pressed_us >= static_cast<int64_t>(kBootLongPressMs) * 1000) {
            page->touch_suppress_click = true;
        }
        EndTouchPress(page);
        return;
    }
    auto* input = lv_event_get_indev(event);
    if (!input || lv_indev_get_type(input) != LV_INDEV_TYPE_POINTER) {
        return;
    }
    lv_indev_get_point(input, &page->touch_last);
    if (code == LV_EVENT_PRESSED) {
        CancelTouchArm(page);
        page->touch_start = page->touch_last;
        page->touch_active = true;
        page->touch_suppress_click = false;
        page->touch_pressed_us = esp_timer_get_time();
        const WorkBuddyPhase phase = page->turn ? page->turn->phase.load() : WorkBuddyPhase::Idle;
        const bool can_record =
            page->authorization && page->authorization->phase.load() == WorkBuddyAuthPhase::Authorized &&
            !page->reauthorization_required && lv_obj_has_flag(page->menu_overlay, LV_OBJ_FLAG_HIDDEN) &&
            lv_obj_has_flag(page->binding_dialog, LV_OBJ_FLAG_HIDDEN) && phase != WorkBuddyPhase::Recognizing &&
            phase != WorkBuddyPhase::Sending && phase != WorkBuddyPhase::Waiting;
        if (can_record) {
            page->touch_arm_timer = lv_timer_create(ArmTouchRecording, kBootLongPressMs, page);
            if (page->touch_arm_timer) {
                lv_timer_set_repeat_count(page->touch_arm_timer, 1);
            } else {
                page->touch_suppress_click = true;
            }
        }
    } else if (code == LV_EVENT_PRESSING && page->touch_active && !page->touch_ptt_held) {
        const int dx = page->touch_last.x - page->touch_start.x;
        const int dy = page->touch_last.y - page->touch_start.y;
        if (std::abs(dx) > kTouchSlop || std::abs(dy) > kTouchSlop) {
            CancelTouchArm(page);
            page->touch_suppress_click = true;
        }
    }
}

/**
 * @brief 仅由LVGL定时器消费实体键输入，合并翻页并过滤已经松开的迟到长按。
 * @param page 当前有效页面；旧页面代号的输入直接丢弃。
 * @return 已处理返回动作时为 true，调用方应结束本次界面更新。
 */
static bool ProcessInput(WorkbuddyScreenPage* page) {
    WorkbuddyScreenInputMessage message{};
    const auto return_to_apps = [page]() {
        const auto go = [](void*) { ScreenGoHome(); };
        if (!ScreenLvAsyncUrgent(go) && !ScreenLvAsync(go)) {
            return false;
        }
        Cancel(page);
        return true;
    };
    if (s_input_overflow_generation.exchange(0, std::memory_order_acq_rel) == page->input_generation) {
        // 固定上限保证本次消费不会被持续输入占住；溢出时优先结束可能仍在采集的录音。
        for (UBaseType_t count = 0; count < kInputQueueLength; ++count) {
            if (xQueueReceive(s_input_queue, &message, 0) != pdPASS) {
                break;
            }
        }
        page->touch_suppress_click = true;
        EndTouchPress(page);
        if (page->turn) {
            page->turn->held.store(false);
        }
        VkPageRepeatStop();
        ESP_LOGW(kTag, "input queue full; pending keys discarded and recording released");
        return false;
    }
    int scroll_steps = 0;
    bool repeat_step = false;
    for (UBaseType_t count = 0; count < kInputQueueLength; ++count) {
        if (xQueueReceive(s_input_queue, &message, 0) != pdPASS) {
            break;
        }
        if (message.generation != page->input_generation) {
            continue;
        }
        const bool back = message.command == WorkbuddyScreenInputCommand::Back ||
                          message.command == WorkbuddyScreenInputCommand::Home;
        // 队列中的旧输入可能晚于弹窗到达；松手仍交给录音收尾，其余复用公共窗口策略。
        if (message.command != WorkbuddyScreenInputCommand::BootUp &&
            UiModal::HandleInput(back ? UiModalInput::Back : UiModalInput::Other)) {
            scroll_steps = 0;
            continue;
        }
        switch (message.command) {
            case WorkbuddyScreenInputCommand::Home:
                if (return_to_apps()) {
                    return true;
                }
                break;
            case WorkbuddyScreenInputCommand::Back:
                if (scroll_steps != 0) {
                    TurnTextPage(page, scroll_steps);
                    scroll_steps = 0;
                }
                if (!TurnTextPage(page, -1) && return_to_apps()) {
                    return true;
                }
                break;
            case WorkbuddyScreenInputCommand::Previous:
                --scroll_steps;
                break;
            case WorkbuddyScreenInputCommand::Next:
                ++scroll_steps;
                break;
            case WorkbuddyScreenInputCommand::BootLong:
                if (BootKey_IsHeld()) {
                    Begin(page);
                }
                break;
            case WorkbuddyScreenInputCommand::BootUp:
                Release(page);
                break;
            case WorkbuddyScreenInputCommand::Repeat:
                if (VkPageRepeatIsActive()) {
                    scroll_steps += message.page_delta;
                    repeat_step = true;
                }
                break;
        }
    }
    if (scroll_steps != 0 && !TurnTextPage(page, scroll_steps) && repeat_step) {
        VkPageRepeatStop();
    }
    return false;
}

/**
 * @brief 在 LVGL 线程渲染授权阶段，二维码只编码登录网址。
 * @param page 当前有效页面。
 * @return 已授权时返回 true，允许继续渲染语音内容。
 */
static bool PaintAuthorization(WorkbuddyScreenPage* page) {
    auto authorization = page->authorization;
    if (!authorization) {
        return false;
    }
    WorkBuddyAuthPhase phase;
    std::string message;
    std::string url;
    {
        std::lock_guard<std::mutex> lock(authorization->view_mutex);
        phase = authorization->phase.load();
        if (page->painted_auth_revision == authorization->revision) {
            return phase == WorkBuddyAuthPhase::Authorized;
        }
        page->painted_auth_revision = authorization->revision;
        message = authorization->message;
        url = authorization->verification_url;
    }
    const bool qr_visible = phase == WorkBuddyAuthPhase::Waiting ||
                            phase == WorkBuddyAuthPhase::Authorizing ||
                            phase == WorkBuddyAuthPhase::Expired || phase == WorkBuddyAuthPhase::Error;
    if (qr_visible) {
        lv_obj_add_flag(page->content, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(page->qr_panel, LV_OBJ_FLAG_HIDDEN);
        const int seconds = authorization->expires_in.load();
        const std::string validity = seconds % 60 == 0 ? std::to_string(seconds / 60) + LangString_WB_MINUTE_SUFFIX
                                                       : std::to_string(seconds) + LangString_WB_SECOND_SUFFIX;
        const std::string hint =
            phase == WorkBuddyAuthPhase::Expired || phase == WorkBuddyAuthPhase::Error
                ? message
                : (phase == WorkBuddyAuthPhase::Authorizing ? LangString_WB_CONFIRM_PHONE
                                                                  : LangString_WB_QR_VALIDITY_PREFIX + validity);
        SetLabelIfChanged(page->qr_hint, hint.c_str());
        if (url.empty()) {
            lv_obj_add_flag(page->qr_frame, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(page->qr_frame, LV_OBJ_FLAG_HIDDEN);
        }
        if (page->painted_url != url && !url.empty()) {
            if (lv_qrcode_update(page->qr, url.data(), url.size()) != LV_RESULT_OK) {
                lv_obj_add_flag(page->qr_frame, LV_OBJ_FLAG_HIDDEN);
                SetLabelIfChanged(page->qr_hint, LangString_WB_QR_FAILED);
                message = LangString_WB_QR_DISPLAY_FAILED;
            } else {
                page->painted_url = url;
            }
        }
    } else {
        lv_obj_add_flag(page->qr_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(page->content, LV_OBJ_FLAG_HIDDEN);
        if (phase == WorkBuddyAuthPhase::Authorized) {
            page->reauthorization_required = authorization->reauthorization_required.load();
            if (authorization->binding_changed.exchange(false)) {
                page->chat.Clear();
            }
            ShowIntro(page, "");
            if (page->reauthorization_required) {
                ScreenShowToast(message.c_str());
            }
        } else {
            ShowIntro(page,
                      authorization->switch_account.load() ? LangString_WB_RELINK_WAIT : LangString_WB_LINK_CHECKING);
        }
    }
    UpdateInputActions(page);
    return phase == WorkBuddyAuthPhase::Authorized;
}

/**
 * @brief 低频检查线程状态，正文仅变化时刷新，倾听波形由独立小区域定时器处理。
 * @param timer 页面拥有的 LVGL 定时器。
 * @return 无；同步录音可见状态并合并正文更新。
 */
static void OnTick(WorkbuddyScreenPage* page) {
    if (page->closed || s_standby) {
        return;
    }
    // 绘制导致LVGL松手事件迟到时，以触摸驱动的实际状态兜底，不让录音多持续一轮。
    if (page->touch_active && !TouchUiFingerIsDown()) {
        // 短点仍交给CLICKED翻页；已臂听或达到长按阈值时必须抑制该点击。
        if (page->touch_ptt_held ||
            esp_timer_get_time() - page->touch_pressed_us >= static_cast<int64_t>(kBootLongPressMs) * 1000) {
            page->touch_suppress_click = true;
            EndTouchPress(page);
        }
    }
    if (ProcessInput(page)) {
        return;
    }
    if (page->authorization_retry_at_us != 0 && esp_timer_get_time() >= page->authorization_retry_at_us) {
        TryStartAuthorization(page);
    }
    SetListeningWave(page, IsListening(page));
    const bool authorized = PaintAuthorization(page);
    if (!page->authorization) {
        return;
    }
    SetLabelIfChanged(page->connection_label, ConnectionText(page->authorization->connection.load()));
    if (authorized) {
        page->reauthorization_required = page->authorization->reauthorization_required.load();
    }
    UpdateInputActions(page);
    if (!authorized) {
        return;
    }
    if (!page->turn) {
        return;
    }
    auto turn = page->turn;
    const WorkBuddyPhase phase = turn->phase.load();
    if (((phase == WorkBuddyPhase::Preparing || phase == WorkBuddyPhase::Recording) &&
         !turn->held.load()) ||
        turn->finish_requested.load()) {
        WorkBuddyFinishTurn(turn);
    }
    const uint32_t revision = turn->result_revision.load();
    const bool same_turn = page->painted_turn.lock() == turn;
    if (same_turn && phase == page->painted_phase && revision == page->painted_result_revision) {
        return;
    }
    const bool partial_update =
        same_turn && phase == page->painted_phase &&
        (phase == WorkBuddyPhase::Recording || phase == WorkBuddyPhase::Recognizing);
    if (partial_update && esp_timer_get_time() - page->last_result_paint_us < kMinTranscriptPaintUs) {
        return;
    }
    const bool ongoing_reply = same_turn && phase == WorkBuddyPhase::Done &&
                               page->painted_phase == WorkBuddyPhase::Done && turn->local_accepted.load();
    if (ongoing_reply && esp_timer_get_time() - page->last_result_paint_us < kMinResultPaintUs) {
        return;
    }
    WorkBuddyTurnResult result;
    {
        std::unique_lock<std::mutex> lock(turn->result_mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            return;
        }
        if (turn->is_local_refresh && phase == WorkBuddyPhase::Done) {
            result = std::move(turn->result);
        } else {
            result = turn->result;
        }
    }
    const WorkBuddyPhase previous_phase = page->painted_phase;
    page->painted_turn = turn;
    page->painted_phase = phase;
    page->painted_result_revision = revision;
    if (result.authorization_required || result.permission_required) {
        page->authorization->reauthorization_required.store(true);
        page->authorization->connection.store(WorkBuddyConnectionState::Failed);
    }
    page->reauthorization_required = page->authorization->reauthorization_required.load();
    UpdateInputActions(page);
    const bool final_transcript = turn->transcript_final.load();
    if (!turn->is_local_refresh && !result.transcript.empty()) {
        const bool follow = !final_transcript || !page->final_transcript_painted;
        page->chat.SetMessage(turn->ui_id, true, result.transcript, follow);
        if (final_transcript) {
            page->final_transcript_painted = true;
        }
    }
    if (phase == WorkBuddyPhase::Preparing && !same_turn) {
        ScreenShowToast(LangString_ASSISTANT_CONNECTING_HOLD);
    }
    if (phase == WorkBuddyPhase::Recognizing && (!same_turn || phase != previous_phase)) {
        ScreenShowToast(LangString_WB_RECOGNIZING);
    }
    if (phase == WorkBuddyPhase::Sending && (!same_turn || phase != previous_phase)) {
        ScreenShowToast(turn->is_local_refresh ? LangString_WB_SYNCING : LangString_WB_SENDING);
    }
    std::string reply;
    if (phase == WorkBuddyPhase::Waiting) {
        reply = LangString_WB_PROCESSING;
    } else if (phase == WorkBuddyPhase::Done && !turn->is_local_refresh) {
        reply = result.reply;
    } else if (phase == WorkBuddyPhase::Error) {
        reply = result.error;
    }
    if (!reply.empty()) {
        page->chat.SetMessage(turn->ui_id, false, reply, turn->is_local_refresh);
    }
    if (phase == WorkBuddyPhase::Done && turn->is_local_refresh &&
        (!same_turn || previous_phase != WorkBuddyPhase::Done)) {
        if (result.messages.empty()) {
            ScreenShowToast(LangString_WB_NO_MESSAGES);
        } else {
            ScreenShowToast(page->chat.ReplaceMessages(result.messages) ? LangString_WB_SYNCED
                                                                        : LangString_WB_LAYOUT_FAILED);
        }
    }
    page->last_result_paint_us = esp_timer_get_time();
}

/** @brief 统一清理页面、定时器和异步任务引用；@param event 页面卸载或删除事件。 */
static void OnLifecycle(lv_event_t* event) {
    auto* page = static_cast<WorkbuddyScreenPage*>(lv_event_get_user_data(event));
    Cancel(page);
    if (s_current_page == page) {
        s_current_page = nullptr;
    }
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        delete page;
    }
}

/**
 * @brief 创建使用统一中文字体的多行文本标签。
 * @param parent 父控件。
 * @param text 初始文案。
 * @param width 标签宽度。
 * @return 标签对象。
 */
static lv_obj_t* Label(lv_obj_t* parent, const char* text, int width) {
    auto* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, UiFont(), 0);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    return label;
}

/**
 * @brief 统一顶部功能按钮为黑白样式，禁用主题动画并保持按下时文字可读。
 * @param button 需要设置的按钮。
 * @param dark 是否使用黑底白字作为主要动作。
 * @return 无。
 */
static void StyleActionButton(lv_obj_t* button, bool dark) {
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(button, dark ? UiColorForeground() : UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(button, dark ? UiColorBackground() : UiColorForeground(), 0);
    lv_obj_set_style_border_color(button, UiColorForeground(), 0);
    lv_obj_set_style_border_width(button, 2, 0);
    lv_obj_set_style_radius(button, UiSx(8), 0);
    lv_obj_set_style_text_font(button, UiFont(), 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 4, 0);
    lv_obj_set_style_bg_color(button, dark ? UiColorBackground() : UiColorForeground(), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(button, dark ? UiColorForeground() : UiColorBackground(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, UiColorBackground(), LV_STATE_DISABLED);
    lv_obj_set_style_text_color(button, UiColorForeground(), LV_STATE_DISABLED);
    lv_obj_set_style_anim_duration(button, 0, 0);
    lv_obj_set_style_transition(button, &kNoTransition, 0);
}

/** @brief 创建透明菜单层和确认层，仅弹窗自身绘制背景，底层对话保持原位。 */
static void CreateMenus(WorkbuddyScreenPage* page, lv_coord_t bar_height) {
    auto create_overlay = [page]() {
        auto* overlay = lv_obj_create(page->root);
        lv_obj_remove_style_all(overlay);
        lv_obj_set_size(overlay, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_bg_color(overlay, UiColorForeground(), 0);
        lv_obj_set_style_bg_opa(overlay, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(overlay, OnCloseOverlay, LV_EVENT_CLICKED, page);
        lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
        return overlay;
    };
    page->menu_overlay = create_overlay();
    s_menu_modal.Bind(page->menu_overlay, [](void* context) {
        auto* page = static_cast<WorkbuddyScreenPage*>(context);
        lv_obj_add_flag(page->menu_overlay, LV_OBJ_FLAG_HIDDEN);
    }, page);
    auto* list = lv_obj_create(page->menu_overlay);
    lv_point_t sync_size, relink_size;
    lv_text_get_size(&sync_size, LangString_WB_SYNC_MESSAGES, UiFont(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&relink_size, LangString_WB_RELINK, UiFont(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int menu_width =
        std::min<int>(LV_HOR_RES - UiSx(32),
                      std::max<int>(UiSx(140), std::max(sync_size.x, relink_size.x) + 30 + UiSx(9) + 2 * UiSx(5) + 10));
    lv_obj_set_size(list, menu_width, LV_SIZE_CONTENT);
    lv_obj_align(list, LV_ALIGN_TOP_RIGHT, -UiSx(16), bar_height + UiSy(54));
    lv_obj_set_style_bg_color(list, UiColorBackground(), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(list, 1, 0);
    lv_obj_set_style_border_color(list, UiColorForeground(), 0);
    lv_obj_set_style_radius(list, UiSx(7), 0);
    lv_obj_set_style_pad_all(list, UiSx(5), 0);
    lv_obj_set_style_pad_row(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    auto action = [list](const char* text, const char* symbol) {
        auto* button = lv_button_create(list);
        StyleActionButton(button, false);
        lv_obj_set_size(button, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(button, std::max<int>(UiSy(49), ScreenTextHeight() + UiSy(16)), 0);
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(button, UiSx(9), 0);
        auto* icon = Label(button, symbol, 30);
        lv_obj_set_style_text_font(icon, &font_awesome_30_1, 0);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        auto* label = Label(button, text, LV_SIZE_CONTENT);
        lv_obj_set_width(label, 0);
        lv_obj_set_flex_grow(label, 1);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        return button;
    };
    page->local_messages_button = action(LangString_WB_SYNC_MESSAGES, FONT_AWESOME_ARROWS_ROTATE);
    page->action_label = lv_obj_get_child(page->local_messages_button, 1);
    lv_obj_add_event_cb(page->local_messages_button, OnLocalMessages, LV_EVENT_CLICKED, page);
    auto* separator = lv_obj_create(list);
    lv_obj_remove_style_all(separator);
    lv_obj_set_size(separator, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(separator, UiColorForeground(), 0);
    lv_obj_set_style_bg_opa(separator, LV_OPA_COVER, 0);
    lv_obj_remove_flag(separator, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    page->switch_account_button = action(LangString_WB_RELINK, FONT_AWESOME_LINK);
    lv_obj_add_event_cb(page->switch_account_button, OnSwitchAccount, LV_EVENT_CLICKED, page);

    page->binding_dialog = create_overlay();
    s_binding_modal.Bind(page->binding_dialog, [](void* context) {
        auto* page = static_cast<WorkbuddyScreenPage*>(context);
        lv_obj_add_flag(page->binding_dialog, LV_OBJ_FLAG_HIDDEN);
    }, page);
    auto* dialog = lv_obj_create(page->binding_dialog);
    lv_obj_set_size(dialog, UiSx(420), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(dialog, UiSy(20), 0);
    lv_obj_set_flex_flow(dialog, LV_FLEX_FLOW_COLUMN);
    ScreenStyleDialogCard(dialog, UiSx(20));
    auto* heading = Label(dialog, LangString_WB_RELINK_TITLE, LV_PCT(100));
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    Label(dialog, LangString_WB_RELINK_WARNING, LV_PCT(100));
    auto* row = lv_obj_create(dialog);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, UiSx(12), 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 2; ++i) {
        auto* button = lv_button_create(row);
        StyleActionButton(button, i == 1);
        lv_obj_set_size(button, 0, UiSy(48));
        lv_obj_set_flex_grow(button, 1);
        auto* label = Label(button, i == 0 ? LangString_WB_RELINK_CANCEL : LangString_WB_RELINK_CONTINUE, LV_PCT(100));
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        ScreenLayoutTextButton(button, label, UiSy(48));
        lv_obj_add_event_cb(button, i == 0 ? OnCloseOverlay : OnConfirmSwitch, LV_EVENT_CLICKED, page);
    }
    lv_obj_center(dialog);
}

/**
 * @brief 创建正文透明触摸层，标题状态不响应翻页或录音，菜单保持可点击。
 * @param page 已创建正文和顶部功能按钮的页面。
 * @return 无；热区自身没有可见边框、按钮或按压动画。
 */
static void CreateTouchLayer(WorkbuddyScreenPage* page) {
    page->touch_hit = lv_obj_create(page->root);
    lv_obj_remove_style_all(page->touch_hit);
    lv_obj_set_style_bg_opa(page->touch_hit, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(page->touch_hit, 0, lv_obj_get_y(page->content));
    lv_obj_set_size(page->touch_hit, LV_HOR_RES, lv_obj_get_height(page->content));
    lv_obj_add_flag(page->touch_hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(page->touch_hit,
                       static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM |
                                                  LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_GESTURE_BUBBLE));
    HapticAttachClick(page->touch_hit);
    lv_obj_add_event_cb(page->touch_hit, OnContentTouch, LV_EVENT_PRESSED, page);
    lv_obj_add_event_cb(page->touch_hit, OnContentTouch, LV_EVENT_PRESSING, page);
    lv_obj_add_event_cb(page->touch_hit, OnContentTouch, LV_EVENT_RELEASED, page);
    lv_obj_add_event_cb(page->touch_hit, OnContentTouch, LV_EVENT_PRESS_LOST, page);
    lv_obj_add_event_cb(page->touch_hit, OnContentTouch, LV_EVENT_CLICKED, page);
    lv_obj_move_foreground(page->menu_button);
    lv_obj_move_foreground(page->qr_panel);
}

/**
 * @brief 创建黑白整页对话界面，支持屏幕短点翻页、500毫秒长按录音及实体按键。
 * @return 按 470 可绘区域缩放的独立 LVGL 屏幕。
 */
lv_obj_t* WorkBuddyScreen::Create() {
    auto* page = new WorkbuddyScreenPage();
    s_current_page = page;
    s_standby = false;
    if (!s_input_queue) {
        s_input_queue = xQueueCreateStatic(kInputQueueLength, sizeof(WorkbuddyScreenInputMessage),
                                           s_input_queue_storage, &s_input_queue_control);
    }
    xQueueReset(s_input_queue);
    if (++s_next_input_generation == 0) {
        ++s_next_input_generation;
    }
    page->input_generation = s_next_input_generation;
    ScreenSetIsHome(false);
    page->root = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(page->root, UiColorBackground(), 0);
    lv_obj_set_style_text_color(page->root, UiColorForeground(), 0);
    lv_obj_set_style_text_font(page->root, UiFont(), 0);
    lv_obj_remove_flag(page->root, LV_OBJ_FLAG_SCROLLABLE);
    auto bar = ScreenCreateStatusBar(page->root);
    CreateListeningWave(page, bar);
    lv_obj_set_style_pad_all(page->root, 0, 0);
    auto* title_group = lv_obj_create(page->root);
    lv_obj_remove_style_all(title_group);
    lv_obj_set_size(title_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(title_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(title_group, 8, 0);
    lv_obj_align(title_group, LV_ALIGN_TOP_MID, 0, bar.height + UiSy(18));
    lv_obj_remove_flag(title_group, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    page->title = Label(title_group, "WorkBuddy", LV_SIZE_CONTENT);
    const auto* title_font = UiFont(UiFontRole::TitleEmphasis);
    lv_obj_set_style_text_font(page->title, title_font ? title_font : UiFont(), 0);
    lv_obj_remove_flag(page->title, LV_OBJ_FLAG_CLICKABLE);
    page->connection_label =
        Label(title_group, ConnectionText(WorkBuddyConnectionState::Unknown), LV_SIZE_CONTENT);
    const auto* status_font = UiFont(UiFontRole::Caption);
    lv_obj_set_style_text_font(page->connection_label, status_font ? status_font : UiFont(), 0);
    lv_obj_remove_flag(page->connection_label, LV_OBJ_FLAG_CLICKABLE);
    page->menu_button = lv_button_create(page->root);
    StyleActionButton(page->menu_button, false);
    lv_obj_set_style_border_width(page->menu_button, 0, 0);
    lv_obj_set_style_bg_color(page->menu_button, UiColorBackground(), LV_STATE_PRESSED);
    lv_obj_set_pos(page->menu_button, LV_HOR_RES - UiSx(44), bar.height + UiSy(15));
    lv_obj_set_size(page->menu_button, UiSx(28), UiSy(34));
    for (int i = 0; i < 3; ++i) {
        auto* dot = lv_obj_create(page->menu_button);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 4, 4);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, UiColorForeground(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_align(dot, LV_ALIGN_CENTER, 0, (i - 1) * 9);
        lv_obj_remove_flag(dot, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    }
    lv_obj_add_event_cb(page->menu_button, OnToggleMenu, LV_EVENT_CLICKED, page);
    page->content = lv_obj_create(page->root);
    lv_obj_set_pos(page->content, UiSx(0), bar.height + UiSy(64));
    lv_obj_set_size(page->content, LV_HOR_RES, LV_VER_RES - bar.height - UiSy(64));
    lv_obj_set_style_bg_color(page->content, UiColorBackground(), 0);
    lv_obj_set_style_radius(page->content, 0, 0);
    lv_obj_set_style_border_width(page->content, 0, 0);
    lv_obj_set_style_pad_all(page->content, kContentPad, 0);
    lv_obj_set_scrollbar_mode(page->content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(page->content, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM |
                                                                 LV_OBJ_FLAG_SCROLL_ELASTIC));
    lv_obj_remove_flag(page->content, LV_OBJ_FLAG_CLICKABLE);
    page->chat.Create(page->content);
    ShowIntro(page, LangString_WB_LINK_CHECKING);
    page->qr_panel = lv_obj_create(page->root);
    lv_obj_set_pos(page->qr_panel, UiSx(16), bar.height + UiSy(64));
    lv_obj_set_size(page->qr_panel, LV_HOR_RES - UiSx(32), LV_VER_RES - bar.height - UiSy(128));
    lv_obj_set_style_bg_color(page->qr_panel, UiColorBackground(), 0);
    lv_obj_set_style_border_width(page->qr_panel, 0, 0);
    lv_obj_set_style_pad_all(page->qr_panel, 0, 0);
    lv_obj_remove_flag(page->qr_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(page->qr_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page->qr_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(page->qr_panel, UiSy(16), 0);
    lv_obj_update_layout(page->qr_panel);
    const int qr_side =
        std::max<int>(112, std::min<int>({UiSx(240), lv_obj_get_content_width(page->qr_panel) - 2 * kUiQrFrameInset,
                                          lv_obj_get_content_height(page->qr_panel) - 4 * UiFont()->line_height -
                                              UiSy(80) - 2 * kUiQrFrameInset}));
    page->qr = UiCreateQrCode(page->qr_panel, qr_side);
    page->qr_frame = lv_obj_get_parent(page->qr);
    page->qr_hint = Label(page->qr_panel, LangString_WB_SCAN_LINK, LV_HOR_RES - UiSx(48));
    lv_obj_set_style_text_align(page->qr_hint, LV_TEXT_ALIGN_CENTER, 0);
    page->refresh_button = lv_button_create(page->qr_panel);
    StyleActionButton(page->refresh_button, false);
    lv_obj_set_style_border_width(page->refresh_button, 0, LV_STATE_DISABLED);
    lv_obj_set_style_opa(page->refresh_button, LV_OPA_COVER, LV_STATE_DISABLED);
    lv_obj_add_event_cb(page->refresh_button, ScreenDrawDisabledBorder, LV_EVENT_DRAW_MAIN_END, nullptr);
    lv_obj_set_size(page->refresh_button, UiSx(112), UiSy(48));
    page->refresh_label = Label(page->refresh_button, LangString_CLOUD_REFRESH, UiSx(96));
    ScreenLayoutTextButton(page->refresh_button, page->refresh_label, UiSy(48));
    lv_obj_add_event_cb(page->refresh_button, OnRefresh, LV_EVENT_CLICKED, page);
    lv_obj_add_flag(page->qr_panel, LV_OBJ_FLAG_HIDDEN);
    CreateMenus(page, bar.height);
    CreateTouchLayer(page);
    lv_obj_add_event_cb(page->root, OnLifecycle, LV_EVENT_SCREEN_UNLOADED, page);
    lv_obj_add_event_cb(page->root, OnLifecycle, LV_EVENT_DELETE, page);
    page->network_hold.emplace(PowerNeed::ForegroundNetwork);
    StartAuthorization(page, false);
    VkKeyScreenDesc keys;
    keys.factory = WorkBuddyScreen::Create;
    keys.on_key = OnKey;
    keys.on_page_key = OnPageKey;
    keys.on_key_long_press = OnKeyLongPress;
    keys.on_key_press_up = OnKeyPressUp;
    keys.on_boot_press_down = OnBootDown;
    keys.on_boot_press_up = OnBootUp;
    keys.on_boot_click = OnBootConsumed;
    keys.on_boot_long_press = OnBootLong;
    keys.on_boot_double_click = OnBootConsumed;
    VkKey_AttachScreen(page->root, "WorkBuddyScreen", keys);
    // 控件和固定队列准备完成后才接收输入；离页后同一代号立即失效。
    s_active_input_generation.store(page->input_generation, std::memory_order_release);
    s_page_lifecycle.Bind(page->root, {[] {
        if (s_current_page) {
            OnTick(s_current_page);
        }
    }, [](UiPagePowerEvent event) {
        if (event == UiPagePowerEvent::PauseApplications || event == UiPagePowerEvent::ResumeApplications) {
            WorkBuddyScreen::SetStandby(event == UiPagePowerEvent::PauseApplications);
        }
    }, nullptr, 150});
    return page->root;
}

/**
 * @brief 供显示驱动判断状态栏中央是否由WorkBuddy倾听动画占用。
 * @return 是否存在可见波形；原子读取不访问LVGL对象，可跨任务调用。
 */
bool WorkBuddyScreen::IsPttWaveVisible() {
    return s_wave_generation.load() != 0;
}

void WorkBuddyScreen::SetStandby(bool standby) {
    auto* page = s_current_page;
    if (!page || page->closed || s_standby == standby) {
        return;
    }
    s_standby = standby;
    if (standby) {
        VkPageRepeatStop();
        page->authorization_retry_at_us = 0;
        s_active_input_generation.store(0, std::memory_order_release);
        if (page->turn) {
            auto turn = page->turn;
            WorkBuddyCancelTurn(turn);
        }
        EndTouchPress(page);
        SetListeningWave(page, false);
        if (page->turn && IsBusy(page)) {
            if (!page->turn->is_local_refresh) {
                page->chat.SetMessage(page->turn->ui_id, false, LangString_WB_STOPPED_HINT, false);
            }
            page->turn.reset();
            page->painted_turn.reset();
            page->painted_phase = WorkBuddyPhase::Idle;
            page->painted_result_revision = 0;
            ShowIntro(page, "");
            ScreenShowToast(LangString_WB_STOPPED);
        }
        if (page->authorization) {
            auto authorization = page->authorization;
            WorkBuddyCancelAuthorization(authorization);
            page->authorization.reset();
        }
        page->network_hold.reset();
    } else {
        page->network_hold.emplace(PowerNeed::ForegroundNetwork);
        xQueueReset(s_input_queue);
        s_active_input_generation.store(page->input_generation, std::memory_order_release);
        if (!page->authorization) {
            StartAuthorization(page, false);
        }
    }
}
