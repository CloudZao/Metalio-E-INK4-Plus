#pragma once
#include <stdint.h>
#include <stddef.h>

#define XZ_APP_ABI_VERSION 1  // 应用接口版本

// 检查可选字段边界后，仍须检查函数指针非空。
#define XZ_APP_HAS_FIELD(api, member) \
    ((api) && (api)->struct_size >= offsetof(xz_app_api_t, member) + sizeof((api)->member))
typedef void (*xz_app_click_t)(void* context);  // 按钮点击回调
typedef int (*xz_app_back_t)(void* context);    // 返回非零表示应用已处理返回键
typedef enum {
    XZ_APP_ALIGN_LEFT,    // 左对齐
    XZ_APP_ALIGN_CENTER,  // 居中
    XZ_APP_ALIGN_RIGHT    // 右对齐
} xz_app_text_align_t;
typedef struct {
    uint16_t font_size;    // 系统界面字号；零或不支持的规格保持现有字号
    uint8_t text_align;    // xz_app_text_align_t 对齐方式
    uint8_t radius;        // 圆角半径
    uint8_t border_width;  // 边框线宽
    uint8_t primary;       // 非零使用黑底白字
} xz_app_style_t;
typedef enum {
    XZ_APP_OK = 0,              // 成功
    XZ_APP_ERR_INVALID = -1,    // 参数无效
    XZ_APP_ERR_NOT_READY = -2,  // 环境未就绪
    XZ_APP_ERR_NOT_FOUND = -3,  // 对象不存在
    XZ_APP_ERR_LIMIT = -4,      // 资源上限
    XZ_APP_ERR_IO = -5,         // 输入输出失败
    XZ_APP_ERR_MEMORY = -6,      // 分配失败
    XZ_APP_ERR_UNSUPPORTED = -7, // 格式或功能未支持
    XZ_APP_ERR_BUSY = -8,        // 资源被其他业务占用
    XZ_APP_ERR_TIMEOUT = -9,     // 操作超时
    XZ_APP_ERR_CANCELLED = -10   // 操作已取消
} xz_app_result_t;

typedef enum {
    XZ_APP_CAP_TIME = 1u << 0,           // 本地时间与单调计时
    XZ_APP_CAP_TIMER = 1u << 1,          // 托管前台定时器
    XZ_APP_CAP_STORAGE = 1u << 2,        // 私有存储
    XZ_APP_CAP_WIDGETS = 1u << 3,        // 基础控件
    XZ_APP_CAP_EXIT = 1u << 4,           // 退出回调
    XZ_APP_CAP_NOTICE = 1u << 5,         // 顶部提示
    XZ_APP_CAP_KEEP_AWAKE = 1u << 6,     // 阻止无操作自动待机，不阻止电源键待机
    XZ_APP_CAP_DRAWING = 1u << 7,        // 托管黑白绘图
    XZ_APP_CAP_ACCELERATION = 1u << 8,   // 三轴加速度，不是角速度
    XZ_APP_CAP_BACK = 1u << 9,           // 应用内部返回导航
    XZ_APP_CAP_SCREEN_CHANGE = 1u << 10, // 推白后切换画面
    XZ_APP_CAP_VIBRATION = 1u << 11,     // 托管限时震动
    XZ_APP_CAP_AUDIO = 1u << 12,         // 托管前台音频播放
    XZ_APP_CAP_RECORDING = 1u << 13,     // 托管前台录音，保存到应用私有数据目录
    XZ_APP_CAP_IMAGE = 1u << 14,         // 托管图片与灰阶显示
    XZ_APP_CAP_FULL_REFRESH = 1u << 15,  // 当前画面完整刷新
    XZ_APP_CAP_CAMERA = 1u << 16,        // 托管前台摄像头取景
    XZ_APP_CAP_IMAGE_TONE = 1u << 17,    // 显示侧调色、滤镜和几何变换
    XZ_APP_CAP_IMAGE_PIXELS = 1u << 18,  // 托管 L8 像素读写，供应用扩展算法
    XZ_APP_CAP_NETWORK = 1u << 19,      // 托管 HTTP、WebSocket、TCP/TLS 和定远端 UDP
    XZ_APP_CAP_DNS = 1u << 20,          // 托管 IPv4 地址解析
    XZ_APP_CAP_PING = 1u << 21          // 托管 ICMP Echo 探测
} xz_app_capability_t;

typedef struct {
    int64_t unix_seconds;  // UTC 秒级时间戳
    int16_t year;          // 本地公历年份
    uint8_t month;         // 月份，1—12
    uint8_t day;           // 日期，1—31
    uint8_t hour;          // 小时，0—23
    uint8_t minute;        // 分钟，0—59
    uint8_t second;        // 秒，0—60
    uint8_t weekday;       // 星期，0 表示周日
} xz_app_time_t;

typedef struct {
    int16_t x1, y1, x2, y2;  // 相对绘图区坐标，矩形含两端
    uint8_t kind;            // 0 直线，1 矩形，2 椭圆
    uint8_t width;           // 线宽，1—8
    uint8_t filled;          // 非零填充前景色，仅矩形与椭圆有效
    uint8_t reserved;        // 必须为零
} xz_app_draw_t;

typedef struct {
    int32_t x, y, z;  // 传感器坐标系加速度，单位 mg
} xz_app_acceleration_t;

typedef enum {
    XZ_APP_AUDIO_ASSET = 1,  // 应用资源节中的音频区域
    XZ_APP_AUDIO_SD = 2      // 产品 SD 根目录下的相对文件路径
} xz_app_audio_source_t;

typedef enum {
    XZ_APP_AUDIO_OGG_OPUS = 1, // Ogg 封装、单声道 Opus
    XZ_APP_AUDIO_WAV_PCM = 2 // RIFF/WAVE，16kHz 单声道 16 位 PCM
} xz_app_audio_format_t;

typedef enum {
    XZ_APP_AUDIO_STOPPED = 0,  // 未播放或已请求停止
    XZ_APP_AUDIO_LOADING,      // 后台准备文件与音频硬件
    XZ_APP_AUDIO_PLAYING,      // 正在输出音频
    XZ_APP_AUDIO_PAUSED,       // 应用暂停或页面暂不在前台
    XZ_APP_AUDIO_ENDED,        // 非循环播放已结束
    XZ_APP_AUDIO_ERROR         // 播放失败，原因见 result
} xz_app_audio_state_t;

typedef struct {
    uint32_t struct_size;  // 设置为本结构大小
    uint32_t source;       // xz_app_audio_source_t
    uint32_t format;       // xz_app_audio_format_t
    uint32_t loop;         // 非零循环播放
    const char* path;      // SD 相对路径，提交时复制；资源来源使用空指针
    uint32_t offset;       // 音频在资源节中的起始字节，SD 来源必须为零
    uint32_t length;       // 资源区域长度；零表示从 offset 到资源节末尾
} xz_app_audio_config_t;

typedef struct {
    uint32_t struct_size;  // 调用前设置容量，返回宿主结构大小
    uint32_t formats;      // 支持的 xz_app_audio_format_t 位掩码
    uint32_t sources;      // 支持的 xz_app_audio_source_t 位掩码
    uint32_t sample_rate;  // 当前输出采样率 Hz
    uint32_t channels;     // 当前输出声道数
} xz_app_audio_caps_t;

typedef struct {
    uint32_t struct_size;   // 调用前设置容量，返回宿主结构大小
    uint32_t state;         // xz_app_audio_state_t
    int32_t result;         // 最近的播放结果码
    uint32_t volume;        // 当前系统音量，0—100
    uint64_t position_ms;   // 当前循环内已提交至硬件的播放位置
    uint64_t duration_ms;   // 总时长，尚未解析时为零
    uint32_t loops;         // 已完成的循环次数
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

typedef enum {
    XZ_APP_IMAGE_GRAY16 = 0, // 16 阶灰度，实际支持范围由板型限定
    XZ_APP_IMAGE_GRAY8,      // 8 阶灰度量化，不切换驱动波形
    XZ_APP_IMAGE_DITHER,     // 黑白有序抖点
    XZ_APP_IMAGE_MONO        // 黑白阈值
} xz_app_image_mode_t;

typedef struct {
    uint32_t struct_size; // 设置为本结构大小
    uint32_t mode;        // xz_app_image_mode_t
    int32_t contrast;     // 相对对比度，-100—100；0 保留原始对比度
    int32_t threshold;    // 二值化阈值，0—255；-1 使用中点 128，仅纯黑白模式使用
    int32_t brightness;   // 亮度偏移，-100—100%；0 不调整
    int32_t exposure;     // 软件曝光，-200—200，单位 0.01 EV；0 不调整，不改变传感器
    int32_t shadows;      // 暗部调节，-100—100%；0 不调整
    int32_t highlights;   // 亮部调节，-100—100%；0 不调整
    int32_t gamma;        // Gamma，25—400，单位 0.01；100 不调整
    int32_t sharpness;    // -100—0 为柔化，0—100 为锐化；0 不调整
    uint32_t invert;      // 0 正常，1 灰度反相
    uint32_t mirror_x;    // 0 正常，1 水平镜像；在旋转前执行
    uint32_t mirror_y;    // 0 正常，1 垂直镜像；在旋转前执行
    uint32_t rotation;    // 顺时针角度，0、90、180 或 270
    uint32_t crop_x;      // 裁剪起点，原始 L8 图宽的万分比
    uint32_t crop_y;      // 裁剪起点，原始 L8 图高的万分比
    uint32_t crop_width;  // 裁剪宽度，1—10000，且 crop_x + crop_width <= 10000
    uint32_t crop_height; // 裁剪高度，1—10000，且 crop_y + crop_height <= 10000
} xz_app_image_effects_t;

typedef struct {
    uint32_t struct_size; // 调用前设置容量，返回宿主结构大小
    uint32_t loading;     // 非零表示后台解码尚未结束
    int32_t result;       // 最近请求结果，失败保留上一张图片
    uint32_t width;       // 当前显示图像宽度
    uint32_t height;      // 当前显示图像高度
    uint32_t gray_levels; // 板型原生灰阶数
    uint32_t threshold;   // 当前纯黑白阈值，默认中点 128
    uint32_t source_width; // 有界原始 L8 图宽度，未应用显示效果
    uint32_t source_height; // 有界原始 L8 图高度，未应用显示效果
    uint32_t revision;    // 每次显示像素更新递增，分块读取可据此检查图像是否变化
} xz_app_image_status_t;

typedef struct {
    uint32_t struct_size; // 调用前设置容量，返回宿主结构大小
    uint32_t running;     // 摄像头采集会话正在运行
    int32_t result;       // 最近采集结果
    uint32_t frames;      // 本次已交付的预览帧数，不代表屏幕物理刷新次数
} xz_app_camera_status_t;

typedef enum {
    XZ_APP_NET_HTTP = 1, // HTTP 或 HTTPS 请求
    XZ_APP_NET_WEBSOCKET, // WebSocket 或 WSS 消息
    XZ_APP_NET_TCP, // TCP 字节流
    XZ_APP_NET_TLS, // 验证服务端证书的 TLS 字节流
    XZ_APP_NET_UDP // 固定远端 UDP 数据报
} xz_app_net_protocol_t;

typedef enum {
    XZ_APP_NET_OPENING = 1, // 后台连接中
    XZ_APP_NET_CONNECTED, // 已连接，可提交发送
    XZ_APP_NET_COMPLETE, // HTTP 响应已完整接收
    XZ_APP_NET_CLOSED, // 对端关闭或应用取消，缓冲仍可读取
    XZ_APP_NET_ERROR // 失败，原因见 result 和 error_code
} xz_app_net_state_t;

typedef struct {
    const char* name; // HTTP 请求头名称
    const char* value; // HTTP 请求头值，提交时复制
} xz_app_net_header_t;

typedef struct {
    uint32_t struct_size; // 设置为本结构大小
    uint32_t protocol; // xz_app_net_protocol_t
    const char* address; // HTTP/WS 使用完整 URL；TCP/TLS/UDP 使用主机名或 IPv4
    uint32_t port; // TCP/TLS/UDP 远端端口；HTTP/WS 必须为零
    uint32_t timeout_ms; // 1000—60000ms；HTTP 为总期限，其他协议为连接及单次发送期限
    const char* method; // HTTP 方法 GET、POST、PUT、PATCH、DELETE 或 HEAD；其他协议为空
    const xz_app_net_header_t* headers; // HTTP/WS 请求头，最多 8 项
    uint32_t header_count; // 请求头数量
    const void* body; // HTTP 请求正文，提交时复制；长度不得超过 send_buffer_size
    uint32_t body_length; // HTTP 正文字节数；其他协议必须为零
    const char* ca_pem; // 可选 PEM 信任根，最多 8192 字节；空指针使用系统信任根
    uint32_t receive_buffer_size; // ELF 指定接收缓冲字节数，必须为正；WS/UDP 同时限定单条消息大小
    uint32_t send_buffer_size; // ELF 指定单条发送槽字节数；HTTP 用于校验请求正文长度，零表示不预留发送槽
    uint32_t response_size_limit; // ELF 指定 HTTP 响应总字节上限；零表示仅受接收缓冲和期限约束，其他协议必须为零
} xz_app_net_config_t;

typedef struct {
    uint32_t struct_size; // 调用前设置容量，返回宿主结构大小
    uint32_t state; // xz_app_net_state_t
    int32_t result; // XZ_APP_OK 或负错误码
    int32_t error_code; // 底层数值错误码，不含错误字符串
    uint32_t http_status; // HTTP 状态码，其他协议为零
    uint32_t pending_send; // 非零表示上一条发送尚未完成
    uint32_t next_length; // 下一条接收记录的字节数，零长度消息由 queued_messages 区分
    uint32_t queued_messages; // 待读记录数；TCP/HTTP 为接收分块，WS/UDP 保留消息边界
    uint32_t received_bytes; // 已接收并入队的字节数，饱和计数
    uint32_t sent_bytes; // 已完成发送的正文或负载字节数，饱和计数
    uint32_t worker_running; // 非零表示连接或后台清理尚未结束
} xz_app_net_status_t;

typedef enum {
    XZ_APP_NET_BINARY = 1u << 0 // WS 二进制消息；TCP/HTTP/UDP 接收记录也设置此位
} xz_app_net_flag_t;

typedef struct {
    uint32_t struct_size; // 设置为本结构大小
    const char* address; // 目标主机名或 IPv4，提交时复制
    uint32_t network_timeout_ms; // 准备网络及解析地址的总期限，1000—60000ms
    uint32_t count; // 探测次数；零表示持续到关闭句柄
    uint32_t interval_ms; // 相邻探测开始时间的最小间隔，零表示不额外等待
    uint32_t timeout_ms; // 单次回复等待上限，必须为正
    uint32_t data_size; // ICMP 负载字节数，0—65507，不含协议头
    uint32_t ttl; // IPv4 TTL，1—255
} xz_app_ping_config_t;

typedef struct {
    uint32_t struct_size; // 调用前设置容量，返回宿主结构大小
    char address[16]; // 已解析的 IPv4 文本，未完成解析时为空
    uint32_t sent; // 成功发出的 ICMP Echo 数量，饱和计数
    uint32_t received; // 已校验的 ICMP Echo Reply 数量，饱和计数
    uint32_t timeouts; // 未在期限内收到回复的次数，饱和计数
    uint32_t sequence; // 最近发出的 ICMP 序号，1—65535 后回绕
    uint32_t ttl; // 最近一次有效回复的 IPv4 TTL
    uint32_t last_rtt_ms; // 最近一次有效回复的往返耗时
    uint32_t min_rtt_ms; // 有效回复的最小往返耗时，无回复时为零
    uint32_t max_rtt_ms; // 有效回复的最大往返耗时
    uint32_t average_rtt_ms; // 有效回复的平均往返耗时
    uint32_t elapsed_ms; // 解析或 Ping 工作耗时，不含网络准备时间，饱和计数
} xz_app_net_diagnostics_t;

typedef struct {
    uint32_t abi_version;                                                                 // 宿主接口版本
    uint32_t struct_size;                                                                 // 宿主接口结构大小
    void* root;                                                                           // 宿主管理的应用内容容器
    int width;                                                                            // 内容区宽度
    int height;                                                                           // 内容区高度
    const char* language;                                                                 // 当前界面语言代码
    void* (*label)(void* parent, int x, int y, int width, int height, const char* text);  // 创建文本
    void* (*button)(void* parent, int x, int y, int width, int height, const char* text, xz_app_click_t click,
                    void* context);  // 创建按钮并登记回调，自动绑定系统点击震动
    void (*set_text)(void* label, const char* text);  // 复制并更新标签、按钮或输入框文本
    void* asset_context;                              // 资源读取上下文，仅应用存活期间有效
    uint32_t asset_size;                              // ELF 中非加载资源节的长度
    uint32_t (*read_asset)(void* context, uint32_t offset, void* buffer,
                           uint32_t length);                       // 同步读取指定资源范围，失败返回零
    void (*set_style)(void* widget, const xz_app_style_t* style);  // 可选控件样式，使用前检查 struct_size
    void (*set_enabled)(void* widget, int enabled);                // 可选控件可用状态，使用前检查 struct_size
    int (*get_local_date)(int* year, int* month, int* day);        // 可选设备本地日期，时钟未就绪返回零
    uint32_t capabilities;                                         // 可选能力位，不表示硬件当前就绪
    void* host_context;                                            // 托管接口共用上下文，仅本次运行有效
    int (*get_local_time)(xz_app_time_t* result);                  // 返回结果码，时间未就绪返回 NOT_READY
    uint64_t (*monotonic_ms)(void);                                // 本次启动的单调毫秒数，不保证跨重启连续
    uint32_t (*timer_start)(void* host, uint32_t interval_ms, int repeat, xz_app_click_t callback,
                            void* context);        // 200ms—24h，最多 8 个，失败为零
    int (*timer_cancel)(void* host, uint32_t id);  // 取消托管定时器
    int (*storage_read)(void* host, const char* key, void* buffer,
                        uint32_t capacity);  // 返回字节数或负错误码，空 buffer 查询大小
    int (*storage_write)(void* host, const char* key, const void* data,
                         uint32_t length);               // 每键最多 16KiB，每应用最多 32 键
    int (*storage_remove)(void* host, const char* key);  // 删除私有数据键
    void* (*container)(void* parent, int x, int y, int width, int height, int scroll_y);  // 非零 scroll_y 允许纵向滚动
    void* (*input)(void* parent, int x, int y, int width, int height, const char* text, uint32_t max_chars,
                   xz_app_click_t submit, void* context);            // 最多 256 字符，确认回调可为空
    int (*get_text)(void* widget, char* buffer, uint32_t capacity);  // 返回 UTF-8 字节数或负错误码，容量包含终止符
    void* (*keyboard)(void* host, void* parent, void* input);        // 绑定系统键盘，每应用最多一个
    int (*keyboard_close)(void* host);                               // 删除键盘并取消待处理确认
    void* (*progress)(void* parent, int x, int y, int width, int height);    // 创建 0—100 进度条
    void (*set_progress)(void* widget, uint32_t percent);                    // 无动画更新进度
    int (*set_on_exit)(void* host, xz_app_click_t callback, void* context);  // 替换或清除单个退出回调
    void (*notify)(void* host, const char* text);                            // 系统顶部提示，调用时复制文字
    int (*set_keep_awake)(void* host, int enabled);  // 阻止自动待机；不保网，离页时自动释放
    void (*clear_children)(void* parent);            // 删除子控件，不删除 parent；不得在绘制事件内调用
    void* (*drawing)(void* parent, int x, int y, int width, int height);  // 黑白绘图区，最大 1024×1600
    int (*draw)(void* widget, const xz_app_draw_t* commands, uint32_t count);  // 复制至宿主，最多 1024 条；零条清空
    int (*set_acceleration)(void* host, int enabled);  // 前台采样订阅，离页与待机自动停止
    int (*get_acceleration)(void* host, xz_app_acceleration_t* result);  // 最近 500ms 样本，未就绪返回 NOT_READY
    uint64_t (*foreground_ms)(void* host);  // 前台累计毫秒；待机与离页不累计
    int (*set_on_back)(void* host, xz_app_back_t callback, void* context);  // 返回键优先交应用处理，零表示使用宿主默认返回
    int (*request_screen_change)(void* host);  // 下一次物理提交先推白，再显示目标画面；不阻塞 UI
    int (*vibrate)(void* host, uint32_t duration_ms);  // 请求 1—15000ms 单次震动，尊重系统震动开关；未结束时不叠加
    int (*audio_get_caps)(void* host, xz_app_audio_caps_t* result);  // 查询实际支持的音频能力
    int (*audio_play)(void* host, const xz_app_audio_config_t* config);  // 后台分块播放；返回成功表示请求已接纳
    int (*audio_pause)(void* host, int paused);                          // 非零暂停，零继续；不重新开始当前音频
    int (*audio_stop)(void* host);                                       // 请求停止并后台归还音频与文件资源
    int (*audio_set_volume)(void* host, uint32_t volume);  // 设置系统音量，0—100，与系统音量键共用
    int (*audio_get_status)(void* host, xz_app_audio_status_t* result);  // 查询播放状态、结果、进度与系统音量
    int (*record_get_caps)(void* host, xz_app_audio_caps_t* result); // 查询录音格式、采样率及声道
    int (*record_start)(void* host); // 单录音槽；成功保存后替换上一条录音
    int (*record_pause)(void* host, int paused); // 非零暂停写入，零继续；保留麦克风占用
    int (*record_stop)(void* host); // 异步停止并保存；待机或离页时宿主也会停止保存
    int (*record_get_status)(void* host, xz_app_record_status_t* result); // 查询状态、时长与播放路径
    int (*request_full_refresh)(void* host); // 异步请求下一次提交完整刷新，重复请求合并
    void* (*image)(void* host, void* parent, int x, int y, int width, int height); // 创建托管图片区域
    int (*image_load_asset)(void* host, void* widget, uint32_t offset, uint32_t length); // 后台解码 ELF 资源中的图片
    int (*image_set_effects)(void* host, void* widget, const xz_app_image_effects_t* effects); // 更新显示副本，不重采摄像头
    int (*image_get_status)(void* host, void* widget, xz_app_image_status_t* result); // 查询解码结果及实际灰阶能力
    int (*camera_preview)(void* host, void* widget, int enabled); // 将最新灰阶取景交给图片区域；零停止
    int (*camera_get_status)(void* host, xz_app_camera_status_t* result); // 查询采集运行状态与结果
    int (*image_get_effects)(void* host, void* widget, xz_app_image_effects_t* result); // 查询当前显示处理参数
    int (*image_read_pixels)(void* host, void* widget, uint32_t processed, uint32_t offset,
                              void* buffer, uint32_t length); // 读紧排 L8，processed 为 0 原图或 1 显示图；每次最多 64 KiB，返回字节数
    int (*image_set_pixels)(void* host, void* widget, uint32_t width, uint32_t height,
                             const void* pixels, uint32_t length); // 复制紧排 L8 为新原图；面积最多 1 Mi 像素，长度须等于宽×高
    int (*network_open)(void* host, const xz_app_net_config_t* config, uint32_t* id); // 异步连接或请求；成功写入非零句柄
    int (*network_send)(void* host, uint32_t id, const void* data, uint32_t length, uint32_t flags); // 复制负载后异步发送；不支持 HTTP
    int (*network_get_status)(void* host, uint32_t id, xz_app_net_status_t* result); // 查询状态和下一条记录长度
    int (*network_read)(void* host, uint32_t id, void* buffer, uint32_t capacity, uint32_t* flags); // TCP/TLS 按容量分次读取；其他协议读取完整记录，容量不足不出队；无记录返回 NOT_READY
    int (*network_get_header)(void* host, uint32_t id, const char* name, char* buffer, uint32_t capacity); // 读取宿主保留的 HTTP 响应头，返回长度或负错误码
    int (*network_close)(void* host, uint32_t id); // 非阻塞取消并立即撤销句柄，后台完成后归还网络资源
    int (*network_set_idle_timer)(void* host, int enabled); // 默认关闭空闲断网计时；开启后在无活动网络工作时遵循系统计时
    int (*network_prepare)(void* host, uint32_t timeout_ms, uint32_t* id); // 异步准备网络，不发业务请求；复用网络状态与关闭接口
    int (*network_resolve)(void* host, const char* hostname, uint32_t timeout_ms, uint32_t* id); // 异步解析首个 IPv4；通过网络状态和诊断结果接口读取
    int (*network_ping)(void* host, const xz_app_ping_config_t* config, uint32_t* id); // 异步 ICMP Echo；通过 network_close 取消
    int (*network_get_diagnostics)(void* host, uint32_t id, xz_app_net_diagnostics_t* result); // 查询 DNS 地址或 Ping 统计；按调用方结构容量复制
} xz_app_api_t;

/**
 * @brief 创建应用界面
 * @note 宿主通过 argv[0] 传入 xz_app_api_t；所有接口及回调仅在 UI 线程使用，不得阻塞、持有退出后的引用或启动后台任务。
 */
int app_main(int argc, char** argv);
