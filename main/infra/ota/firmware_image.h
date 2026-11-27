#pragma once
#include <cstddef>
#include <functional>
#include <string>

class FirmwareImage {
public:
    /** @brief 确认当前运行镜像，结束待验证状态的回滚窗口 */
    static void MarkCurrentVersionValid();

    /** @brief 下载、校验并写入非运行分区；成功后选择新启动分区 */
    bool StartUpgradeFromUrl(const std::string& url, std::function<void(int, size_t)> progress);

    /** @brief 从本地镜像写入非运行分区，不删除源文件 */
    bool StartUpgradeFromFile(const std::string& path, std::function<void(int, size_t)> progress);

private:
    bool Upgrade(const std::string& url);
    bool UpgradeFromFile(const std::string& path);
    std::function<void(int, size_t)> upgrade_callback_;  // 当前写入进度，不持有业务状态
};
