#ifndef _SYSTEM_INFO_H_
#define _SYSTEM_INFO_H_

#include <cstddef>
#include <string>
class SystemInfo {
public:
    /** @brief 查询 Flash 容量，读取失败返回 0 */
    static size_t GetFlashSize();

    /** @brief 查询系统运行以来的最小空闲堆容量，单位字节 */
    static size_t GetMinimumFreeHeapSize();

    /** @brief 查询当前空闲堆容量，单位字节 */
    static size_t GetFreeHeapSize();

    /** @brief 获取 Wi-Fi 站点 MAC 标识，读取失败返回空字符串 */
    static std::string GetMacAddress();

    /** @brief 获取构建目标芯片名称 */
    static std::string GetChipModelName();

    /** @brief 获取板型与固件版本组成的客户端标识 */
    static std::string GetUserAgent();
};

#endif  // _SYSTEM_INFO_H_
