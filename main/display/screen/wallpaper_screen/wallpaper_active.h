#pragma once

#include <cstddef>
#include <string>

// 壁纸启用状态：NVS 仅保存文件名，UI 读取内存缓存。
// 不要在 LVGL 任务中访问 NVS；缓存由 hydrate / Set* 维护。

/** @brief 当前启用的关机壁纸名；未 hydrate 或文件缺失时返回空。 */
std::string Wallpaper_GetActiveFilename();

/** @brief 当前启用的待机壁纸名；未 hydrate 或文件缺失时返回空。 */
std::string Wallpaper_GetStandbyFilename();

/** @brief 是否为关机壁纸文件名 */
bool Wallpaper_IsActiveFilename(const char* filename);
/** @brief 是否为待机壁纸文件名 */
bool Wallpaper_IsStandbyFilename(const char* filename);

/** @brief 设置关机壁纸；仅在非 LVGL 任务中调用。 */
bool Wallpaper_SetActiveFromFile(const char* path_or_name, std::string& err_out);

/** @brief 设置待机壁纸；仅在非 LVGL 任务中调用。 */
bool Wallpaper_SetStandbyFromFile(const char* path_or_name, std::string& err_out);

/** @brief 清除关机壁纸启用状态，并恢复默认关机图。 */
void Wallpaper_ClearShutdownWallpaper();

/** @brief 清除待机壁纸启用状态，并恢复默认待机页。 */
void Wallpaper_ClearStandbyWallpaper();

/** @brief 删除壁纸时，同步清理所有引用该文件的启用状态。 */
void Wallpaper_ClearActiveIfMatches(const char* filename);

/** @brief 解析当前启用的关机壁纸路径；文件存在时返回 true。 */
bool Wallpaper_TryResolveActiveWallpaperPath(char* out_path, size_t out_len);

/** @brief 解析当前启用的待机壁纸路径；文件存在时返回 true。 */
bool Wallpaper_TryResolveStandbyWallpaperPath(char* out_path, size_t out_len);

using Wallpaper_HydrateDoneFn = void (*)(void* user);
/** @brief 从 NVS 重新灌入关机/待机缓存；已就绪时只回调。 */
void Wallpaper_RequestHydrateFromNvs(Wallpaper_HydrateDoneFn done, void* user);

/** @brief 同步灌入缓存；仅允许在非 LVGL 任务中调用。 */
void Wallpaper_HydrateFromNvsNow();
