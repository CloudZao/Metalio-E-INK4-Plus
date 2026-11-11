#pragma once

#include <string>

namespace sd_data {
/** @brief 生成书籍辅助数据路径；已有数据沿用原位置，无数据时使用新目录。 */
std::string BookPath(const char* book, const char* suffix);
/** @brief 生成壁纸名称元数据路径；已有数据沿用原位置，无数据时使用新目录。 */
std::string WallpaperMetaPath(const char* wallpaper);
/** @brief 创建阅读应用的独占工作文件路径。 */
std::string BookWorkPath(const char* kind, unsigned id);
/** @brief 挂载后补齐目录、清理分页工作文件并恢复壁纸保存备份，不扫描书库。 */
void Prepare();
/** @brief 清理指定前缀的八位十六进制临时文件；失败返回 false，可重试。
 * @note 调用方须排除同前缀的活动文件。
 */
bool CleanWorkFiles(const char* root, const char* prefix);
/** @brief 删除书籍辅助数据，不删除书籍正文。 */
bool DeleteBookData(const char* book);
/** @brief 删除壁纸名称信息及其旧位置副本，不删除图片。 */
bool DeleteWallpaperData(const char* wallpaper);
/** @brief 文件或目录移动成功后同步辅助数据的路径映射。 */
bool Relocate(const char* from, const char* to);
}
