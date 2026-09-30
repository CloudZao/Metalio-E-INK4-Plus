#pragma once

#include <lvgl.h>

/** @brief 禁止在 LVGL 任务读 NVS：只用内存缓存 / SD 回退；NVS 由后台 hydrate。 */
void RefreshActiveName();
/** @brief 列表排序：当前壁纸优先 */
void SortFilesActiveFirst();
/** @brief 活动壁纸水合完成回调 */
void OnActiveHydrated(void* user);
/** @brief 收集 SD 壁纸文件列表 */
void CollectWallpapers();
/** @brief 壁纸列表总页数 */
int Wallpaper_ListPageCount();
/** @brief 钳制壁纸列表页码 */
void Wallpaper_ClampListPage();
/** @brief 先拆掉引用 RasterImage 的控件；缩略图缓存在进程内复用，下次进 app 免白屏 */
void DetachRasterUsers();
/** @brief 只保留当前页缩略图，翻页时释放其他缓存。 */
void ClearOffPageThumbs();
/** @brief 网格单元格：封面、启用状态角标和多选勾选框。 */
lv_obj_t* CreateGridCell(lv_obj_t* parent, int index, lv_coord_t cell_w, lv_coord_t cover_h);
/** @brief 重建列表页（内部） */
void RebuildListPageInternal(bool schedule_thumbs);
/** @brief 重建壁纸列表当前页 */
void RebuildListPage();
/** @brief 请求重建壁纸列表 */
void RequestWallpaperListRebuild();
/** @brief 构建列表主体 */
void BuildListBody(lv_obj_t* scr, lv_coord_t status_h);
/** @brief 先确保本页缩略图完成，再刷屏，避免首帧白底和重复全刷。 */
void FillPageThumbsSync(int page);
/** @brief 调度缩略图填充 */
void ScheduleThumbFill();
