#pragma once
#include <stdint.h>
#include <stddef.h>

#define XZ_APP_ABI_VERSION 1 // 应用接口版本

// 检查可选字段边界后，仍须检查函数指针非空。
#define XZ_APP_HAS_FIELD(api, member) ((api) && (api)->struct_size >= offsetof(xz_app_api_t, member) + sizeof((api)->member))
typedef void (*xz_app_click_t)(void* context); // 按钮点击回调
typedef int (*xz_app_back_t)(void* context); // 返回非零表示应用已处理返回键
typedef enum {
    XZ_APP_ALIGN_LEFT, // 左对齐
    XZ_APP_ALIGN_CENTER, // 居中
    XZ_APP_ALIGN_RIGHT // 右对齐
} xz_app_text_align_t;
typedef struct {
    uint16_t font_size; // 字库字号，零表示保持现有字号
    uint8_t text_align; // xz_app_text_align_t 对齐方式
    uint8_t radius; // 圆角半径
    uint8_t border_width; // 边框线宽
    uint8_t primary; // 非零使用黑底白字
} xz_app_style_t;
typedef enum {
    XZ_APP_OK = 0, // 成功
    XZ_APP_ERR_INVALID = -1, // 参数无效
    XZ_APP_ERR_NOT_READY = -2, // 环境未就绪
    XZ_APP_ERR_NOT_FOUND = -3, // 对象不存在
    XZ_APP_ERR_LIMIT = -4, // 资源上限
    XZ_APP_ERR_IO = -5, // 存储失败
    XZ_APP_ERR_MEMORY = -6, // 分配失败
    XZ_APP_ERR_UNSUPPORTED = -7, // 格式或功能未支持
    XZ_APP_ERR_BUSY = -8 // 资源被其他业务占用
} xz_app_result_t;

typedef enum {
    XZ_APP_CAP_TIME = 1u << 0, // 本地时间与单调计时
    XZ_APP_CAP_TIMER = 1u << 1, // 托管前台定时器
    XZ_APP_CAP_STORAGE = 1u << 2, // 私有存储
    XZ_APP_CAP_WIDGETS = 1u << 3, // 基础控件
    XZ_APP_CAP_EXIT = 1u << 4, // 退出回调
    XZ_APP_CAP_NOTICE = 1u << 5, // 顶部提示
    XZ_APP_CAP_KEEP_AWAKE = 1u << 6, // 阻止无操作自动待机，不阻止电源键待机
    XZ_APP_CAP_DRAWING = 1u << 7, // 托管黑白绘图
    XZ_APP_CAP_ACCELERATION = 1u << 8, // 三轴加速度，不是角速度
    XZ_APP_CAP_BACK = 1u << 9, // 应用内部返回导航
    XZ_APP_CAP_SCREEN_CHANGE = 1u << 10, // 推白后切换画面
    XZ_APP_CAP_VIBRATION = 1u << 11, // 托管限时震动
    XZ_APP_CAP_AUDIO = 1u << 12, // 托管前台音频播放
    XZ_APP_CAP_RECORDING = 1u << 13 // 托管前台录音，保存到应用私有数据目录
} xz_app_capability_t;

typedef struct {
    int64_t unix_seconds; // UTC 秒级时间戳
    int16_t year; // 本地公历年份
    uint8_t month; // 月份，1—12
    uint8_t day; // 日期，1—31
    uint8_t hour; // 小时，0—23
    uint8_t minute; // 分钟，0—59
    uint8_t second; // 秒，0—60
    uint8_t weekday; // 星期，0 表示周日
} xz_app_time_t;

typedef struct {
    int16_t x1, y1, x2, y2; // 相对绘图区坐标，矩形含两端
    uint8_t kind; // 0 直线，1 矩形，2 椭圆
    uint8_t width; // 线宽，1—8
    uint8_t filled; // 非零填充黑色，仅矩形与椭圆有效
    uint8_t reserved; // 必须为零
} xz_app_draw_t;

typedef struct {
    int32_t x, y, z; // 传感器坐标系加速度，单位 mg
} xz_app_acceleration_t;

typedef enum {
    XZ_APP_AUDIO_ASSET = 1, // 应用资源节中的音频区域
    XZ_APP_AUDIO_SD = 2 // 产品 SD 根目录下的相对文件路径
} xz_app_audio_source_t;

typedef enum {
    XZ_APP_AUDIO_OGG_OPUS = 1, // Ogg 封装、单声道 Opus
    XZ_APP_AUDIO_WAV_PCM = 2 // RIFF/WAVE，16kHz 单声道 16 位 PCM
} xz_app_audio_format_t;

typedef enum {
    XZ_APP_AUDIO_STOPPED = 0, // 未播放或已请求停止
    XZ_APP_AUDIO_LOADING, // 后台准备文件与音频硬件
    XZ_APP_AUDIO_PLAYING, // 正在输出音频
    XZ_APP_AUDIO_PAUSED, // 应用暂停或页面暂不在前台
    XZ_APP_AUDIO_ENDED, // 非循环播放已结束
    XZ_APP_AUDIO_ERROR // 播放失败，原因见 result
} xz_app_audio_state_t;

typedef struct {
    uint32_t struct_size; // 设置为本结构大小
    uint32_t source; // xz_app_audio_source_t
    uint32_t format; // xz_app_audio_format_t
    uint32_t loop; // 非零循环播放
    const char* path; // SD 相对路径，提交时复制；资源来源使用空指针
    uint32_t offset; // 音频在资源节中的起始字节，SD 来源必须为零
    uint32_t length; // 资源区域长度；零表示从 offset 到资源节末尾
} xz_app_audio_config_t;

typedef struct {
    uint32_t struct_size; // 调用前设置容量，返回宿主结构大小
    uint32_t formats; // 支持的 xz_app_audio_format_t 位掩码
    uint32_t sources; // 支持的 xz_app_audio_source_t 位掩码
    uint32_t sample_rate; // 当前输出采样率 Hz
    uint32_t channels; // 当前输出声道数
} xz_app_audio_caps_t;

typedef struct {
    uint32_t struct_size; // 调用前设置容量，返回宿主结构大小
    uint32_t state; // xz_app_audio_state_t
    int32_t result; // 最近的播放结果码
    uint32_t volume; // 当前系统音量，0—100
    uint64_t position_ms; // 当前循环内已提交至硬件的播放位置
    uint64_t duration_ms; // 总时长，尚未解析时为零
    uint32_t loops; // 已完成的循环次数
} xz_app_audio_status_t;

typedef enum {
    XZ_APP_RECORD_IDLE = 0, // 尚无录音
    XZ_APP_RECORD_PREPARING, // 准备硬件和临时文件
    XZ_APP_RECORD_RECORDING, // 正在录音
    XZ_APP_RECORD_PAUSED, // 暂停写入，仍持有麦克风
    XZ_APP_RECORD_SAVING, // 完成文件并替换上一条
    XZ_APP_RECORD_SAVED, // 保存完成，可播放
    XZ_APP_RECORD_ERROR // 失败，上一条已保存录音仍保留
} xz_app_record_state_t;

typedef struct {
    uint32_t struct_size; // 调用前设置容量，返回宿主结构大小
    uint32_t state; // xz_app_record_state_t
    int32_t result; // 最近操作的结果码
    uint32_t duration_ms; // 本次实际录入的时长，暂停时间不计入
    uint32_t saved_duration_ms; // 已保存录音时长，未保存时为零
    uint32_t bytes; // 本次 PCM 字节数，不含 WAV 文件头
    char path[256]; // 已保存文件相对产品 SD 根目录的路径，可传给 audio_play
} xz_app_record_status_t;

typedef struct {
    uint32_t abi_version; // 宿主接口版本
    uint32_t struct_size; // 宿主接口结构大小
    void* root; // 宿主管理的应用内容容器
    int width; // 内容区宽度
    int height; // 内容区高度
    const char* language; // 当前界面语言代码
    void* (*label)(void* parent, int x, int y, int width, int height, const char* text); // 创建文本
    void* (*button)(void* parent, int x, int y, int width, int height, const char* text,
                    xz_app_click_t click, void* context); // 创建按钮并登记回调，自动绑定系统点击震动
    void (*set_text)(void* label, const char* text); // 复制并更新标签、按钮或输入框文本
    void* asset_context; // 资源读取上下文，仅应用存活期间有效
    uint32_t asset_size; // ELF 中非加载资源节的长度
    uint32_t (*read_asset)(void* context, uint32_t offset, void* buffer, uint32_t length); // 同步读取指定资源范围，失败返回零
    void (*set_style)(void* widget, const xz_app_style_t* style); // 可选控件样式，使用前检查 struct_size
    void (*set_enabled)(void* widget, int enabled); // 可选控件可用状态，使用前检查 struct_size
    int (*get_local_date)(int* year, int* month, int* day); // 可选设备本地日期，时钟未就绪返回零
    uint32_t capabilities; // 可选能力位，不表示硬件当前就绪
    void* host_context; // 托管接口共用上下文，仅本次运行有效
    int (*get_local_time)(xz_app_time_t* result); // 返回结果码，时间未就绪返回 NOT_READY
    uint64_t (*monotonic_ms)(void); // 本次启动的单调毫秒数，不保证跨重启连续
    uint32_t (*timer_start)(void* host, uint32_t interval_ms, int repeat, xz_app_click_t callback, void* context); // 200ms—24h，最多 8 个，失败为零
    int (*timer_cancel)(void* host, uint32_t id); // 取消托管定时器
    int (*storage_read)(void* host, const char* key, void* buffer, uint32_t capacity); // 返回字节数或负错误码，空 buffer 查询大小
    int (*storage_write)(void* host, const char* key, const void* data, uint32_t length); // 每键最多 16KiB，每应用最多 32 键
    int (*storage_remove)(void* host, const char* key); // 删除私有数据键
    void* (*container)(void* parent, int x, int y, int width, int height, int scroll_y); // 非零 scroll_y 允许纵向滚动
    void* (*input)(void* parent, int x, int y, int width, int height, const char* text, uint32_t max_chars, xz_app_click_t submit, void* context); // 最多 256 字符，确认回调可为空
    int (*get_text)(void* widget, char* buffer, uint32_t capacity); // 返回 UTF-8 字节数或负错误码，容量包含终止符
    void* (*keyboard)(void* host, void* parent, void* input); // 绑定系统键盘，每应用最多一个
    int (*keyboard_close)(void* host); // 删除键盘并取消待处理确认
    void* (*progress)(void* parent, int x, int y, int width, int height); // 创建 0—100 进度条
    void (*set_progress)(void* widget, uint32_t percent); // 无动画更新进度
    int (*set_on_exit)(void* host, xz_app_click_t callback, void* context); // 替换或清除单个退出回调
    void (*notify)(void* host, const char* text); // 系统顶部提示，调用时复制文字
    int (*set_keep_awake)(void* host, int enabled); // 阻止自动待机；不保网，离页时自动释放
    void (*clear_children)(void* parent); // 删除子控件，不删除 parent；不得在绘制事件内调用
    void* (*drawing)(void* parent, int x, int y, int width, int height); // 黑白绘图区，最大 1024×1600
    int (*draw)(void* widget, const xz_app_draw_t* commands, uint32_t count); // 复制至宿主，最多 1024 条；零条清空
    int (*set_acceleration)(void* host, int enabled); // 前台采样订阅，离页与待机自动停止
    int (*get_acceleration)(void* host, xz_app_acceleration_t* result); // 最近 500ms 样本，未就绪返回 NOT_READY
    uint64_t (*foreground_ms)(void* host); // 前台累计毫秒；待机与离页不累计
    int (*set_on_back)(void* host, xz_app_back_t callback, void* context); // 返回键优先交应用处理，零表示使用宿主默认返回
    int (*request_screen_change)(void* host); // 下一次物理提交先推白，再显示目标画面；不阻塞 UI
    int (*vibrate)(void* host, uint32_t duration_ms); // 请求 1—15000ms 单次震动，尊重系统震动开关；未结束时不叠加
    int (*audio_get_caps)(void* host, xz_app_audio_caps_t* result); // 查询实际支持的音频能力
    int (*audio_play)(void* host, const xz_app_audio_config_t* config); // 后台分块播放；返回成功表示请求已接纳
    int (*audio_pause)(void* host, int paused); // 非零暂停，零继续；不重新开始当前音频
    int (*audio_stop)(void* host); // 请求停止并后台归还音频与文件资源
    int (*audio_set_volume)(void* host, uint32_t volume); // 异步设置系统音量，0—100，与系统音量键共用
    int (*audio_get_status)(void* host, xz_app_audio_status_t* result); // 查询播放状态、结果、进度与系统音量
    int (*record_get_caps)(void* host, xz_app_audio_caps_t* result); // 查询录音格式、采样率及声道
    int (*record_start)(void* host); // 单录音槽；成功保存后替换上一条录音
    int (*record_pause)(void* host, int paused); // 非零暂停写入，零继续；保留麦克风占用
    int (*record_stop)(void* host); // 异步停止并保存；待机或离页时宿主也会停止保存
    int (*record_get_status)(void* host, xz_app_record_status_t* result); // 查询状态、时长与播放路径
} xz_app_api_t;

/**
 * @brief 创建应用界面
 * @note 宿主通过 argv[0] 传入 xz_app_api_t；所有接口及回调仅在 UI 线程使用，不得阻塞、持有退出后的引用或启动后台任务。
 */
int app_main(int argc, char** argv);
