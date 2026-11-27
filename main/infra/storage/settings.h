#ifndef SETTINGS_H
#define SETTINGS_H

#include <cstdint>
#include <string>
#include <nvs_flash.h>

class Settings {
public:
    /** @brief 打开指定配置域；只读模式下不存在的配置域按空配置处理 */
    Settings(const std::string& ns, bool read_write = false);

    /** @brief 释放配置句柄，尚未显式提交的修改在退出时尝试提交 */
    ~Settings();
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    /** @brief 读取字符串配置，不存在或读取失败时返回默认值 */
    std::string GetString(const std::string& key, const std::string& default_value = "");

    /** @brief 写入字符串配置，成功暂存时返回 true */
    bool SetString(const std::string& key, const std::string& value);

    /** @brief 读取整数配置，不存在或读取失败时返回默认值 */
    int32_t GetInt(const std::string& key, int32_t default_value = 0);

    /** @brief 写入整数配置，成功暂存时返回 true */
    bool SetInt(const std::string& key, int32_t value);

    /** @brief 读取布尔配置，不存在或读取失败时返回默认值 */
    bool GetBool(const std::string& key, bool default_value = false);

    /** @brief 写入布尔配置，成功暂存返回 true */
    bool SetBool(const std::string& key, bool value);

    /** @brief 删除配置项，不存在视为成功 */
    bool EraseKey(const std::string& key);

    /** @brief 删除当前命名空间全部配置，成功暂存返回 true */
    bool EraseAll();

    /** @brief 提交配置；此前任一写入失败或提交失败返回 false，提交失败允许显式重试 */
    bool Commit();

private:
    nvs_handle_t nvs_handle_ = 0;  // 独占的配置句柄，打开失败时为零
    bool read_write_ = false;
    bool dirty_ = false;
    bool write_failed_ = false;   // 本批次任一写入失败后，不将部分成功报告为整批成功
    bool commit_failed_ = false;  // 显式提交失败后由调用方决定重试，析构不重复提交
};

#endif
