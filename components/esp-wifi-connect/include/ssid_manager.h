#ifndef SSID_MANAGER_H
#define SSID_MANAGER_H

#include <mutex>
#include <string>
#include <vector>

struct SsidItem {
    std::string ssid; // 网络名称
    std::string password; // 网络密码，空串表示开放网络
};

class SsidManager {
public:
    /** @brief 获取无线网络配置管理器 */
    static SsidManager& GetInstance() {
        static SsidManager instance;
        return instance;
    }
    enum class ReadResult {
        Ok, // 已读取完整文件
        Missing, // 文件确实不存在
        Invalid, // 文件内容非法，可从有效 NVS 修复
        Error, // 存储不可用或读取失败
    };
    using ReadStorage = ReadResult (*)(std::string&);
    using WriteStorage = bool (*)(const std::string&);
    /** @brief 注册存储入口并执行一次开机同步，须在网络启动前调用 */
    void InitializeStorage(ReadStorage read, WriteStorage write);
    /** @brief 重试未完成的写卡，不在运行中导入卡配置 */
    void RetryStorage();
    /** @brief 保存网络，新网络置于列表首位 */
    void AddSsid(const std::string& ssid, const std::string& password);
    /** @brief 删除指定名称的网络 */
    void RemoveSsid(const std::string& ssid);
    /** @brief 将指定名称的网络设为默认网络 */
    void SetDefaultSsid(const std::string& ssid);
    /** @brief 清空已保存的网络 */
    void Clear();
    /** @brief 获取线程安全的网络配置快照 */
    std::vector<SsidItem> GetSsidList() const;

private:
    SsidManager();
    void LoadFromNvs();
    bool SaveToNvs(bool pending);
    void SaveChange(std::vector<SsidItem> previous);
    void SyncStorage();
    mutable std::mutex mutex_;
    std::vector<SsidItem> ssid_list_;
    bool nvs_ready_ = false;
    bool pending_ = false;
    ReadStorage read_ = nullptr;
    WriteStorage write_ = nullptr;
};
#endif
