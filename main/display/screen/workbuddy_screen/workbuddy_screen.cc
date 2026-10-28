#include "workbuddy_screen.h"
#include "workbuddy_chat.h"
#include "metalio_audio.h"
#include "font_awesome.h"

#include "ui_scale.h"
#include "ui_qrcode.h"
#include "workbuddy/workbuddy_audio.h"

#include "application.h"
#include "boot_key_handler.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "lv_adapter_display.h"
#include "power_policy.h"
#include "screen_common.h"
#include "vk_key_handler.h"
#include "vk_page_repeat.h"
#include "workbuddy/workbuddy_client.h"
#include "workbuddy/workbuddy_voice_stream.h"

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
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>

LV_FONT_DECLARE(font_awesome_30_1);

namespace {
constexpr const char* kTag = "WorkBuddyScreen";
constexpr size_t kRate = 16000;
constexpr size_t kMaxSamples = kRate * 15;
constexpr size_t kMinSamples = kRate / 2;
constexpr int64_t kLocalReplyWaitUs = 180LL * 1000000;
constexpr int32_t kContentPad = UiSx(8);
constexpr int32_t kTouchSlop = UiSx(24);
constexpr int64_t kMinResultPaintUs = 1500000;
constexpr int64_t kMinTranscriptPaintUs = 1000000;
constexpr uint32_t kVoiceStackBytes = 16 * 1024;
constexpr uint32_t kPcStackBytes = 12 * 1024;
std::atomic<unsigned> s_request_workers{0}; // 最多保留一个旧请求收尾和一个当前请求
std::atomic<unsigned> s_authorization_workers{0}; // 限制快速离页重进时尚未退出的授权任务
/** @brief 沿用百问AI波形；实时正文出字时降低动画频率，仅更新顶部小区域，不表示实际音量。 */
constexpr int kWaveBars = 27;
constexpr int kWaveFrames = 16;
constexpr uint32_t kWaveFrameMs = 200;
constexpr uint8_t kWaveCurve[kWaveFrames] = {12, 16, 22, 30, 38, 44, 42, 36, 28, 20, 14, 10, 12, 18, 26, 34};
const lv_style_prop_t kNoTransitionProperties[] = {LV_STYLE_PROP_INV}; // 有效的空动画属性列表
/**
 * @brief 本页面使用的有效空过渡，描述符和属性数组均保持到程序结束。
 * 当前LVGL在状态切换时直接读取transition->props，写入nullptr会导致按压崩溃。
 */
const lv_style_transition_dsc_t kNoTransition = {
    kNoTransitionProperties, nullptr, lv_anim_path_linear, 0, 0
};
enum class Phase {
    Preparing, // 准备实时语音连接
    Recording, // 采集并上传音频
    Recognizing, // 等待最终识别结果
    Sending, // 提交指令或读取消息
    Waiting, // 等待电脑回复
    Done, // 当前结果可供阅读
    Error, // 本轮失败
    Idle // 尚无请求
};
enum class AuthPhase {
    Checking, // 查询绑定或创建二维码
    Waiting, // 等待手机扫码
    Authorizing, // 手机已提交授权
    Authorized, // 设备绑定已确认
    Expired, // 二维码已过期
    Error // 授权请求失败
};

enum class ConnectionState {
    Unknown, // 尚未获得连接状态
    Querying, // 正在查询授权或助理连接
    Failed, // 网络、授权或响应校验失败
    Online, // 服务明确报告助理在线
    Offline // 服务明确报告助理离线
};

const char* ConnectionText(ConnectionState state) {
    switch (state) {
        case ConnectionState::Querying: return "（状态查询中）";
        case ConnectionState::Failed: return "（查询失败）";
        case ConnectionState::Online: return "（在线）";
        case ConnectionState::Offline: return "（离线）";
        default: return "（未知）";
    }
}

ConnectionState ConnectionResult(const workbuddy::LocalAssistantStatus& result) {
    if (!result.ok) return ConnectionState::Failed;
    return result.online ? ConnectionState::Online : ConnectionState::Offline;
}

/** @brief 跨任务传递按键意图；输入任务不得读取页面或调用 LVGL。 */
enum class InputCommand {
    Home, // 返回应用首页
    Back, // 盖板返回键：上一页正文，无上一页时退出应用
    Previous, // 上一页正文
    Next, // 下一页正文
    BootLong, // BOOT 长按开始录音
    BootUp, // BOOT 松开结束录音
    Repeat // 盖板键长按连翻
};

/** @brief 只携带页面代号和按键，不携带可能随离页释放的控件指针。 */
struct InputMessage {
    uint32_t generation; // 输入所属页面代号
    InputCommand command; // 待处理的按键动作
    int page_delta; // 连翻步长，其余命令为 0
};

constexpr UBaseType_t kInputQueueLength = 16;
StaticQueue_t s_input_queue_control;
uint8_t s_input_queue_storage[kInputQueueLength * sizeof(InputMessage)];
QueueHandle_t s_input_queue = nullptr;
std::atomic<uint32_t> s_active_input_generation{0};
/** @brief 只向显示驱动暴露拥有波形的页面代号，不跨线程传递控件指针。 */
std::atomic<uint32_t> s_wave_generation{0};
std::atomic<uint32_t> s_input_overflow_generation{0};
uint32_t s_next_input_generation = 0;  // 仅 LVGL 线程递增，0 表示页面不接收输入。

/**
 * @brief 非阻塞投递按键，固定队列不为连续翻页分配堆内存。
 * @param command 按键动作。
 * @param page_delta 连翻步长，普通按键使用默认值 0。
 * @return 无；队列满时由界面线程停止录音并丢弃积压输入。
 */
void QueueInput(InputCommand command, int page_delta = 0) {
    const uint32_t generation = s_active_input_generation.load(std::memory_order_acquire);
    if (generation == 0) return;
    const InputMessage message{generation, command, page_delta};
    if (xQueueSend(s_input_queue, &message, 0) != pdPASS) {
        // 不能丢失 BOOT 松开后仍让录音继续，溢出统一执行安全收尾。
        s_input_overflow_generation.store(generation, std::memory_order_release);
    }
}

/** @brief 授权任务独立于录音任务，不持有 LVGL 指针，也不提前申请录音缓冲。 */
struct Authorization {
    DownloadGate gate; // 请求取消与 HTTP 绑定门闩
    std::atomic<AuthPhase> phase{AuthPhase::Checking}; // 当前处理阶段
    std::atomic<bool> refresh_requested{false}; // 请求刷新当前扫码会话
    std::atomic<bool> switch_account{false}; // 本轮是否允许更换绑定账号
    std::atomic<bool> running{false}; // 授权检查或扫码是否仍在运行
    std::atomic<int> expires_in{600}; // 当前扫码会话的有效秒数
    std::atomic<ConnectionState> connection{ConnectionState::Unknown}; // 当前连接查询状态
    std::atomic<bool> binding_changed{false}; // 扫码成功后需要清空旧账号消息
    std::atomic<bool> reauthorization_required{false}; // 当前凭证或权限需要重新绑定
    std::mutex view_mutex; // 保护授权显示快照
    uint32_t revision = 1; // 授权显示快照版本
    std::string verification_url; // 用于二维码显示的手机授权地址
    std::string message = "正在检查 WorkBuddy 授权…"; // 可直接显示的授权状态说明
};

/** @brief 一轮对话的共享状态；音频和网络任务不持有任何 LVGL 对象。 */
struct Turn {
    uint32_t ui_id = 0; // 页面内的对话轮次，仅由 LVGL 线程设置
    bool is_local_refresh = false; // 是否仅手动读取助理消息
    std::atomic<Phase> phase{Phase::Preparing}; // 当前处理阶段
    std::atomic<uint32_t> result_revision{0}; // 识别或回复结果的版本
    std::atomic<bool> local_accepted{false}; // 电脑是否已受理本轮指令
    std::atomic<bool> watch_running{false}; // 是否仍在自动读取回复
    std::atomic<bool> held{true}; // 屏触或 BOOT 是否保持录音意图
    std::atomic<bool> finish_requested{false}; // 采集端请求结束本轮录音
    std::atomic<bool> finish_scheduled{false}; // 是否已经安排串行收尾
    std::atomic<size_t> samples{0}; // 本轮已接收的单声道采样数
    DownloadGate gate; // 请求取消与 HTTP 绑定门闩
    // 音频任务先创建、再启动网络任务，此后只读该引用，录音数据由内部有界队列承载。
    std::shared_ptr<workbuddy::VoiceStream> stream; // 本轮实时语音连接及有界缓冲
    std::atomic<bool> stream_running{false}; // 识别工作任务是否仍使用音频缓冲
    std::atomic<bool> transcript_final{false}; // 识别文字是否已最终确认
    std::mutex result_mutex; // 保护识别和回复结果
    workbuddy::TurnResult result; // 当前识别或电脑请求结果
    workbuddy::AudioCapture audio; // 仅由音频任务启停的音频占用

};

/** @brief 页面控件与定时器，只允许 LVGL 线程访问。 */
struct Page {
    uint32_t input_generation = 0; // 页面独立输入代号
    int64_t last_result_paint_us = 0; // 上次正文更新的单调时钟时间，微秒
    bool touch_active = false; // 是否仍跟踪本次屏幕按压
    bool touch_ptt_held = false; // 屏触是否已经进入长按录音
    bool touch_suppress_click = false; // 本次松手后是否抑制短点翻页
    int64_t touch_pressed_us = 0; // 屏幕按下的时间，微秒
    lv_timer_t* touch_arm_timer = nullptr; // 等待长按阈值的一次性定时器
    lv_obj_t* touch_hit = nullptr; // 正文录音与翻页热区
    lv_point_t touch_start{}; // 本次按压起点
    lv_point_t touch_last{}; // 最近一次按压坐标
    lv_obj_t* root = nullptr; // 页面根对象
    lv_obj_t* status_clock = nullptr; // 共用状态栏的时钟标签
    lv_obj_t* status_notification = nullptr; // 共用状态栏的通知标签
    lv_obj_t* wave = nullptr; // 倾听波形容器
    lv_obj_t* wave_bars[kWaveBars]{}; // 倾听动画的短线控件
    lv_timer_t* wave_timer = nullptr; // 倾听动画定时器
    lv_coord_t wave_max_height = 0; // 波形可用高度，像素
    uint8_t wave_frame = 0; // 当前波形动画帧
    bool wave_visible = false; // 波形是否已显示
    lv_obj_t* content = nullptr; // 对话内容容器
    WorkBuddyChat chat; // 有界消息、气泡和分页
    uint32_t next_turn_id = 0; // 本页的语音轮次序号
    bool final_transcript_painted = false; // 当前轮最终文字是否已经定位
    lv_obj_t* title = nullptr; // 居中的 30 号粗体标题
    lv_obj_t* connection_label = nullptr; // 现有中文最小档 25 号常规字连接状态
    lv_obj_t* menu_button = nullptr; // 标题右侧竖向三点菜单
    lv_obj_t* menu_overlay = nullptr; // 可点击关闭的透明菜单遮罩
    lv_obj_t* binding_dialog = nullptr; // 保留底层内容的重新绑定确认层
    lv_obj_t* local_messages_button = nullptr; // 同步消息菜单项
    lv_obj_t* action_label = nullptr; // 助理消息按钮的当前文字
    lv_obj_t* refresh_button = nullptr; // 二维码刷新按钮
    lv_obj_t* refresh_label = nullptr; // 刷新状态文字
    lv_obj_t* qr_frame = nullptr; // 共用样式的二维码外框
    lv_obj_t* switch_account_button = nullptr; // 重新绑定菜单项
    bool reauthorization_required = false; // 当前凭证或权限需要重新绑定
    lv_obj_t* qr_panel = nullptr; // 扫码页面及刷新操作容器
    lv_obj_t* qr = nullptr; // 二维码内容控件
    lv_obj_t* qr_hint = nullptr; // 二维码下方的操作或错误提示
    std::string painted_url; // 已经编码的授权地址
    std::shared_ptr<Authorization> authorization; // 当前页面的授权上下文
    int64_t authorization_retry_at_us = 0; // 本地任务资源不足时的重试时间，仅由页面节拍推进
    uint32_t painted_auth_revision = 0; // 已经渲染的授权快照版本
    lv_timer_t* timer = nullptr; // 页面状态轮询定时器
    // 页面停留期间软保网；仍允许系统按无操作时间进入待机，避免每轮提问重新连 Wi-Fi。
    std::optional<PowerNeedHold> network_hold; // 页面原位持有前台保网引用，待机或离页时归还
    std::shared_ptr<Turn> turn; // 当前语音或助理消息请求
    std::weak_ptr<Turn> painted_turn; // 当前显示内容对应的请求
    Phase painted_phase = Phase::Idle; // 当前显示的处理阶段
    uint32_t painted_result_revision = 0; // 已经显示的结果版本
    bool closed = false; // 页面是否已完成离页清理
};

Page* s_current_page = nullptr; // 仅 LVGL 线程访问当前页面
bool s_standby = false; // 待机覆盖层暂停交互和绘制

/**
 * @brief 使用真实采样和按压状态判断倾听，避免麦克风准备失败时仍显示动画。
 * @param page 当前LVGL页面。
 * @return 是否仍在前台持续采音。
 */
bool IsListening(const Page* page) {
    const auto& turn = page->turn;
    return !page->closed && s_active_input_generation.load() == page->input_generation && turn &&
           turn->phase.load() == Phase::Recording && turn->samples.load() > 0 && turn->held.load() &&
           !turn->finish_requested.load() && !turn->gate.IsCancelled() &&
           (TouchUiFingerIsDown() || BootKey_IsHeld());
}

/**
 * @brief 绘制与百问AI相同的对称波浪，仅修改27条短线的高度。
 * @param page 已创建波形控件的页面。
 * @return 无；不修改正文或触发整页强制刷新。
 */
void PaintListeningWave(Page* page) {
    for (int i = 0; i < kWaveBars; ++i) {
        const int phase = (page->wave_frame + std::abs(i - kWaveBars / 2)) % kWaveFrames;
        // 统一使用LVGL坐标类型；Xtensa上的lv_coord_t可能是long，不能与int字面量混合推导模板。
        const lv_coord_t height = std::max<lv_coord_t>(2, kWaveCurve[phase] * page->wave_max_height / 44);
        lv_obj_set_height(page->wave_bars[i], height);
    }
}

/** @brief 同步动画可见性并管理定时器；@param page 当前页面；@param show 是否显示；@return 无。 */
void SetListeningWave(Page* page, bool show);

/**
 * @brief 仅在实际采音时推进波浪；松手或采集结束后及时释放动画定时器。
 * @param timer 页面独占的LVGL动画定时器。
 * @return 无；所有控件操作均在LVGL任务内执行。
 */
void OnListeningWave(lv_timer_t* timer) {
    auto* page = static_cast<Page*>(lv_timer_get_user_data(timer));
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
void SetListeningWave(Page* page, bool show) {
    if (show == page->wave_visible) return;
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
        if (!page->wave_timer) ESP_LOGW(kTag, "Listening wave timer unavailable");
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
 * @return 无；初始保持隐藏，首批PCM到达后才启动动画。
 */
void CreateListeningWave(Page* page, const EpdStatusBar& bar) {
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
        lv_obj_set_style_bg_color(line, lv_color_black(), 0);
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
void SetLabelIfChanged(lv_obj_t* label, const char* value) {
    if (label && std::strcmp(lv_label_get_text(label), value) != 0) lv_label_set_text(label, value);
}

/** @brief 显示居中的授权或连接提示，不修改已保存的对话。 */
void ShowIntro(Page* page, std::string text) {
    page->chat.ShowIntro(text);
}

/** @brief 仅在对话可见且没有菜单或确认框时处理分页。 */
bool TurnTextPage(Page* page, int delta) {
    if (lv_obj_has_flag(page->content, LV_OBJ_FLAG_HIDDEN) ||
        !lv_obj_has_flag(page->menu_overlay, LV_OBJ_FLAG_HIDDEN) ||
        !lv_obj_has_flag(page->binding_dialog, LV_OBJ_FLAG_HIDDEN)) return false;
    return page->chat.TurnPage(delta);
}

/**
 * @brief 判断当前页面是否已有录音准备、采集或网络请求，防止并发提交。
 * @param page 当前页面，允许为空。
 * @return 存在未完成对话时为 true。
 */
bool IsBusy(const Page* page) {
    if (!page || !page->turn) return false;
    const Phase phase = page->turn->phase.load();
    return page->turn->stream_running.load() || phase == Phase::Preparing || phase == Phase::Recording ||
           phase == Phase::Recognizing || phase == Phase::Sending || phase == Phase::Waiting;
}

/**
 * @brief 新提问或手动查看历史前停止上一轮后台读取，不取消电脑已经受理的任务。
 * @param page 当前LVGL页面。
 * @return 无；实际关闭套接字交给清理任务，不阻塞界面。
 */
void CancelPreviousWatch(Page* page) {
    if (!page || !page->turn || !page->turn->watch_running.load()) return;
    auto previous = page->turn;
    previous->gate.RequestCancel();
    Application::GetInstance().Schedule([previous]() { previous->gate.AbortBoundHttp(); }, Application::TaskDomain::Cleanup);
}

/**
 * @brief 分别更新助理消息、重新绑定和二维码刷新入口。
 * @param page 当前有效页面。
 * @return 无。
 */
void UpdateInputActions(Page* page) {
    if (!page || !page->authorization) return;
    const auto auth = page->authorization->phase.load();
    const bool authorized = auth == AuthPhase::Authorized;
    const bool busy = IsBusy(page);
    const bool authorizing = auth == AuthPhase::Checking || auth == AuthPhase::Authorizing;
    const bool refreshing = page->authorization->refresh_requested.load();
    const bool running = page->authorization->running.load();
    const auto set_disabled = [](lv_obj_t* button, bool disabled) {
        if (disabled) lv_obj_add_state(button, LV_STATE_DISABLED);
        else lv_obj_remove_state(button, LV_STATE_DISABLED);
    };
    SetLabelIfChanged(page->action_label, "同步消息");
    set_disabled(page->local_messages_button, !authorized || busy || running || page->reauthorization_required);
    set_disabled(page->switch_account_button, busy || running);
    set_disabled(page->menu_button, busy || authorizing);
    SetLabelIfChanged(page->refresh_label, refreshing ? "刷新中" : "刷新");
    if (Board::GetInstance().IsNetworkReady()) lv_obj_remove_flag(page->refresh_button, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(page->refresh_button, LV_OBJ_FLAG_HIDDEN);
    set_disabled(page->refresh_button, authorizing || refreshing);
}

/**
 * @brief 只发布发生变化的授权文案，避免重复轮询导致墨水屏持续刷新。
 * @param authorization 后台任务拥有的授权上下文。
 * @param phase 新授权阶段。
 * @param message 页面提示。
 * @param url 仅供扫码的登录地址，不含设备令牌。
 */
void SetAuthorizationView(const std::shared_ptr<Authorization>& authorization, AuthPhase phase,
                          const std::string& message, const std::string& url = {}) {
    std::lock_guard<std::mutex> lock(authorization->view_mutex);
    if (authorization->gate.IsCancelled()) return;
    if (phase == AuthPhase::Checking) authorization->connection.store(ConnectionState::Querying);
    else if (phase == AuthPhase::Error) authorization->connection.store(ConnectionState::Failed);
    else if (phase != AuthPhase::Authorized) authorization->connection.store(ConnectionState::Unknown);
    if (authorization->phase.load() == phase && authorization->message == message &&
        authorization->verification_url == url) return;
    authorization->message = message;
    authorization->verification_url = url;
    authorization->phase.store(phase);
    ++authorization->revision;
}

/**
 * @brief 在可取消的短等待中检查刷新操作，不阻塞 LVGL 或 Application 主任务。
 * @param authorization 当前授权任务。
 * @param milliseconds 最长等待时间。
 * @return 未取消且未请求刷新时返回 true。
 */
bool WaitAuthorization(const std::shared_ptr<Authorization>& authorization, int milliseconds) {
    for (int elapsed = 0; elapsed < milliseconds; elapsed += 200) {
        if (authorization->gate.IsCancelled() || authorization->refresh_requested.load()) return false;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return !authorization->gate.IsCancelled();
}

/**
 * @brief 错误或超时后等待用户主动刷新，避免不断创建未使用的二维码。
 * @param authorization 当前授权任务。
 * @return 用户刷新时为 true，离开页面时为 false。
 */
bool WaitAuthorizationRefresh(const std::shared_ptr<Authorization>& authorization) {
    while (!authorization->gate.IsCancelled()) {
        if (authorization->refresh_requested.exchange(false)) return true;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return false;
}

/**
 * @brief 保存授权后实际查询电脑状态；任何失败只展示原因，不自动发起下一轮扫码。
 * @param authorization 不持有LVGL指针的授权任务。
 * @return 无；保留已经保存的设备授权，电脑离线时仍允许手动重试读取消息。
 */
void VerifyLocalAssistant(const std::shared_ptr<Authorization>& authorization) {
    SetAuthorizationView(authorization, AuthPhase::Checking, "设备授权已保存，正在确认电脑连接…");
    auto status = workbuddy::GetLocalStatus(authorization->gate);
    if (authorization->gate.IsCancelled()) return;
    authorization->connection.store(ConnectionResult(status));
    authorization->reauthorization_required.store(status.code == 401 || status.code == 403);
    std::string message;
    if (status.ok) {
        message = status.online ? "电脑已连接，可以开始使用。" :
                "设备已绑定，电脑暂未连接。\n请在电脑打开对应账号的 WorkBuddy，连接后从菜单“同步消息”重试。";
    } else {
        message = "设备已绑定，暂时无法连接电脑：\n" + status.error;
        if (status.code == 401 || status.code == 403) message += "\n\n处理提示中的问题后，再点击右上角菜单“重新绑定”。";
        else message += "\n\n无需重复扫码，可从菜单“同步消息”重试。";
    }
    SetAuthorizationView(authorization, AuthPhase::Authorized, message);
}

/**
 * @brief 执行授权检查和二维码轮询，恢复待处理会话并让所有网络请求串行执行。
 * @param authorization 后台授权上下文；不包含页面对象。
 */
void RunAuthorization(const std::shared_ptr<Authorization>& authorization) {
    bool create_new = authorization->refresh_requested.exchange(false);
    while (!authorization->gate.IsCancelled()) {
        auto awake = std::make_unique<PowerNeedHold>(PowerNeed::ForegroundNetwork);
        SetAuthorizationView(authorization, AuthPhase::Checking,
                             create_new ? "正在生成新的授权二维码…" : "正在检查 WorkBuddy 授权…");
        // 先恢复待处理会话：旧凭证在重新授权期间仍有效，先查询旧凭证会掩盖即将轮换的新凭证。
        auto session = create_new ? workbuddy::AuthorizationSession{} : workbuddy::LoadPendingSession();
        if (session.ok) authorization->switch_account.store(session.account_switch_allowed);
        if (!create_new && !session.ok) {
            auto status = workbuddy::GetAuthorization(authorization->gate);
            if (authorization->gate.IsCancelled()) return;
            if (!status.ok) {
                SetAuthorizationView(authorization, AuthPhase::Error, status.error);
                awake.reset();
                if (!WaitAuthorizationRefresh(authorization)) return;
                // 授权状态查询的网络错误不代表凭证失效，重试时仍先检查已保存的授权。
                create_new = false;
                continue;
            }
            if (status.authorized) {
                VerifyLocalAssistant(authorization);
                return;
            }
        }
        if (!session.ok) session = workbuddy::CreateAuthorizationSession(authorization->gate,
                                                                        authorization->switch_account.load());
        if (authorization->gate.IsCancelled()) return;
        if (!session.ok) {
            SetAuthorizationView(authorization, AuthPhase::Error, session.error);
            awake.reset();
            if (!WaitAuthorizationRefresh(authorization)) return;
            create_new = true;
            continue;
        }
        authorization->expires_in.store(session.expires_in);
        const char* scan_hint = session.account_switch_allowed ? "请朋友扫码，用自己的账号完成授权" : "请用手机扫码登录并授权";
        SetAuthorizationView(authorization, AuthPhase::Waiting, scan_hint, session.verification_url);
        const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(session.expires_in) * 1000000;
        int failures = 0;
        while (!authorization->gate.IsCancelled()) {
            if (authorization->refresh_requested.exchange(false)) break;
            auto status = workbuddy::PollAuthorizationSession(session, authorization->gate);
            if (authorization->gate.IsCancelled()) return;
            if (authorization->refresh_requested.load()) continue;
            if (status.ok && status.authorized) {
                authorization->binding_changed.store(true);
                VerifyLocalAssistant(authorization);
                return;
            }
            const bool terminal = status.ok && (status.status == "expired" || status.status == "failed");
            if (terminal || esp_timer_get_time() >= deadline) {
                // 只有 Java 已确认会话终止，才允许退回旧授权；本地超时不代表腾讯回调不会继续到达。
                if (terminal) {
                    auto previous = workbuddy::GetAuthorization(authorization->gate);
                    if (authorization->gate.IsCancelled()) return;
                    if (authorization->refresh_requested.load()) continue;
                    if (previous.ok && previous.authorized) {
                        VerifyLocalAssistant(authorization);
                        return;
                    }
                }
                SetAuthorizationView(authorization, AuthPhase::Expired,
                                     status.status == "failed" ? "授权未完成，请刷新二维码重试" : "二维码已过期，请点击刷新");
                awake.reset();
                if (!WaitAuthorizationRefresh(authorization)) return;
                break;
            }
            if (!status.ok) {
                ++failures;
                if (failures >= 3) {
                    SetAuthorizationView(authorization, AuthPhase::Error, status.error);
                    awake.reset();
                    if (!WaitAuthorizationRefresh(authorization)) return;
                    break;
                }
                SetAuthorizationView(authorization, AuthPhase::Waiting,
                                     "网络暂时中断，正在重新查询授权…", session.verification_url);
            } else {
                failures = 0;
                const bool authorizing = status.status == "authorizing";
                SetAuthorizationView(authorization, authorizing ? AuthPhase::Authorizing : AuthPhase::Waiting,
                                     authorizing ? "请在手机上完成腾讯 WorkBuddy 授权" : scan_hint,
                                     session.verification_url);
            }
            WaitAuthorization(authorization, session.poll_interval * 1000);
        }
        create_new = true;
    }
}

/**
 * @brief 授权任务入口，先销毁共享引用再删除 FreeRTOS 任务，避免上下文泄漏。
 * @param argument 堆上保存的授权上下文引用。
 */
void AuthorizationTask(void* argument) {
    {
        std::unique_ptr<std::shared_ptr<Authorization>> holder(static_cast<std::shared_ptr<Authorization>*>(argument));
        auto authorization = *holder;
        RunAuthorization(authorization);
        authorization->running.store(false);
        // 授权完成后低频读取真实连接状态；取消仍复用同一门闩和任务上限。
        while (!authorization->gate.IsCancelled() && authorization->phase.load() == AuthPhase::Authorized) {
            if (!WaitAuthorization(authorization, 30000)) break;
            PowerNeedHold awake(PowerNeed::ForegroundNetwork);
            authorization->connection.store(ConnectionState::Querying);
            auto status = workbuddy::GetLocalStatus(authorization->gate);
            if (authorization->gate.IsCancelled()) break;
            authorization->connection.store(ConnectionResult(status));
            authorization->reauthorization_required.store(status.code == 401 || status.code == 403);
        }
    }
    s_authorization_workers.fetch_sub(1);
    vTaskDeleteWithCaps(nullptr);
}

/** @brief 尝试创建授权任务，资源未释放时由现有页面节拍延后重试 */
void TryStartAuthorization(Page* page) {
    auto authorization = page->authorization;
    page->authorization_retry_at_us = 0;
    if (s_authorization_workers.fetch_add(1) >= 2) {
        s_authorization_workers.fetch_sub(1);
        authorization->running.store(false);
        page->authorization_retry_at_us = esp_timer_get_time() + 1000000;
        SetAuthorizationView(authorization, AuthPhase::Checking, "正在等待上一次连接关闭…");
        return;
    }
    auto* argument = new (std::nothrow) std::shared_ptr<Authorization>(authorization);
    authorization->running.store(true);
    if (!argument || xTaskCreateWithCaps(AuthorizationTask, "workbuddy_auth", 12 * 1024, argument, 4, nullptr,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        delete argument;
        s_authorization_workers.fetch_sub(1);
        authorization->running.store(false);
        page->authorization_retry_at_us = esp_timer_get_time() + 2000000;
        SetAuthorizationView(authorization, AuthPhase::Error, "内存不足，正在等待资源释放…");
    }
}

/**
 * @brief 启动或刷新授权任务；换绑刷新保留模式，成功前保留正式凭证及旧账号消息。
 * @param page 当前有效页面。
 * @param create_new 是否跳过旧会话并重新扫码授权。
 * @param switch_account 是否由用户主动请求更换绑定账号，默认普通授权。
 * @return 无；已有授权轮询时只置刷新位，不并发建立授权连接。
 */
void StartAuthorization(Page* page, bool create_new, bool switch_account = false) {
    if (!page || page->closed || s_standby) return;
    if (page->authorization && page->authorization->running.load()) {
        if (switch_account) page->authorization->switch_account.store(true);
        page->authorization->refresh_requested.store(true);
        return;
    }
    // 任务创建失败后重试也保留换绑意图，避免退回只允许原账号续授权的普通二维码。
    if (page->authorization) {
        auto previous = page->authorization;
        previous->gate.RequestCancel();
        Application::GetInstance().Schedule([previous]() { previous->gate.AbortBoundHttp(); }, Application::TaskDomain::Cleanup);
    }
    const bool switching = switch_account || (create_new && page->authorization &&
            page->authorization->phase.load() != AuthPhase::Authorized && page->authorization->switch_account.load());
    // 重新授权只会发生在尚未录音或上一轮请求已结束时，清掉旧错误避免授权后再次跳回二维码。
    CancelPreviousWatch(page);
    page->turn.reset();
    page->reauthorization_required = false;
    auto authorization = std::make_shared<Authorization>();
    authorization->switch_account.store(switching);
    authorization->refresh_requested.store(create_new);
    authorization->running.store(true);
    page->authorization = authorization;
    page->painted_auth_revision = 0;
    page->painted_phase = Phase::Idle;
    page->painted_result_revision = 0;
    page->painted_turn.reset();
    page->painted_url.clear();
    lv_obj_add_flag(page->menu_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(page->binding_dialog, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(page->qr_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(page->content, LV_OBJ_FLAG_HIDDEN);
    ShowIntro(page, switching ? "正在准备重新绑定…" : "正在检查绑定，请稍候…");
    TryStartAuthorization(page);
}

/**
 * @brief 安全发布结果文本，再用原子状态通知 LVGL 定时器。
 * @param turn 目标对话。
 * @param message 用户可读错误，不包含授权凭证。
 */
void Fail(const std::shared_ptr<Turn>& turn, const char* message) {
    std::lock_guard<std::mutex> lock(turn->result_mutex);
    turn->result.error = message;
    turn->result_revision.fetch_add(1);
    turn->phase.store(Phase::Error);
}

/**
 * @brief 发布有界结果并递增版本，同一阶段收到新文字也能触发墨水屏更新。
 * @param turn 当前对话共享状态。
 * @param result 将移动到界面的结果，避免复制长回复。
 * @param phase 新显示阶段。
 * @return 无；不调用LVGL。
 */
void PublishTurnResult(const std::shared_ptr<Turn>& turn, workbuddy::TurnResult result, Phase phase) {
    if (turn->gate.IsCancelled()) return;
    std::lock_guard<std::mutex> lock(turn->result_mutex);
    turn->result = std::move(result);
    turn->result_revision.fetch_add(1);
    turn->phase.store(phase);
}

/**
 * @brief 以短间隔等待下一次只读查询，离页或停止等待时迅速退出。
 * @param turn 不包含页面控件的后台状态。
 * @param milliseconds 本轮等待时间。
 * @return 请求仍可继续时为true。
 */
bool WaitLocalReply(const std::shared_ptr<Turn>& turn, int milliseconds) {
    for (int elapsed = 0; elapsed < milliseconds; elapsed += 200) {
        if (turn->gate.IsCancelled()) return false;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return !turn->gate.IsCancelled();
}

/**
 * @brief 在三分钟预算内读取本次指令后的新消息；只更新变化文字，绝不重发POST。
 * @param turn 当前对话，后台任务不持有页面或LVGL指针。
 * @param accepted Java已确认受理的结果，包含本次message_id与原问题。
 * @return 无；首条回复后允许继续提问，新提问会取消旧读取；消息不代表任务完成。
 */
void WatchLocalReplies(const std::shared_ptr<Turn>& turn, workbuddy::TurnResult accepted) {
    PowerNeedHold wait_hold(PowerNeed::ForegroundNetwork);
    const int64_t started = esp_timer_get_time();
    const int64_t deadline = started + kLocalReplyWaitUs;
    bool received = false;
    int failures = 0;
    while (!turn->gate.IsCancelled() && esp_timer_get_time() < deadline) {
        const int64_t elapsed = esp_timer_get_time() - started;
        const int interval = failures ? failures * 3000 : (elapsed < 30000000 ? 2000 : (elapsed < 90000000 ? 5000 : 10000));
        const int wait_ms = static_cast<int>(std::min<int64_t>(interval, (deadline - esp_timer_get_time()) / 1000));
        if (wait_ms <= 0 || !WaitLocalReply(turn, wait_ms)) break;
        const int64_t remaining_ms = (deadline - esp_timer_get_time()) / 1000;
        if (remaining_ms < 1000) break;
        auto reply = workbuddy::GetLocalReply(accepted.message_id, turn->gate,
                static_cast<int>(std::min<int64_t>(remaining_ms, 35000)));
        if (turn->gate.IsCancelled()) break;
        if (!reply.ok) {
            ++failures;
            const bool transient = reply.error_code <= 0 || reply.error_code == 429 || reply.error_code >= 500;
            if (transient && failures < 3) continue;
            reply.transcript = accepted.transcript;
            reply.error = "助理指令已受理，但自动读取中断：\n" + reply.error +
                    "\n\n助理任务可能继续执行。可从菜单“同步消息”查看，请勿重复发送同一指令。";
            if (received) {
                // 移动已显示文字，错误时保留最后读到的回复，不复制另一份长文本。
                std::lock_guard<std::mutex> lock(turn->result_mutex);
                reply.error = std::move(turn->result.reply) + "\n\n" + reply.error;
            }
            PublishTurnResult(turn, std::move(reply), Phase::Error);
            return;
        }
        failures = 0;
        if (reply.status == "waiting") continue;
        reply.transcript = accepted.transcript;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(turn->result_mutex);
            // 固定受理编号持续读取，避免乱猜消息排序；内容不变时不刷新墨水屏。
            changed = !received || turn->result.reply != reply.reply;
        }
        if (changed) PublishTurnResult(turn, std::move(reply), Phase::Done);
        received = true;
    }
    if (turn->gate.IsCancelled()) return;
    if (!received) {
        accepted.reply =
                "助理指令已受理，三分钟内暂未读到回复。助理任务仍可能继续执行，稍后可从菜单“同步消息”查看，请勿重复发送同一指令。";
        PublishTurnResult(turn, std::move(accepted), Phase::Done);
    }
}

/** @brief 在音频任务启动采音；@param turn 已收到实时连接ready的会话；@return 无。 */
void StartCapture(const std::shared_ptr<Turn>& turn);
/**
 * @brief 在任务分配和收尾边界记录两类堆，区分内部RAM碎片、PSRAM不足与旧请求未退出。
 * @param stage 内存记录阶段，不携带用户正文或凭证。
 * @param task_name 请求任务名称。
 * @param stack_bytes 创建所需栈字节数；非创建阶段填零。
 * @return 无；最大连续块决定能否分配整块任务栈，不能只比较总剩余内存。
 */
void LogRequestMemory(const char* stage, const char* task_name, uint32_t stack_bytes = 0) {
    ESP_LOGW(kTag, "Request memory: stage=%s task=%s stack_bytes=%u stack_heap=PSRAM workers=%u "
             "internal_free=%u internal_largest=%u internal_min=%u psram_free=%u psram_largest=%u",
             stage, task_name, static_cast<unsigned>(stack_bytes), s_request_workers.load(),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}

/** @brief 在后台等待可取消的音频初始化，再建立实时识别连接。 */
workbuddy::TurnResult RecognizeTurn(const std::shared_ptr<Turn>& turn) {
    workbuddy::TurnResult result;
    if (turn->gate.IsCancelled() || !turn->held.load()) {
        result.error = "录音已取消";
        return result;
    }
    PowerNeedHold preparing_audio(PowerNeed::LocalMic);
    if (!MetalioAudio_WaitModeReady(kMetalioAudioInitWaitMs, [turn]() {
            return !turn->gate.IsCancelled() && turn->held.load();
        })) {
        result.error = turn->gate.IsCancelled() || !turn->held.load() ? "录音已取消" :
                (MetalioAudio_GetState() == MetalioAudioState::Failed ?
                 "音频模块初始化失败，请检查模块连接后重启设备" : "音频模块尚未就绪，请稍后重试");
    } else if (turn->gate.IsCancelled() || !turn->held.load()) {
        result.error = "录音已取消";
    } else result = turn->stream->Recognize(turn->gate,
            [turn]() {
                if (!Application::GetInstance().Schedule([turn]() { StartCapture(turn); }, Application::TaskDomain::Audio)) {
                    Fail(turn, "录音任务暂忙，请稍后重试");
                    turn->stream->Cancel();
                }
            },
            [turn](const std::string& text, bool final) {
                std::lock_guard<std::mutex> lock(turn->result_mutex);
                if (turn->gate.IsCancelled() || turn->phase.load() == Phase::Error) return;
                turn->result.transcript = text;
                turn->result_revision.fetch_add(1);
                if (final) {
                    turn->transcript_final.store(true);
                    turn->phase.store(Phase::Sending);
                }
            });
    return result;
}

/**
 * @brief 实时转发录音并先发布识别文字，然后只提交一次最终文字；手动消息读取沿用原入口。
 * @param arg 堆上持有的shared_ptr，对应当前录音或消息读取请求。
 * @return 无；完成后销毁共享引用，并使用WithCaps配对回收PSRAM任务栈。
 */
void RequestTask(void* arg) {
    {
        PowerNeedHold request(PowerNeed::ForegroundNetwork);
        std::unique_ptr<std::shared_ptr<Turn>> holder(static_cast<std::shared_ptr<Turn>*>(arg));
        auto turn = *holder;
        workbuddy::TurnResult result;
        if (turn->is_local_refresh) {
            result = workbuddy::GetLocalMessages(turn->gate);
        } else {
            result = RecognizeTurn(turn);
            turn->stream_running.store(false);
            turn->held.store(false);
            while (!Application::GetInstance().Schedule([turn]() {
                // 先停止采音再释放队列；此时网络任务已经退出识别循环。
                turn->audio.Stop();
                turn->stream->ReleaseAudioBuffer();
            }, Application::TaskDomain::Audio)) {
                // 网络工作任务保留上下文，收尾消息入队前不交还最后一份音频占用。
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (!turn->gate.IsCancelled() && turn->phase.load() != Phase::Error) {
                if (result.ok) {
                    const std::string transcript = result.transcript;
                    // 最终文字已发布到UI，电脑请求可独立等待；失败绝不自动重发指令。
                    result = workbuddy::PostText(transcript, turn->gate);
                    result.transcript = transcript;
                    if (!result.ok && (result.error_code <= 0 || result.error_code >= 500)) {
                        result.error += "\n\n发送结果尚未确认，请先查看助理消息，避免重复发送同一指令。";
                    }
                } else {
                    std::lock_guard<std::mutex> lock(turn->result_mutex);
                    result.transcript = turn->result.transcript;
                    result.error += "\n\n本次识别未完成，指令未发送到电脑。";
                }
            } else {
                // 采音失败或短录音错误优先于随后触发的网络取消错误，保留用户已经看到的文字。
                std::lock_guard<std::mutex> lock(turn->result_mutex);
                result = turn->result;
            }
        }
        if (!turn->gate.IsCancelled()) {
            if (!turn->is_local_refresh && result.ok && result.execution_mode == "local" &&
                result.status == "accepted" && !result.message_id.empty()) {
                turn->local_accepted.store(true);
                turn->watch_running.store(true);
                PublishTurnResult(turn, result, Phase::Waiting);
                WatchLocalReplies(turn, std::move(result));
                turn->watch_running.store(false);
                turn->result_revision.fetch_add(1);
            } else {
                const bool ok = result.ok;
                PublishTurnResult(turn, std::move(result), ok ? Phase::Done : Phase::Error);
            }
        }
    }
    // FreeRTOS删除任务不会展开栈；智能指针已退出作用域，WithCaps才能释放栈和任务控制块。
    s_request_workers.fetch_sub(1);
    vTaskDeleteWithCaps(nullptr);
}

/**
 * @brief 为网络请求显式申请PSRAM栈，并限制尚未退出的旧任务数量；失败时归还参数和计数。
 * @param turn 当前对话，只将共享引用交给任务，不访问页面控件。
 * @param name FreeRTOS任务名称。
 * @param stack_bytes 栈大小，单位字节；不通过缩小栈来掩盖内部RAM不足。
 * @return 空串表示启动成功，否则返回可直接展示的具体错误。
 */
const char* StartRequestTask(const std::shared_ptr<Turn>& turn, const char* name, uint32_t stack_bytes) {
    if (s_request_workers.fetch_add(1) >= 2) {
        s_request_workers.fetch_sub(1);
        LogRequestMemory("previous_workers_closing", name, stack_bytes);
        return "上一轮连接正在关闭，请稍后再试";
    }
    auto* argument = new (std::nothrow) std::shared_ptr<Turn>(turn);
    if (!argument) {
        s_request_workers.fetch_sub(1);
        LogRequestMemory("argument_allocation_failed", name, stack_bytes);
        return "请求上下文内存不足，请稍后再试";
    }
    // TCB仍由IDF放内部RAM；只有大块栈使用PSRAM。NVS由client调度至Application内部栈。
    if (xTaskCreateWithCaps(RequestTask, name, stack_bytes, argument, 4, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        delete argument;
        s_request_workers.fetch_sub(1);
        LogRequestMemory("task_allocation_failed", name, stack_bytes);
        return "语音或消息任务内存不足，请稍后再试";
    }
    return "";
}

/**
 * @brief 串行结束采集，排空实时音频队列后确认最终识别；不会创建第二个网络任务。
 * @param turn 需要结束的对话。
 * @return 无；finish只能在采音回调退出后发布，防止截断最后一帧。
 */
void Finish(const std::shared_ptr<Turn>& turn) {
    if (turn->finish_scheduled.exchange(true)) return;
    if (!Application::GetInstance().Schedule([turn]() {
        turn->audio.Stop();
        if (turn->gate.IsCancelled() || turn->phase.load() == Phase::Error) {
            if (turn->stream) turn->stream->Cancel();
            return;
        }
        if (turn->samples.load() < kMinSamples) {
            Fail(turn, "请持续按住，看到倾听波形后再说话（至少半秒）");
            if (turn->stream) turn->stream->Cancel();
            return;
        }
        turn->phase.store(Phase::Recognizing);
        if (turn->stream) turn->stream->FinishInput();
    }, Application::TaskDomain::Audio)) {
        turn->finish_scheduled.store(false);
        Fail(turn, "录音收尾任务暂忙，请稍后重试");
        if (turn->stream) turn->stream->Cancel();
    }
}

/**
 * @brief 实时服务已就绪后启动与百问AI共用的前处理采音，仅向有界队列写入。
 * @param turn 录音上下文；只允许音频工作任务调用。
 * @return 无；迟到的ready不得重新打开已松手或已取消的录音。
 */
void StartCapture(const std::shared_ptr<Turn>& turn) {
    if (turn->gate.IsCancelled() || turn->finish_scheduled.load() || turn->phase.load() != Phase::Preparing ||
        !turn->held.load() || (!TouchUiFingerIsDown() && !BootKey_IsHeld())) {
        turn->held.store(false);
        turn->stream->Cancel();
        return;
    }
    // 仅保存弱引用，避免“对话持有音频回调、回调又持有对话”的循环引用。
    std::weak_ptr<Turn> weak = turn;
    turn->phase.store(Phase::Recording);
    std::string capture_error;
    const bool started = turn->audio.Start([weak](const int16_t* samples, size_t count) {
        auto current = weak.lock();
        if (!current || current->gate.IsCancelled() || !current->held.load() ||
            current->phase.load() != Phase::Recording) return false;
        // 只读取驱动和BOOT的线程安全状态，不访问Page，LVGL繁忙时松手也能及时停止收音。
        if (!TouchUiFingerIsDown() && !BootKey_IsHeld()) {
            current->held.store(false);
            return false;
        }
        if (!samples || count == 0) {
            Fail(current, "麦克风读取失败，请重新进入应用");
            current->finish_requested.store(true);
            return false;
        }
        const size_t offset = current->samples.load();
        const size_t copy = std::min(count, kMaxSamples - offset);
        if (!current->stream->PushPcm(samples, copy)) {
            // 网络失败退出与末帧可能并发，正常收尾不应被覆盖为“队列已满”。
            if (!current->held.load() || !current->stream_running.load() || current->gate.IsCancelled() ||
                current->phase.load() != Phase::Recording) return false;
            Fail(current, "实时语音队列已满或连接中断，请重新说话");
            current->finish_requested.store(true);
            return false;
        }
        current->samples.store(offset + copy);
        if (offset + copy == kMaxSamples) {
            current->finish_requested.store(true);
            return false;
        }
        return true;
    }, [turn]() {
        return !turn->gate.IsCancelled() && turn->held.load() &&
               !turn->finish_scheduled.load() && (TouchUiFingerIsDown() || BootKey_IsHeld());
    }, capture_error);
    if (!started) {
        turn->audio.Stop();
        Fail(turn, capture_error.c_str());
        turn->stream->Cancel();
        return;
    }
}

/**
 * @brief 长按后先建立实时识别通道，收到ready再开麦，连接期间提供明确等待提示。
 * @param page 仍然有效的前台页面。
 * @return 无；未授权或已有请求时忽略按压，旧任务不会被重复提交。
 */
void Begin(Page* page) {
    if (!page || page->closed || !page->authorization ||
        page->authorization->phase.load() != AuthPhase::Authorized || page->reauthorization_required ||
        IsBusy(page) ||
        !lv_obj_has_flag(page->menu_overlay, LV_OBJ_FLAG_HIDDEN) ||
        !lv_obj_has_flag(page->binding_dialog, LV_OBJ_FLAG_HIDDEN)) return;
    if (page->turn && page->turn->phase.load() == Phase::Error && !page->turn->is_local_refresh) Finish(page->turn);
    CancelPreviousWatch(page);
    auto turn = std::make_shared<Turn>();
    if (++page->next_turn_id == 0) ++page->next_turn_id;
    turn->ui_id = page->next_turn_id;
    page->final_transcript_painted = false;
    page->turn = turn;
    if (!Application::GetInstance().Schedule([turn]() {
        if (turn->gate.IsCancelled() || turn->finish_scheduled.load() || !turn->held.load() ||
            (!TouchUiFingerIsDown() && !BootKey_IsHeld())) {
            turn->held.store(false);
            return;
        }
        std::string error;
        turn->stream = workbuddy::VoiceStream::Create(error);
        if (!turn->stream) {
            LogRequestMemory("stream_initialization_failed", "workbuddy_voice", kVoiceStackBytes);
            Fail(turn, error.c_str());
            return;
        }
        turn->stream_running.store(true);
        const char* start_error = StartRequestTask(turn, "workbuddy_voice", kVoiceStackBytes);
        if (*start_error) {
            turn->stream_running.store(false);
            turn->held.store(false);
            turn->stream->ReleaseAudioBuffer();
            Fail(turn, start_error);
        }
    }, Application::TaskDomain::Audio)) {
        turn->held.store(false);
        Fail(turn, "录音任务暂忙，请稍后重试");
    }
}

/**
 * @brief 点击时读取PC会话历史，不把“助理消息”误当成旧消息编号之后的增量轮询。
 * @param event 当前页面的助理消息按钮点击事件。
 * @return 无；不发送新指令，原助理任务在离页或断网后仍由电脑继续处理。
 */
void OnLocalMessages(lv_event_t* event) {
    auto* page = static_cast<Page*>(lv_event_get_user_data(event));
    if (!page || page->closed || IsBusy(page) || !page->authorization ||
        page->authorization->phase.load() != AuthPhase::Authorized) return;
    if (page->turn && !page->turn->is_local_refresh && page->turn->phase.load() == Phase::Error) Finish(page->turn);
    CancelPreviousWatch(page);
    auto turn = std::make_shared<Turn>();
    turn->is_local_refresh = true;
    turn->held.store(false);
    turn->finish_scheduled.store(true);
    turn->phase.store(Phase::Sending);
    page->turn = turn;
    lv_obj_add_flag(page->menu_overlay, LV_OBJ_FLAG_HIDDEN);
    page->painted_phase = Phase::Idle;
    UpdateInputActions(page);
    const char* start_error = StartRequestTask(turn, "workbuddy_pc", kPcStackBytes);
    if (*start_error) Fail(turn, start_error);
}

/**
 * @brief 仅当屏触与BOOT都已松开才结束本轮，避免两种录音输入相互打断。
 * @param page 当前页面，允许为空。
 * @return 无；真正停止采集由状态定时器投递到音频任务。
 */
void Release(Page* page) {
    if (page && page->turn && !page->touch_ptt_held && !BootKey_IsHeld()) page->turn->held.store(false);
    if (page && !IsListening(page)) SetListeningWave(page, false);
}

/**
 * @brief 取消尚未触发的长按定时器，松手或离页后不允许迟到开始录音。
 * @param page 当前LVGL页面。
 * @return 无。
 */
void CancelTouchArm(Page* page) {
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
void EndTouchPress(Page* page) {
    CancelTouchArm(page);
    page->touch_active = false;
    page->touch_ptt_held = false;
    Release(page);
}

/** @brief 全屏短点翻页及长按录音入口；@param event 触摸事件。 */
void OnContentTouch(lv_event_t* event);
/** @brief 刷新当前扫码流程的二维码 */
void OnRefresh(lv_event_t* event);
/** @brief 打开重新绑定确认框；@param event 菜单项点击事件。 */
void OnSwitchAccount(lv_event_t* event);

/**
 * @brief 离页只置取消位，HTTP 关闭和音频停止交给相应工作任务，避免卡住 LVGL。
 * @param page 即将卸载的页面。
 * @return 无；后台只持有共享请求上下文，随后完成资源回收。
 */
void Cancel(Page* page) {
    if (page->closed) return;
    page->closed = true;
    uint32_t generation = page->input_generation;
    if (s_active_input_generation.compare_exchange_strong(generation, 0, std::memory_order_acq_rel)) VkPageRepeatStop();
    if (page->timer) { lv_timer_delete(page->timer); page->timer = nullptr; }
    EndTouchPress(page);
    SetListeningWave(page, false);
    // 先拆子控件回调，避免父屏删除后按压丢失事件继续引用已释放的 Page。
    if (page->touch_hit) {
        lv_obj_remove_event_cb_with_user_data(page->touch_hit, OnContentTouch, page);
        HapticDetachClick(page->touch_hit);
    }
    if (page->local_messages_button) lv_obj_remove_event_cb_with_user_data(page->local_messages_button, OnLocalMessages, page);
    if (page->refresh_button) lv_obj_remove_event_cb_with_user_data(page->refresh_button, OnRefresh, page);
    if (page->switch_account_button) lv_obj_remove_event_cb_with_user_data(page->switch_account_button, OnSwitchAccount, page);
    if (page->authorization) {
        auto authorization = page->authorization;
        authorization->gate.RequestCancel();
        Application::GetInstance().Schedule([authorization]() { authorization->gate.AbortBoundHttp(); }, Application::TaskDomain::Cleanup);
    }
    page->network_hold.reset();
    if (!page->turn) return;
    auto turn = page->turn;
    turn->held.store(false);
    turn->gate.RequestCancel();
    Application::GetInstance().Schedule([turn]() {
        if (!turn->is_local_refresh) turn->audio.Stop();
        turn->gate.AbortBoundHttp();
    }, Application::TaskDomain::Audio);
}

/** @brief 刷新当前扫码会话，不切换绑定意图或删除正式凭证 */
void OnRefresh(lv_event_t* event) {
    auto* page = static_cast<Page*>(lv_event_get_user_data(event));
    if (!page || page->closed || !page->authorization) return;
    const auto phase = page->authorization->phase.load();
    if (phase == AuthPhase::Authorized || phase == AuthPhase::Checking || phase == AuthPhase::Authorizing) return;
    StartAuthorization(page, true, page->authorization->switch_account.load());
    UpdateInputActions(page);
}

/**
 * @brief 用户主动获取换绑二维码，停止旧回复读取并清理录音收尾，不删除助理任务。
 * @param event 顶部重新绑定按钮的点击事件。
 * @return 无；忙碌时忽略迟到点击，新账号成功授权后才替换设备凭证。
 */
void OnConfirmSwitch(lv_event_t* event) {
    auto* page = static_cast<Page*>(lv_event_get_user_data(event));
    if (!page || page->closed || !page->authorization || IsBusy(page) ||
        page->authorization->running.load()) return;
    lv_obj_add_flag(page->binding_dialog, LV_OBJ_FLAG_HIDDEN);
    page->touch_suppress_click = true;
    EndTouchPress(page);
    if (page->turn) {
        auto previous = page->turn;
        previous->held.store(false);
        previous->gate.RequestCancel();
        // Error阶段的采集收尾可能尚未执行，切换账号也必须先释放原麦克风持有。
        Application::GetInstance().Schedule([previous]() {
            if (!previous->is_local_refresh) previous->audio.Stop();
            previous->gate.AbortBoundHttp();
        }, Application::TaskDomain::Audio);
    }
    StartAuthorization(page, true, true);
    UpdateInputActions(page);
}

void OnCloseOverlay(lv_event_t* event) {
    auto* page = static_cast<Page*>(lv_event_get_user_data(event));
    if (!page || page->closed) return;
    lv_obj_add_flag(page->menu_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(page->binding_dialog, LV_OBJ_FLAG_HIDDEN);
}

void OnToggleMenu(lv_event_t* event) {
    auto* page = static_cast<Page*>(lv_event_get_user_data(event));
    if (!page || page->closed || IsBusy(page)) return;
    EndTouchPress(page);
    if (lv_obj_has_flag(page->menu_overlay, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(page->menu_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(page->menu_overlay);
    } else lv_obj_add_flag(page->menu_overlay, LV_OBJ_FLAG_HIDDEN);
}

void OnSwitchAccount(lv_event_t* event) {
    auto* page = static_cast<Page*>(lv_event_get_user_data(event));
    if (!page || page->closed || IsBusy(page)) return;
    lv_obj_add_flag(page->menu_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(page->binding_dialog, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(page->binding_dialog);
}

/** @brief BOOT按下先等待长按阈值，短按不启动麦克风；@return 已消费按键。 */
bool OnBootDown() { return true; }
/** @brief BOOT满500毫秒后投递录音意图，与屏触共用长按语义；@return 已消费按键。 */
bool OnBootLong() { QueueInput(InputCommand::BootLong); return true; }
/** @brief BOOT 松开只投递结束意图，不跨线程读取对话指针；@return 已消费按键。 */
bool OnBootUp() { QueueInput(InputCommand::BootUp); return true; }
/** @brief 消费BOOT单击和双击，避免触发其它应用；@return true。 */
bool OnBootConsumed() { return true; }

/**
 * @brief 输入任务只投递翻页或返回意图，避免与 LVGL 绘制并发修改控件树。
 * @param key 统一虚拟键名。
 * @return 当前页面是否处理此按键。
 */
bool OnKey(const char* key) {
    if (!key) return false;
    if (std::strcmp(key, "vk_home") == 0) {
        QueueInput(InputCommand::Home);
        return true;
    }
    if (std::strcmp(key, "vk_prev") == 0) {
        QueueInput(InputCommand::Back);
        return true;
    }
    if (std::strcmp(key, "vk_next") == 0) {
        QueueInput(InputCommand::Next);
        return true;
    }
    return false;
}

/** @brief 连翻只投递步长，控件访问和边界判断仍在 LVGL 线程执行。 */
bool QueuePageRepeat(int page_delta) {
    if (s_active_input_generation.load(std::memory_order_acquire) == 0) return false;
    QueueInput(InputCommand::Repeat, page_delta);
    return true;
}

/** @brief 复用通用长按连翻节拍，每秒按方向切换十页。 */
bool OnKeyLongPress(const char* key) {
    if (key && std::strcmp(key, "vk_home") == 0) return OnKey(key);
    return VkPageRepeatTryStart(key, QueuePageRepeat);
}

/** @brief 盖板键松开后停止连翻。 */
bool OnKeyPressUp(const char* key) {
    return VkPageRepeatOnPressUp(key);
}

/**
 * @brief 同百问AI在长按阈值到达时开始录音，已松手或正在发送时不会迟到开启。
 * @param timer 页面持有的一次性LVGL定时器，触发后由LVGL自动删除。
 * @return 无；先清句柄，避免松手和离页重复释放定时器。
 */
void ArmTouchRecording(lv_timer_t* timer) {
    auto* page = static_cast<Page*>(lv_timer_get_user_data(timer));
    page->touch_arm_timer = nullptr;
    if (page->closed || !page->touch_active || page->touch_suppress_click) return;
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
void OnContentTouch(lv_event_t* event) {
    auto* page = static_cast<Page*>(lv_event_get_user_data(event));
    if (!page || page->closed || lv_event_get_target(event) != page->touch_hit) return;
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_CLICKED) {
        if (!page->touch_suppress_click) {
            if (page->touch_start.x < LV_HOR_RES / 3) QueueInput(InputCommand::Previous);
            else if (page->touch_start.x > LV_HOR_RES * 2 / 3) QueueInput(InputCommand::Next);
        }
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!page->touch_active || code == LV_EVENT_PRESS_LOST ||
            esp_timer_get_time() - page->touch_pressed_us >= static_cast<int64_t>(kBootLongPressMs) * 1000)
            page->touch_suppress_click = true;
        EndTouchPress(page);
        return;
    }
    auto* input = lv_event_get_indev(event);
    if (!input || lv_indev_get_type(input) != LV_INDEV_TYPE_POINTER) return;
    lv_indev_get_point(input, &page->touch_last);
    if (code == LV_EVENT_PRESSED) {
        CancelTouchArm(page);
        page->touch_start = page->touch_last;
        page->touch_active = true;
        page->touch_suppress_click = false;
        page->touch_pressed_us = esp_timer_get_time();
        const Phase phase = page->turn ? page->turn->phase.load() : Phase::Idle;
        const bool can_record = page->authorization &&
                page->authorization->phase.load() == AuthPhase::Authorized && !page->reauthorization_required &&
                lv_obj_has_flag(page->menu_overlay, LV_OBJ_FLAG_HIDDEN) &&
                lv_obj_has_flag(page->binding_dialog, LV_OBJ_FLAG_HIDDEN) &&
                phase != Phase::Recognizing && phase != Phase::Sending && phase != Phase::Waiting;
        if (can_record) {
            page->touch_arm_timer = lv_timer_create(ArmTouchRecording, kBootLongPressMs, page);
            if (page->touch_arm_timer) lv_timer_set_repeat_count(page->touch_arm_timer, 1);
            else page->touch_suppress_click = true;
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
bool ProcessInput(Page* page) {
    InputMessage message{};
    const auto return_to_apps = [page]() {
        const auto go = [](void*) { ScreenGoHome(); };
        if (!ScreenLvAsyncUrgent(go) && !ScreenLvAsync(go)) return false;
        Cancel(page);
        return true;
    };
    if (s_input_overflow_generation.exchange(0, std::memory_order_acq_rel) == page->input_generation) {
        // 固定上限保证本次消费不会被持续输入占住；溢出时优先结束可能仍在采集的录音。
        for (UBaseType_t count = 0; count < kInputQueueLength; ++count) {
            if (xQueueReceive(s_input_queue, &message, 0) != pdPASS) break;
        }
        page->touch_suppress_click = true;
        EndTouchPress(page);
        if (page->turn) page->turn->held.store(false);
        VkPageRepeatStop();
        ESP_LOGW(kTag, "input queue full; pending keys discarded and recording released");
        return false;
    }
    int scroll_steps = 0;
    bool repeat_step = false;
    for (UBaseType_t count = 0; count < kInputQueueLength; ++count) {
        if (xQueueReceive(s_input_queue, &message, 0) != pdPASS) break;
        if (message.generation != page->input_generation) continue;
        switch (message.command) {
            case InputCommand::Home:
                if (return_to_apps()) return true;
                break;
            case InputCommand::Back:
                if (scroll_steps != 0) {
                    TurnTextPage(page, scroll_steps);
                    scroll_steps = 0;
                }
                if (!lv_obj_has_flag(page->menu_overlay, LV_OBJ_FLAG_HIDDEN) ||
                    !lv_obj_has_flag(page->binding_dialog, LV_OBJ_FLAG_HIDDEN)) {
                    lv_obj_add_flag(page->menu_overlay, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(page->binding_dialog, LV_OBJ_FLAG_HIDDEN);
                } else if (!TurnTextPage(page, -1) && return_to_apps()) {
                    return true;
                }
                break;
            case InputCommand::Previous: --scroll_steps; break;
            case InputCommand::Next: ++scroll_steps; break;
            case InputCommand::BootLong:
                if (BootKey_IsHeld()) Begin(page);
                break;
            case InputCommand::BootUp: Release(page); break;
            case InputCommand::Repeat:
                if (VkPageRepeatIsActive()) {
                    scroll_steps += message.page_delta;
                    repeat_step = true;
                }
                break;
        }
    }
    if (scroll_steps != 0 && !TurnTextPage(page, scroll_steps) && repeat_step) VkPageRepeatStop();
    return false;
}

/**
 * @brief 在 LVGL 线程渲染授权阶段，二维码只编码登录网址。
 * @param page 当前有效页面。
 * @return 已授权时返回 true，允许继续渲染语音内容。
 */
bool PaintAuthorization(Page* page) {
    auto authorization = page->authorization;
    if (!authorization) return false;
    AuthPhase phase;
    std::string message;
    std::string url;
    {
        std::lock_guard<std::mutex> lock(authorization->view_mutex);
        phase = authorization->phase.load();
        if (page->painted_auth_revision == authorization->revision) return phase == AuthPhase::Authorized;
        page->painted_auth_revision = authorization->revision;
        message = authorization->message;
        url = authorization->verification_url;
    }
    const bool qr_visible = phase == AuthPhase::Waiting || phase == AuthPhase::Authorizing ||
                            phase == AuthPhase::Expired || phase == AuthPhase::Error;
    if (qr_visible) {
        lv_obj_add_flag(page->content, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(page->qr_panel, LV_OBJ_FLAG_HIDDEN);
        const int seconds = authorization->expires_in.load();
        const std::string validity = seconds % 60 == 0 ? std::to_string(seconds / 60) + " 分钟" :
                std::to_string(seconds) + " 秒";
        const std::string hint = phase == AuthPhase::Expired || phase == AuthPhase::Error ? message :
                (phase == AuthPhase::Authorizing ? "请在手机上确认绑定" : "手机扫码绑定助理\n二维码有效期 " + validity);
        SetLabelIfChanged(page->qr_hint, hint.c_str());
        if (url.empty()) lv_obj_add_flag(page->qr_frame, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(page->qr_frame, LV_OBJ_FLAG_HIDDEN);
        if (page->painted_url != url && !url.empty()) {
            if (lv_qrcode_update(page->qr, url.data(), url.size()) != LV_RESULT_OK) {
                lv_obj_add_flag(page->qr_frame, LV_OBJ_FLAG_HIDDEN);
                SetLabelIfChanged(page->qr_hint, "二维码生成失败，请点击刷新重试。");
                message = "二维码暂时无法显示";
            } else page->painted_url = url;
        }
    } else {
        lv_obj_add_flag(page->qr_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(page->content, LV_OBJ_FLAG_HIDDEN);
        if (phase == AuthPhase::Authorized) {
            page->reauthorization_required = authorization->reauthorization_required.load();
            if (authorization->binding_changed.exchange(false)) page->chat.Clear();
            ShowIntro(page, "");
            if (page->reauthorization_required) ScreenShowToast(message.c_str());
        } else {
            ShowIntro(page, authorization->switch_account.load() ?
                    "正在准备重新绑定，请稍候…" : "正在检查绑定，请稍候…");
        }
    }
    UpdateInputActions(page);
    return phase == AuthPhase::Authorized;
}

/**
 * @brief 低频检查线程状态，正文仅变化时刷新，倾听波形由独立小区域定时器处理。
 * @param timer 页面拥有的 LVGL 定时器。
 * @return 无；同步录音可见状态并合并正文更新。
 */
void OnTick(lv_timer_t* timer) {
    auto* page = static_cast<Page*>(lv_timer_get_user_data(timer));
    if (page->closed || s_standby) return;
    // 绘制导致LVGL松手事件迟到时，以触摸驱动的实际状态兜底，不让录音多持续一轮。
    if (page->touch_active && !TouchUiFingerIsDown()) {
        // 短点仍交给CLICKED翻页；已臂听或达到长按阈值时必须抑制该点击。
        if (page->touch_ptt_held || esp_timer_get_time() - page->touch_pressed_us >=
                static_cast<int64_t>(kBootLongPressMs) * 1000) {
            page->touch_suppress_click = true;
            EndTouchPress(page);
        }
    }
    if (ProcessInput(page)) return;
    if (page->authorization_retry_at_us != 0 && esp_timer_get_time() >= page->authorization_retry_at_us) {
        TryStartAuthorization(page);
    }
    SetListeningWave(page, IsListening(page));
    const bool authorized = PaintAuthorization(page);
    if (!page->authorization) return;
    SetLabelIfChanged(page->connection_label, ConnectionText(page->authorization->connection.load()));
    if (authorized) page->reauthorization_required = page->authorization->reauthorization_required.load();
    UpdateInputActions(page);
    if (!authorized) return;
    if (!page->turn) return;
    auto turn = page->turn;
    const Phase phase = turn->phase.load();
    if (((phase == Phase::Preparing || phase == Phase::Recording) && !turn->held.load()) ||
        turn->finish_requested.load()) Finish(turn);
    const uint32_t revision = turn->result_revision.load();
    const bool same_turn = page->painted_turn.lock() == turn;
    if (same_turn && phase == page->painted_phase && revision == page->painted_result_revision) return;
    const bool partial_update = same_turn && phase == page->painted_phase &&
            (phase == Phase::Recording || phase == Phase::Recognizing);
    if (partial_update && esp_timer_get_time() - page->last_result_paint_us < kMinTranscriptPaintUs) return;
    const bool ongoing_reply = same_turn && phase == Phase::Done && page->painted_phase == Phase::Done &&
                               turn->local_accepted.load();
    if (ongoing_reply && esp_timer_get_time() - page->last_result_paint_us < kMinResultPaintUs) return;
    workbuddy::TurnResult result;
    {
        std::unique_lock<std::mutex> lock(turn->result_mutex, std::try_to_lock);
        if (!lock.owns_lock()) return;
        if (turn->is_local_refresh && phase == Phase::Done) result = std::move(turn->result);
        else result = turn->result;
    }
    const Phase previous_phase = page->painted_phase;
    page->painted_turn = turn;
    page->painted_phase = phase;
    page->painted_result_revision = revision;
    if (result.authorization_required || result.permission_required) {
        page->authorization->reauthorization_required.store(true);
        page->authorization->connection.store(ConnectionState::Failed);
    }
    page->reauthorization_required = page->authorization->reauthorization_required.load();
    UpdateInputActions(page);
    const bool final_transcript = turn->transcript_final.load();
    if (!turn->is_local_refresh && !result.transcript.empty()) {
        const bool follow = !final_transcript || !page->final_transcript_painted;
        page->chat.SetMessage(turn->ui_id, true, result.transcript, follow);
        if (final_transcript) page->final_transcript_painted = true;
    }
    if (phase == Phase::Preparing && !same_turn) ScreenShowToast("正在连接语音服务，请继续按住");
    if (phase == Phase::Recognizing && (!same_turn || phase != previous_phase)) ScreenShowToast("正在识别…");
    if (phase == Phase::Sending && (!same_turn || phase != previous_phase))
        ScreenShowToast(turn->is_local_refresh ? "正在同步消息…" : "正在发送…");
    std::string reply;
    if (phase == Phase::Waiting) reply = "助理正在处理…";
    else if (phase == Phase::Done && !turn->is_local_refresh) reply = result.reply;
    else if (phase == Phase::Error) reply = result.error;
    if (!reply.empty()) page->chat.SetMessage(turn->ui_id, false, reply, turn->is_local_refresh);
    if (phase == Phase::Done && turn->is_local_refresh && (!same_turn || previous_phase != Phase::Done)) {
        if (result.messages.empty()) ScreenShowToast("暂无可同步的文本消息");
        else ScreenShowToast(page->chat.ReplaceMessages(result.messages) ? "消息已同步" : "消息排版失败，请稍后重试");
    }
    page->last_result_paint_us = esp_timer_get_time();
}

/** @brief 统一清理页面、定时器和异步任务引用；@param event 页面卸载或删除事件。 */
void OnLifecycle(lv_event_t* event) {
    auto* page = static_cast<Page*>(lv_event_get_user_data(event));
    Cancel(page);
    if (s_current_page == page) s_current_page = nullptr;
    if (lv_event_get_code(event) == LV_EVENT_DELETE) delete page;
}

/**
 * @brief 创建使用统一中文字体的多行文本标签。
 * @param parent 父控件。
 * @param text 初始文案。
 * @param width 标签宽度。
 * @return 标签对象。
 */
lv_obj_t* Label(lv_obj_t* parent, const char* text, int width) {
    auto* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, fontpack_lv_font_ui(), 0);
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
void StyleActionButton(lv_obj_t* button, bool dark) {
    lv_obj_set_style_bg_color(button, dark ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(button, dark ? lv_color_white() : lv_color_black(), 0);
    lv_obj_set_style_border_color(button, lv_color_black(), 0);
    lv_obj_set_style_border_width(button, 2, 0);
    lv_obj_set_style_radius(button, UiSx(8), 0);
    lv_obj_set_style_text_font(button, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 4, 0);
    lv_obj_set_style_bg_color(button, dark ? lv_color_white() : lv_color_black(), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(button, dark ? lv_color_black() : lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, lv_color_white(), LV_STATE_DISABLED);
    lv_obj_set_style_text_color(button, lv_color_black(), LV_STATE_DISABLED);
    lv_obj_set_style_anim_duration(button, 0, 0);
    lv_obj_set_style_transition(button, &kNoTransition, 0);
}

/** @brief 创建透明菜单层和确认层，仅弹窗自身绘制背景，底层对话保持原位。 */
void CreateMenus(Page* page, lv_coord_t bar_height) {
    auto create_overlay = [page]() {
        auto* overlay = lv_obj_create(page->root);
        lv_obj_remove_style_all(overlay);
        lv_obj_set_size(overlay, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(overlay, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(overlay, OnCloseOverlay, LV_EVENT_CLICKED, page);
        lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
        return overlay;
    };
    page->menu_overlay = create_overlay();
    auto* list = lv_obj_create(page->menu_overlay);
    lv_obj_set_size(list, UiSx(140), LV_SIZE_CONTENT);
    lv_obj_align(list, LV_ALIGN_TOP_RIGHT, -UiSx(16), bar_height + UiSy(54));
    lv_obj_set_style_bg_color(list, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(list, 1, 0);
    lv_obj_set_style_border_color(list, lv_color_black(), 0);
    lv_obj_set_style_radius(list, UiSx(7), 0);
    lv_obj_set_style_pad_all(list, UiSx(5), 0);
    lv_obj_set_style_pad_row(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    auto action = [list](const char* text, const char* symbol) {
        auto* button = lv_button_create(list);
        StyleActionButton(button, false);
        lv_obj_set_size(button, LV_PCT(100), UiSy(49));
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(button, UiSx(9), 0);
        auto* icon = Label(button, symbol, 30);
        lv_obj_set_style_text_font(icon, &font_awesome_30_1, 0);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        auto* label = Label(button, text, LV_SIZE_CONTENT);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        return button;
    };
    page->local_messages_button = action("同步消息", FONT_AWESOME_ARROWS_ROTATE);
    page->action_label = lv_obj_get_child(page->local_messages_button, 1);
    lv_obj_add_event_cb(page->local_messages_button, OnLocalMessages, LV_EVENT_CLICKED, page);
    auto* separator = lv_obj_create(list);
    lv_obj_remove_style_all(separator);
    lv_obj_set_size(separator, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(separator, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(separator, LV_OPA_COVER, 0);
    lv_obj_remove_flag(separator, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    page->switch_account_button = action("重新绑定", FONT_AWESOME_LINK);
    lv_obj_add_event_cb(page->switch_account_button, OnSwitchAccount, LV_EVENT_CLICKED, page);

    page->binding_dialog = create_overlay();
    auto* dialog = lv_obj_create(page->binding_dialog);
    lv_obj_set_size(dialog, UiSx(420), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(dialog, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dialog, lv_color_black(), 0);
    lv_obj_set_style_border_width(dialog, 1, 0);
    lv_obj_set_style_radius(dialog, UiSx(9), 0);
    lv_obj_set_style_pad_all(dialog, UiSx(20), 0);
    lv_obj_set_style_pad_row(dialog, UiSy(20), 0);
    lv_obj_set_flex_flow(dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);
    auto* heading = Label(dialog, "重新绑定助理账号？", UiSx(376));
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    Label(dialog, "新账号授权成功前，保留原绑定。已提交的任务可能继续执行。", UiSx(376));
    auto* row = lv_obj_create(dialog);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, UiSx(376), UiSy(48));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, UiSx(12), 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 2; ++i) {
        auto* button = lv_button_create(row);
        StyleActionButton(button, i == 1);
        lv_obj_set_size(button, UiSx(182), UiSy(48));
        auto* label = Label(button, i == 0 ? "暂不更换" : "继续绑定", UiSx(168));
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(label);
        lv_obj_add_event_cb(button, i == 0 ? OnCloseOverlay : OnConfirmSwitch, LV_EVENT_CLICKED, page);
    }
    lv_obj_center(dialog);
}

/**
 * @brief 创建正文透明触摸层，标题状态不响应翻页或录音，菜单保持可点击。
 * @param page 已创建正文和顶部功能按钮的页面。
 * @return 无；热区自身没有可见边框、按钮或按压动画。
 */
void CreateTouchLayer(Page* page) {
    page->touch_hit = lv_obj_create(page->root);
    lv_obj_remove_style_all(page->touch_hit);
    lv_obj_set_style_bg_opa(page->touch_hit, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(page->touch_hit, 0, lv_obj_get_y(page->content));
    lv_obj_set_size(page->touch_hit, LV_HOR_RES, lv_obj_get_height(page->content));
    lv_obj_add_flag(page->touch_hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(page->touch_hit, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE |
            LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_GESTURE_BUBBLE));
    HapticAttachClick(page->touch_hit);
    lv_obj_add_event_cb(page->touch_hit, OnContentTouch, LV_EVENT_PRESSED, page);
    lv_obj_add_event_cb(page->touch_hit, OnContentTouch, LV_EVENT_PRESSING, page);
    lv_obj_add_event_cb(page->touch_hit, OnContentTouch, LV_EVENT_RELEASED, page);
    lv_obj_add_event_cb(page->touch_hit, OnContentTouch, LV_EVENT_PRESS_LOST, page);
    lv_obj_add_event_cb(page->touch_hit, OnContentTouch, LV_EVENT_CLICKED, page);
    lv_obj_move_foreground(page->menu_button);
    lv_obj_move_foreground(page->qr_panel);
}

}  // namespace

/**
 * @brief 创建黑白整页对话界面，支持屏幕短点翻页、500毫秒长按录音及实体按键。
 * @return 按 470 可绘区域缩放的独立 LVGL 屏幕。
 */
lv_obj_t* WorkBuddyScreen::Create() {
    auto* page = new Page();
    s_current_page = page;
    s_standby = false;
    if (!s_input_queue) {
        s_input_queue = xQueueCreateStatic(kInputQueueLength, sizeof(InputMessage),
                                          s_input_queue_storage, &s_input_queue_control);
    }
    xQueueReset(s_input_queue);
    if (++s_next_input_generation == 0) ++s_next_input_generation;
    page->input_generation = s_next_input_generation;
    ScreenSetIsHome(false);
    page->root = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(page->root, lv_color_white(), 0);
    lv_obj_set_style_text_color(page->root, lv_color_black(), 0);
    lv_obj_set_style_text_font(page->root, fontpack_lv_font_ui(), 0);
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
    const auto* title_font = fontpack_lv_font_get(30, 4);
    lv_obj_set_style_text_font(page->title, title_font ? title_font : fontpack_lv_font_ui(), 0);
    lv_obj_remove_flag(page->title, LV_OBJ_FLAG_CLICKABLE);
    page->connection_label = Label(title_group, ConnectionText(ConnectionState::Unknown), LV_SIZE_CONTENT);
    const auto* status_font = fontpack_lv_font_get(25, 2);
    lv_obj_set_style_text_font(page->connection_label, status_font ? status_font : fontpack_lv_font_ui(), 0);
    lv_obj_remove_flag(page->connection_label, LV_OBJ_FLAG_CLICKABLE);
    page->menu_button = lv_button_create(page->root);
    StyleActionButton(page->menu_button, false);
    lv_obj_set_style_border_width(page->menu_button, 0, 0);
    lv_obj_set_style_bg_color(page->menu_button, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_pos(page->menu_button, LV_HOR_RES - UiSx(44), bar.height + UiSy(15));
    lv_obj_set_size(page->menu_button, UiSx(28), UiSy(34));
    for (int i = 0; i < 3; ++i) {
        auto* dot = lv_obj_create(page->menu_button);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 4, 4);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_align(dot, LV_ALIGN_CENTER, 0, (i - 1) * 9);
        lv_obj_remove_flag(dot, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    }
    lv_obj_add_event_cb(page->menu_button, OnToggleMenu, LV_EVENT_CLICKED, page);
    page->content = lv_obj_create(page->root);
    lv_obj_set_pos(page->content, UiSx(0), bar.height + UiSy(64));
    lv_obj_set_size(page->content, LV_HOR_RES, LV_VER_RES - bar.height - UiSy(64));
    lv_obj_set_style_bg_color(page->content, lv_color_white(), 0);
    lv_obj_set_style_radius(page->content, 0, 0);
    lv_obj_set_style_border_width(page->content, 0, 0);
    lv_obj_set_style_pad_all(page->content, kContentPad, 0);
    lv_obj_set_scrollbar_mode(page->content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(page->content, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE |
            LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC));
    lv_obj_remove_flag(page->content, LV_OBJ_FLAG_CLICKABLE);
    page->chat.Create(page->content);
    ShowIntro(page, "正在检查绑定，请稍候…");
    page->qr_panel = lv_obj_create(page->root);
    lv_obj_set_pos(page->qr_panel, UiSx(16), bar.height + UiSy(64));
    lv_obj_set_size(page->qr_panel, LV_HOR_RES - UiSx(32), LV_VER_RES - bar.height - UiSy(128));
    lv_obj_set_style_bg_color(page->qr_panel, lv_color_white(), 0);
    lv_obj_set_style_border_width(page->qr_panel, 0, 0);
    lv_obj_set_style_pad_all(page->qr_panel, 0, 0);
    lv_obj_remove_flag(page->qr_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(page->qr_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page->qr_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(page->qr_panel, UiSy(16), 0);
    page->qr_frame = UiCreateQrFrame(page->qr_panel, UiSx(240));
    page->qr = lv_qrcode_create(page->qr_frame);
    lv_qrcode_set_size(page->qr, UiSx(240));
    lv_qrcode_set_dark_color(page->qr, lv_color_black());
    lv_qrcode_set_light_color(page->qr, lv_color_white());
    lv_qrcode_set_quiet_zone(page->qr, true);
    page->qr_hint = Label(page->qr_panel, "请使用手机扫码绑定", LV_HOR_RES - UiSx(48));
    lv_obj_set_style_text_align(page->qr_hint, LV_TEXT_ALIGN_CENTER, 0);
    page->refresh_button = lv_button_create(page->qr_panel);
    StyleActionButton(page->refresh_button, false);
    lv_obj_set_size(page->refresh_button, UiSx(112), UiSy(48));
    page->refresh_label = Label(page->refresh_button, "刷新", UiSx(96));
    lv_obj_set_style_text_align(page->refresh_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(page->refresh_label);
    lv_obj_add_event_cb(page->refresh_button, OnRefresh, LV_EVENT_CLICKED, page);
    lv_obj_add_flag(page->qr_panel, LV_OBJ_FLAG_HIDDEN);
    CreateMenus(page, bar.height);
    CreateTouchLayer(page);
    lv_obj_add_event_cb(page->root, OnLifecycle, LV_EVENT_SCREEN_UNLOADED, page);
    lv_obj_add_event_cb(page->root, OnLifecycle, LV_EVENT_DELETE, page);
    page->timer = lv_timer_create(OnTick, 150, page);
    page->network_hold.emplace(PowerNeed::ForegroundNetwork);
    StartAuthorization(page, false);
    VkKeyScreenDesc keys;
    keys.factory = WorkBuddyScreen::Create;
    keys.on_key = OnKey;
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
    if (!page || page->closed || s_standby == standby) return;
    s_standby = standby;
    if (standby) {
        VkPageRepeatStop();
        page->authorization_retry_at_us = 0;
        s_active_input_generation.store(0, std::memory_order_release);
        if (page->turn) {
            auto turn = page->turn;
            turn->gate.RequestCancel();
            turn->held.store(false);
            Application::GetInstance().Schedule([turn]() {
                if (!turn->is_local_refresh) turn->audio.Stop();
                turn->gate.AbortBoundHttp();
            }, Application::TaskDomain::Audio);
        }
        EndTouchPress(page);
        SetListeningWave(page, false);
        if (page->turn && IsBusy(page)) {
            if (!page->turn->is_local_refresh) {
                page->chat.SetMessage(page->turn->ui_id, false,
                        "本次录音或等待已停止。已提交的任务可能继续执行，可同步消息查看。", false);
            }
            page->turn.reset();
            page->painted_turn.reset();
            page->painted_phase = Phase::Idle;
            page->painted_result_revision = 0;
            ShowIntro(page, "");
            ScreenShowToast("已停止录音或等待，可继续对话");
        }
        if (page->authorization) {
            auto authorization = page->authorization;
            authorization->gate.RequestCancel();
            Application::GetInstance().Schedule([authorization]() { authorization->gate.AbortBoundHttp(); }, Application::TaskDomain::Cleanup);
            page->authorization.reset();
        }
        page->network_hold.reset();
    } else {
        page->network_hold.emplace(PowerNeed::ForegroundNetwork);
        xQueueReset(s_input_queue);
        s_active_input_generation.store(page->input_generation, std::memory_order_release);
        if (!page->authorization) StartAuthorization(page, false);
    }
}
