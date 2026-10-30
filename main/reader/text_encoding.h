#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace reader {

/** @brief 检查样本是否为合法 UTF-8；允许末尾字符被采样边界截断 */
bool LooksLikeUtf8(const uint8_t* data, size_t len);

/** @brief 将 UTF-8 或 GB18030 字节转为 UTF-8，去除文件头 BOM 和回车 */
bool BytesToUtf8(const uint8_t* data, size_t len, bool as_gb18030, std::string& out);

/** @brief 从 UTF-8 取下一个码点；非法序列返回一个字节并输出问号 */
size_t Utf8Next(const uint8_t* s, size_t len, uint32_t* cp);

/**
 * @brief 从 GB18030（兼容 GBK/GB2312）取下一个 Unicode 码点
 * @return 消耗的源字节数（1～4）；合法字符占 1、2 或 4 字节，残缺字符消耗其有效前缀并输出问号
 * @note 调用方须提供至少四字节预读，或文件末尾的全部剩余字节。
 */
size_t Gb18030Next(const uint8_t* s, size_t len, uint32_t* cp);

}  // namespace reader
