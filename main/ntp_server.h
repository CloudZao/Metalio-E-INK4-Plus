#pragma once

#include <atomic>
#include <ctime>
#include <mutex>
#include <string>

class NtpServer {
public:
    struct Result {
        std::string timezone; // IP 所在地区的时区标识，缓存可能只有偏移
        int offset_seconds = 0; // 当前 UTC 偏移，含夏令时
        bool timezone_from_cache = false; // 本次定位失败，沿用上次偏移
        time_t utc_time = 0; // 校时成功后的 UTC 时间
        struct tm local_time = {}; // 按当前偏移转换的当地时间
    };

    /** @brief 获取网络校时单例 */
    static NtpServer& GetInstance();
    /** @brief 在恢复 RTC 前加载已保存的时区偏移 */
    void Initialize();
    /**
     * @brief 查询 IP 时区并通过 NTP 校时，成功返回时区和时间
     * @param ip 空字符串查询当前公网出口，否则仅接受 IPv4 或 IPv6 地址
     * @param result 成功时返回校时结果
     * @return 时区可用且 NTP 校时成功返回 true
     * @note 仅查询当前公网出口失败时允许使用缓存；调用会更新系统 UTC 和显示时区
     */
    bool Sync(const std::string& ip, Result& result);
    /** @brief 保存并应用云端或定位接口提供的当前时区偏移 */
    bool SetTimezone(int offset_seconds, const std::string& timezone = "");
    /** @brief 获取当前 UTC 偏移秒数，未知时返回零 */
    int UtcOffset() const { return offset_seconds_.load(); }

private:
    NtpServer() = default;
    bool SetTimezoneLocked(int offset_seconds, const std::string& timezone);
    bool QueryTimezone(const std::string& ip, std::string& timezone, int& offset);
    std::mutex mutex_;
    std::atomic<int> offset_seconds_{0};
    std::string timezone_;
    bool timezone_valid_ = false;
};
