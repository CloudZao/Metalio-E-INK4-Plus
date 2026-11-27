#pragma once
#include <atomic>
#include <string>
#include <functional>

enum class FirmwareCheckStatus {
    Deferred,   // 系统暂停、已有查询或页面已经退出
    Failed,     // 查询失败或版本元数据无效
    Current,    // 无可用新版本或未配置升级入口
    Available,  // 新版本可用，由界面确认是否升级
};

struct FirmwareCheckResult {
    FirmwareCheckStatus status = FirmwareCheckStatus::Failed;  // 查询结果
    std::string current;                                       // 当前固件版本
    std::string version;                                       // 可用固件版本
    std::string url;                                           // 下载地址，不得输出日志
};

class FirmwareUpdate {
public:
    /** @brief 获取固件检查与升级用例的唯一所有者 */
    static FirmwareUpdate& GetInstance();

    /** @brief 在启动时绑定升级前的会话收尾入口 */
    void Initialize(bool (*prepare_update)(), void (*resume_conversation)());

    /** @brief 确认当前运行镜像，须在平台存储初始化后调用 */
    void PrepareBoot();

    /** @brief 处理版本检查；占用、暂停或待机时保留重试责任 */
    bool CheckVersion();

    /** @brief 在网络工作线程查询版本；结果回调在该线程调用，页面需自行转交 UI 线程 */
    bool RequestVersionCheck(std::function<bool()> still_needed, std::function<void(FirmwareCheckResult)> completed);

    /** @brief 查询本次启动的版本检查是否已处理 */
    bool VersionChecked() const;

    /** @brief 查询升级是否已接纳或正在执行 */
    bool IsBusy() const {
        return busy_.load(std::memory_order_acquire);
    }

    /** @brief 在内部 RAM 启动线程检查 SD 固件并执行升级 */
    bool TrySdCard();

    /** @brief 请求从指定地址升级；接纳结果不代表升级完成 */
    bool RequestFromUrl(const std::string& url, const std::string& current_version, const std::string& new_version);

    /** @brief 请求手动地址升级，版本文案由升级模块提供 */
    bool RequestManual(const std::string& url);

private:
    FirmwareUpdate() = default;
    FirmwareCheckResult QueryVersion(const std::function<bool()>& still_needed);
    bool UpgradeFromUrl(const std::string& url, const std::string& current_version, const std::string& new_version);
    void FinishUpgrade();
    bool PrepareExecution();
    bool (*prepare_update_)() = nullptr;        // 归还会话资源并返回是否需要失败恢复
    void (*resume_conversation_)() = nullptr;   // 升级失败后按页面意图重新建立会话
    bool restore_conversation_ = false;         // 仅升级执行任务访问
    std::atomic<bool> busy_{false};             // 包含已入队但尚未开始的升级
    std::atomic<bool> checking_{false};         // 查询的排队和执行占用，与镜像写入独立
    std::atomic<bool> version_checked_{false};  // 有界检查已处理，取消不置位
};
