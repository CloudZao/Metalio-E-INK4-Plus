#pragma once

// 统一保护 VFS/FatFS 访问与 SD 生命周期，覆盖第三方解码器和标准文件接口。
namespace sd_access {
/** @brief 暂停普通新访问，允许已有句柄与受控待机任务收尾 */
void Quiesce();
/** @brief 解除待机访问限制，不改变挂载或介质切换状态 */
void Resume();
/** @brief 查询待机访问限制 */
bool IsQuiescing();
/** @brief 查询是否没有在途操作、事务及打开的文件或目录 */
bool IsIdle();
/** @brief 返回待完成的访问计数 */
unsigned Pending();

/** @brief 更新本机文件系统可用状态，不改变待机访问限制 */
void SetMounted(bool mounted);

/**
 * @brief 串行化挂载、卸载与介质切换；等待在途访问，超时则不执行变更
 * @note 同任务允许嵌套；持有 Transaction 时拒绝升级，避免等待自身。
 */
class Transition {
public:
    explicit Transition(unsigned timeout_ms = 1000);
    ~Transition();
    explicit operator bool() const { return acquired_; }
    Transition(const Transition&) = delete;
    Transition& operator=(const Transition&) = delete;
private:
    bool acquired_ = false; // 已持有本次变更权限
};

/**
 * @brief 允许受控待机任务在暂停访问后完成图片解码
 * @note 不绕过挂载状态及介质切换保护，已有句柄仍可收尾。
 */
class Permit {
public:
    Permit();
    ~Permit();
    Permit(const Permit&) = delete;
    Permit& operator=(const Permit&) = delete;
private:
    bool acquired_ = false; // 已登记当前任务的临时权限
};

/** @brief 保护多文件事务的完整生命周期，覆盖关闭文件后的重命名与清理 */
class Transaction {
public:
    Transaction();
    ~Transaction();
    explicit operator bool() const { return admitted_; }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
private:
    bool admitted_; // 本事务已通过访问许可检查
};
}
