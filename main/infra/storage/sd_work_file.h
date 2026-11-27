#pragma once
#include <cstddef>

enum class SdWorkArea { Transfer, Books, Wallpaper, Image };  // 工作文件归属

/** @brief 获取工作区固定路径，无效归属返回空指针。 */
const char* SdWorkDirectory(SdWorkArea area);

/**
 * @brief 创建独占工作文件并返回文件描述符；失败返回 -1，输出路径清空。
 * @note 调用方须持有 SD 事务，负责关闭和删除；前缀只允许字母、数字、下划线和连字符。
 */
int SdCreateWorkFile(SdWorkArea area, const char* prefix, char* path, size_t capacity);
