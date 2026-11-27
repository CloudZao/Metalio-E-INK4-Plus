#pragma once
#include "infra/storage/storage_error.h"

// 统一保护 VFS/FatFS 访问与 SD 生命周期，覆盖第三方解码器和标准文件接口。

/** @brief 启动时登记同步缓存释放回调，回调不得递归申请介质切换 */
void SdAccessConfigureCacheRelease(void (*release)());

/** @brief 暂停普通新访问，允许已有句柄与受控待机任务收尾 */
void SdAccessQuiesce();

/** @brief 解除待机访问限制，不改变挂载或介质切换状态 */
void SdAccessResume();

/** @brief 查询待机访问限制 */
bool SdAccessIsQuiescing();

/** @brief 查询是否没有在途操作、事务及打开的文件或目录 */
bool SdAccessIsIdle();

/** @brief 返回待完成的访问计数 */
unsigned SdAccessPending();

/** @brief 更新本机文件系统可用状态，不改变待机访问限制 */
void SdAccessSetMounted(bool mounted, StorageError unavailable = {});

/** @brief 返回当前介质访问状态快照；实际操作仍须申请事务。 */
StorageError SdAccessStatus();

/** @brief 获取介质挂载代次，用于作废跨挂载缓存 */
unsigned SdAccessMountGeneration();

/**
 * @brief 串行化挂载、卸载与介质切换；等待在途访问，超时则不执行变更
 * @note 同任务允许嵌套；持有 Transaction 时拒绝升级，避免等待自身。
 */
class SdAccessTransition {
public:
    /** @brief 等待在途操作排空并申请介质切换权限，超时后可通过布尔转换检查失败 */
    explicit SdAccessTransition(unsigned timeout_ms = 1000);

    /** @brief 归还当前作用域持有的介质切换权限 */
    ~SdAccessTransition();

    /** @brief 查询本次介质切换权限是否申请成功 */
    explicit operator bool() const {
        return acquired_;
    }

    SdAccessTransition(const SdAccessTransition&) = delete;
    SdAccessTransition& operator=(const SdAccessTransition&) = delete;

private:
    bool acquired_ = false;  // 已持有本次变更权限
};

/**
 * @brief 允许受控待机任务在暂停访问后完成图片解码
 * @note 不绕过挂载状态及介质切换保护，已有句柄仍可收尾。
 */
class SdAccessPermit {
public:
    /** @brief 为当前任务登记受控待机访问权限，离开作用域时自动归还 */
    SdAccessPermit();

    /** @brief 归还当前任务的受控待机访问权限 */
    ~SdAccessPermit();
    SdAccessPermit(const SdAccessPermit&) = delete;
    SdAccessPermit& operator=(const SdAccessPermit&) = delete;

private:
    bool acquired_ = false;  // 已登记当前任务的临时权限
};

/** @brief 预留可跨任务交接的异步 SD 访问权限。 */
class SdAccessDeferredTransaction {
public:
    /** @brief 预留异步事务，计入卸卡等待；可由另一任务接管。 */
    SdAccessDeferredTransaction();

    /** @brief 撤销尚未被执行任务接管的异步事务预留 */
    ~SdAccessDeferredTransaction();

    /** @brief 查询是否仍持有可交接的异步事务预留 */
    explicit operator bool() const {
        return admitted_;
    }

    SdAccessDeferredTransaction(const SdAccessDeferredTransaction&) = delete;
    SdAccessDeferredTransaction& operator=(const SdAccessDeferredTransaction&) = delete;

private:
    friend class SdAccessTransaction;
    bool admitted_ = false;  // 尚未被执行任务接管的预留
};

/** @brief 保护多文件事务的完整生命周期，覆盖关闭文件后的重命名与清理。 */
class SdAccessTransaction {
public:
    /** @brief 为当前任务申请完整文件事务权限，可通过布尔转换检查结果 */
    SdAccessTransaction();

    /** @brief 接管已预留事务，允许在待机或卸卡等待期间完成已接纳的写入。 */
    explicit SdAccessTransaction(SdAccessDeferredTransaction& deferred);

    /** @brief 归还文件事务及当前任务的关联访问权限 */
    ~SdAccessTransaction();

    /** @brief 查询本次文件事务是否已获准执行 */
    explicit operator bool() const {
        return admitted_;
    }

    /** @brief 返回本次申请失败原因，不受后续介质状态变化影响。 */
    const StorageError& Error() const { return error_; }

    SdAccessTransaction(const SdAccessTransaction&) = delete;
    SdAccessTransaction& operator=(const SdAccessTransaction&) = delete;

private:
    StorageError error_; // 申请失败时的原因快照
    bool admitted_;  // 本事务已通过访问许可检查
};
