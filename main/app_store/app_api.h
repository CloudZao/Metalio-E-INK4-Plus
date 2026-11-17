#pragma once
#include <stdint.h>
#include <stddef.h>

#define XZ_APP_ABI_VERSION 1 // 应用接口版本

// 检查可选字段边界后，仍须检查函数指针非空。
#define XZ_APP_HAS_FIELD(api, member) ((api) && (api)->struct_size >= offsetof(xz_app_api_t, member) + sizeof((api)->member))
typedef void (*xz_app_click_t)(void* context); // 按钮点击回调
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
    XZ_APP_ERR_MEMORY = -6 // 分配失败
} xz_app_result_t;

typedef enum {
    XZ_APP_CAP_TIME = 1u << 0, // 本地时间与单调计时
    XZ_APP_CAP_TIMER = 1u << 1, // 托管前台定时器
    XZ_APP_CAP_STORAGE = 1u << 2, // 私有存储
    XZ_APP_CAP_WIDGETS = 1u << 3, // 基础控件
    XZ_APP_CAP_EXIT = 1u << 4, // 退出回调
    XZ_APP_CAP_NOTICE = 1u << 5, // 顶部提示
    XZ_APP_CAP_KEEP_AWAKE = 1u << 6 // 阻止无操作自动待机，不阻止电源键待机
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
    uint32_t abi_version; // 宿主接口版本
    uint32_t struct_size; // 宿主接口结构大小
    void* root; // 宿主管理的应用内容容器
    int width; // 内容区宽度
    int height; // 内容区高度
    const char* language; // 当前界面语言代码
    void* (*label)(void* parent, int x, int y, int width, int height, const char* text); // 创建文本
    void* (*button)(void* parent, int x, int y, int width, int height, const char* text,
                    xz_app_click_t click, void* context); // 创建按钮并登记回调
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
} xz_app_api_t;

/**
 * @brief 创建应用界面
 * @note 宿主通过 argv[0] 传入 xz_app_api_t；所有接口及回调仅在 UI 线程使用，不得阻塞、持有退出后的引用或启动后台任务。
 */
int app_main(int argc, char** argv);
