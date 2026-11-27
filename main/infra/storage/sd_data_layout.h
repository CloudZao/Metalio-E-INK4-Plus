#pragma once

#include <string>

/** @brief 生成书籍辅助数据路径；已有数据沿用原位置，无数据时使用新目录。 */
std::string SdDataBookPath(const char* book, const char* suffix);

/** @brief 生成壁纸名称元数据路径；已有数据沿用原位置，无数据时使用新目录。 */
std::string SdDataWallpaperMetaPath(const char* wallpaper);

/** @brief 创建阅读应用的独占工作文件路径。 */
std::string SdDataBookWorkPath(const char* kind, unsigned id);

/** @brief 挂载后补齐目录、清理分页工作文件并恢复壁纸保存备份，不扫描书库。 */
void SdDataPrepare();

/** @brief 清理指定前缀的八位十六进制临时文件；失败返回 false，可重试。
 * @note 调用方须排除同前缀的活动文件。
 */
bool SdDataCleanWorkFiles(const char* root, const char* prefix);

/** @brief 按已知路径删除书籍辅助数据，不删除正文或遍历书籍目录。
 * @note 带未知会话编号的旧工作文件不在单书删除时查找，删除所属目录时一并清理。
 */
bool SdDataDeleteBookData(const char* book);

/** @brief 删除壁纸名称信息及其旧位置副本，不删除图片。 */
bool SdDataDeleteWallpaperData(const char* wallpaper);

/** @brief 文件或目录移动成功后同步辅助数据的路径映射。 */
bool SdDataRelocate(const char* from, const char* to);
