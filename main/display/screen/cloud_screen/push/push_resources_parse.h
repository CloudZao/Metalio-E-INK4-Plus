#pragma once

#include "cloud_screen/push/push_resources_priv.h"
#include "cloud_screen/push/wallpaper_cloud_library.h"

namespace reader {

/** @brief 解析 JSON 无符号 64 位字段 */
bool ParseUint64Field(const cJSON* node, uint64_t* out);
/** @brief 解析推送资源类型 */
PushResourceType ParseResourceType(const char* s);
/** @brief 清洗文件基名 */
std::string SanitizeBasename(const std::string& raw);
/** @brief .ttf/.otf → fonts_ttf（供阅读设置「导入 TTF」）；.ef 等 → fonts */
bool IsTtfOrOtfBasename(const std::string& file);
/** @brief 字体本地路径是否允许 */
bool IsAllowedFontLocalPath(const std::string& path);
/** @brief 带进度读取 HTTP 正文 */
std::string ReadHttpBodyProgress(Http* http, size_t max_bytes, std::string& err_out);
/** @brief 解析单条推送资源 JSON */
bool ParsePushResourceObject(const cJSON* obj, CloudPushResource& out);
/** @brief 转为云书籍任务 */
CloudBookTask ToBookTask(const CloudPushResource& item);
/** @brief 转为云壁纸任务 */
CloudWallpaperTask ToWallpaperTask(const CloudPushResource& item);
/** @brief 下载推送字体到本地 */
bool DownloadFont(const CloudPushResource& item, std::string& err_out,
                  CloudDownloadProgressFn on_progress, void* progress_user, DownloadGate* gate);

}  // namespace reader
