#pragma once

#include <string>

/**
 * OTA「忽略本次升级」持久化（NVS）。
 * 存被忽略的目标固件版本号；开机同版本不再弹确认框。
 * 「下次提醒我」不写此处。
 */
namespace OtaIgnore {

std::string Get();
void Set(const std::string& firmware_version);
void Clear();
bool IsIgnored(const std::string& firmware_version);

}  // namespace OtaIgnore
