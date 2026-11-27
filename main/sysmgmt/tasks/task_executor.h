#pragma once
#include <cstdint>
#include <functional>

enum class TaskDomain : uint8_t {
    Voice,    // 协议和语音会话，串行执行
    Network,  // 联网、激活及固件请求
    Storage,  // 使用内部 RAM 栈的存储操作
    Audio,    // 本地音频服务的申请和释放
    Cleanup,  // 网络取消与资源回收，使用 PSRAM 栈，不执行 NVS 操作
    NetworkIo, // 较长的网络作业，使用PSRAM栈，不阻塞联网控制
    Image,    // 本地解码与图片保存，使用PSRAM栈，与联网控制及存储提交独立
    Transfer, // 文件传输及提交，内部RAM栈；不占用配置保存或联网控制通道
};

class TaskExecutor {
public:
    /** @brief 投递有界后台任务；失败时由调用方决定重试，捕获对象在队列锁外释放 */
    static bool Schedule(std::function<void()> callback, TaskDomain domain = TaskDomain::Voice);

    /** @brief 判断当前线程是否为指定领域的串行工作线程 */
    static bool IsCurrent(TaskDomain domain);

    /**
     * @brief 由领域资源的唯一所有者控制空闲线程保留
     * @note 硬件会话启动后保留其控制任务，完成收尾后才允许空闲退出。
     */
    static void SetRetained(TaskDomain domain, bool retained);
};
