#pragma once

#include <string>

/** @brief 保存忽略的目标版本，开机同版本不再提示；下次提醒不写入 */
void FirmwareIgnoreSet(const std::string& firmware_version);

/** @brief 升级成功后清除忽略记录 */
void FirmwareIgnoreClear();

/** @brief 查询目标版本是否与已有忽略记录一致 */
bool FirmwareIgnoreIsIgnored(const std::string& firmware_version);
