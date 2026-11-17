#pragma once

#include <lvgl.h>

/** @brief 禁止在 LVGL 任务读 NVS：只用内存缓存 / SD 回退；NVS 由后台 hydrate。 */
void RefreshActiveName();
/** @brief 活动壁纸水合完成回调 */
void OnActiveHydrated(void* user);
/** @brief 收集 SD 壁纸文件列表 */
void CollectWallpapers();
/** @brief 取消目录分页并释放句柄 */
void CancelWallpaperDirectory();
/** @brief 检查当前页是否仍在加载 */
bool WallpaperDirectoryLoading();
/** @brief 壁纸列表总页数 */
int Wallpaper_ListPageCount();
/** @brief 钳制壁纸列表页码 */
void Wallpaper_ClampListPage();
/** @brief 先拆掉引用 RasterImage 的控件；缩略图缓存在进程内复用，下次进 app 免白屏 */
void DetachRasterUsers();
/** @brief 网格单元格：封面、启用状态角标和多选勾选框。 */
lv_obj_t* CreateGridCell(lv_obj_t* parent, int index, lv_coord_t cell_w, lv_coord_t cover_h);
/** @brief 按已缓存缩略图刷新一格：有图则填入，未解码显示加载中，解码失败显示失败文案。 */
void PaintCellThumb(int index);
/** @brief 重建列表页（内部） */
void RebuildListPageInternal();
/** @brief 重建壁纸列表当前页 */
void RebuildListPage();
/** @brief 请求重建壁纸列表 */
void RequestWallpaperListRebuild(bool screen_change = false);
/** @brief 下一帧先推白再画出当前画面。缩略图回填不要调。 */
void Wallpaper_RevealAfterWhite();
/** @brief 构建列表主体 */
void BuildListBody(lv_obj_t* scr, lv_coord_t status_h);
/** @brief 作废在飞缩略图解码，丢弃尚未开始的队列。 */
void CancelThumbFill();
/** @brief 后台逐张解码当前页缺的缩略图。 */
void ScheduleThumbFill();
