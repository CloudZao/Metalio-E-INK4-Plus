#ifndef SETTINGS_H
#define SETTINGS_H

#include <string>
#include <nvs_flash.h>

class Settings {
public:
    Settings(const std::string& ns, bool read_write = false);
    ~Settings();

    std::string GetString(const std::string& key, const std::string& default_value = "");
    void SetString(const std::string& key, const std::string& value);
    int32_t GetInt(const std::string& key, int32_t default_value = 0);
    /** @brief 写入整数配置，成功暂存时返回 true */
    bool SetInt(const std::string& key, int32_t value);
    /** @brief 提交尚未保存的配置，提交成功时返回 true */
    bool Commit();
    bool GetBool(const std::string& key, bool default_value = false);
    void SetBool(const std::string& key, bool value);
    void EraseKey(const std::string& key);
    void EraseAll();

private:
    std::string ns_;
    nvs_handle_t nvs_handle_ = 0;
    bool read_write_ = false;
    bool dirty_ = false;
    bool commit_failed_ = false; // 保留显式提交失败状态，避免析构重复提交
};

#endif
